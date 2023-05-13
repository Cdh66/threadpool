#include "threadpool.h"
#include <functional>
#include <thread>
#include <iostream>
#include <chrono>

const int TASK_MAX_THRESHHOLD = 1024;
const int THREAD_MAX_THRESHHOLD = 10;
const int THREAD_MAX_IDLE_TIME = 10; // ��λ��s

ThreadPool::ThreadPool()
	: initThreadSize_(4)
	, taskSize_(0)
	, taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
	, threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
	, poolMode_(PoolMode::MODE_FIXED)
	, isPoolRunning_(false)
	, idleThreadSize_(0)
	, curThreadSize_(0)
{}

ThreadPool::~ThreadPool()
{
	isPoolRunning_ = false;

	// �ȴ��̳߳��������̷߳���  ������״̬������ | ִ��������
	std::unique_lock<std::mutex> lock(taskQueMtx_);
	notEmpty_.notify_all(); // �ڻ������֪ͨ
	exitCond_.wait(lock, [&]()->bool { return threads_.size() == 0; });
}

// �����̳߳ع���ģʽ
void ThreadPool::setMode(PoolMode mode)
{
	if (checkRunningState())
		return;
	poolMode_ = mode;
}

// ����task�������������ֵ
void ThreadPool::setTaskQueMaxThreshHold(int threshhold)
{
	if (checkRunningState())
		return;
	taskQueMaxThreshHold_ = threshhold;
}

// �����̳߳�cachedģʽ���̵߳���ֵ
void ThreadPool::setThreadSizeThreshHold(int threshhold)
{
	if (checkRunningState())
		return;
	if (poolMode_ == PoolMode::MODE_CACHED)
		threadSizeThreshHold_ = threshhold;
}

// ���̳߳��ύ���� ���û����ã���������
Result ThreadPool::submitTask(std::shared_ptr<Task> sp)
{
	// ��ȡ��
	std::unique_lock<std::mutex> lock(taskQueMtx_);

	// �߳�ͨ�� �ȴ���������п���
	// �û��ύ�����������������1s�������ж��ύ����ʧ�ܣ�����
	/*
	while (taskQue_.size() == taskQueMaxThreshHold_) {
		notFull_.wait(lock);
	}*/
	if (!notFull_.wait_for(lock, std::chrono::seconds(1),
		[&]()->bool {return taskQue_.size() < (size_t)taskQueMaxThreshHold_; })) {
		// ��ʾnotFull_�ȴ�1s��������Ȼû������
		std::cerr << "task queue is full, submit task fail." << std::endl;
		return Result(sp, false);
	}

	// ���п��࣬��������������
	taskQue_.emplace(sp);
	taskSize_++;

	// ������в��գ�֪ͨnotEmpty_
	notEmpty_.notify_all();

	// cachedģʽ��������С���������������ȽϽ���
	// ��Ҫ��������������Ϳ����߳��������ж��Ƿ���Ҫ����µ��߳�
	if (poolMode_ == PoolMode::MODE_CACHED && taskSize_ > idleThreadSize_ && curThreadSize_ < threadSizeThreshHold_) {
		std::cout << ">>>create new thread" << std::endl;
		// �����µ�thread�̶߳���
		auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
		int threadId = ptr->getId();
		threads_.emplace(threadId, std::move(ptr));
		//threads_.emplace_back(std::move(ptr)); // unique_ptr û����ֵ�����븳ֵ������
		threads_[threadId]->start(); // �����߳�
		curThreadSize_++;
		idleThreadSize_++;
	}

	// ���������Result����
	return Result(sp);
}

// �����̳߳�
void ThreadPool::start(int initThreadSize)
{
	// �����̳߳ص�����״̬
	isPoolRunning_ = true;

	// ��¼��ʼ�̸߳���
	initThreadSize_ = initThreadSize;
	curThreadSize_ = initThreadSize;

	// �����̶߳���
	for (size_t i = 0; i < initThreadSize_; i++) {
		// ����thread�̶߳����ʱ�򣬰��̺߳�������thread�̶߳���
		auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
		int threadId = ptr->getId();
		threads_.emplace(threadId, std::move(ptr));
		//threads_.emplace_back(std::move(ptr)); // unique_ptr û����ֵ�����븳ֵ������
	}

	// ���������߳�
	for (size_t i = 0; i < initThreadSize_; i++) {
		threads_[i]->start();
		idleThreadSize_++; // ��¼��ʼ�����̵߳�����
	}
}

// �����̺߳���    ��������
void ThreadPool::threadFunc(int threadid)
{
	auto lastTime = std::chrono::high_resolution_clock().now();

	// ��������ִ������֮���̳߳�������
	for (;;) {
		std::shared_ptr<Task> task;
		{
			// �Ȼ�ȡ��
			std::unique_lock<std::mutex> lock(taskQueMtx_);

			std::cout << "tid: " << std::this_thread::get_id() << " ���Ի�ȡ����..." << std::endl;

			// cachedģʽ�£�����ʱ�䳬��60s�ĳ���initThreadSize_�Ķ����߳���Ҫ����
			// ��ǰʱ�� - ��һ���߳�ִ�е�ʱ�� > 60s
			
			// �� + ˫���ж�
			while (taskQue_.size() == 0) {
				// �̳߳ؽ���
				if (!isPoolRunning_) {
					// �̳߳ؽ����������߳���Դ
					threads_.erase(threadid);
					curThreadSize_--;
					idleThreadSize_--;

					std::cout << "threadid: " << std::this_thread::get_id() << "exit!" << std::endl;
					exitCond_.notify_all();
					return; // �����̺߳������ǽ�����ǰ�߳���
				}

				if (poolMode_ == PoolMode::MODE_CACHED) {
					// ����������ʱ����
					if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1))) {
						auto now = std::chrono::high_resolution_clock().now();
						auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
						if (dur.count() >= THREAD_MAX_IDLE_TIME
							&& curThreadSize_ > initThreadSize_) {
							// ��ʼ���յ�ǰ�߳�
							// ��¼�߳����������ֵ�޸�
							// ���̶߳�����߳��б���ɾ��
							threads_.erase(threadid);
							curThreadSize_--;
							idleThreadSize_--;

							std::cout << "threadid: " << std::this_thread::get_id() << "exit!" << std::endl;
							return; // �����̺߳������ǽ�����ǰ�߳���
						}
					}
				}
				else {
					// �ȴ�notEmpty����
					notEmpty_.wait(lock);
				}

				/*if (!isPoolRunning_) {
					threads_.erase(threadid);
					curThreadSize_--;
					idleThreadSize_--;

					std::cout << "threadid: " << std::this_thread::get_id() << "exit!" << std::endl;
					exitCond_.notify_all();
					return;
				}*/
			}
			

			idleThreadSize_--;

			std::cout << "tid: " << std::this_thread::get_id() << " ��ȡ����ɹ�..." << std::endl;

			// �����������ȡһ���������
			task = taskQue_.front();
			taskQue_.pop();
			taskSize_--;

			// �����Ȼ��ʣ�����񣬼���֪ͨ�����߳�ִ������
			if (taskQue_.size() > 0) {
				notEmpty_.notify_all();
			}

			// ֪ͨnotFull_
			notFull_.notify_all();
		}// �����ͷ�

		// ��ǰ�̸߳���ִ���������
		if (task != nullptr) {
			//task->run(); // ִ�����񣬽�����ķ���ֵͨ��setVal������Result
			task->exec();
		}
		idleThreadSize_++;
		lastTime = std::chrono::high_resolution_clock().now(); // �����߳�ִ���������ʱ��
	}

	
	return;
}

bool ThreadPool::checkRunningState() const
{
	return isPoolRunning_;
}

////////////////////////////////// �̷߳���ʵ��
int Thread::generateId_ = 0;

Thread::Thread(ThreadFunc func)
	:func_(func)
	, threadId_(generateId_++)
{}

Thread::~Thread()
{}

// �����߳�
void Thread::start()
{
	// ����һ���߳���ִ���̺߳���
	std::thread t(func_, threadId_); // C++11 �̶߳���t �̺߳���func_
	t.detach(); // ���÷����߳�
}

int Thread::getId() const
{
	return threadId_;
}

///////////////////////////////// Task����ʵ��
Task::Task()
	: result_(nullptr)
{}

void Task::exec()
{
	if (result_ != nullptr) {
		result_->setVal(run()); // ���﷢����̬�ĵ���
	}
}

void Task::setResult(Result* res)
{
	result_ = res;
}

///////////////////////////////// Result������ʵ��
Result::Result(std::shared_ptr<Task> task, bool isVaild)
	: task_(task), isVaild_(isVaild)
{
	task_->setResult(this);
}

Any Result::get()
{
	if (!isVaild_) {
		return "";
	}

	sem_.wait(); // task�������û��ִ���꣬����������û����߳�
	return std::move(any_);
}

void Result::setVal(Any any)
{
	// �洢task�ķ���ֵ
	this->any_ = std::move(any);
	sem_.post(); // �Ѿ���ȡ������ķ���ֵ�������ź�����Դ
}