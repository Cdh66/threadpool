#include "threadpool.h"
#include <functional>
#include <thread>
#include <iostream>
#include <chrono>

const int TASK_MAX_THRESHHOLD = 1024;
const int THREAD_MAX_THRESHHOLD = 10;
const int THREAD_MAX_IDLE_TIME = 10; // 单位：s

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

	// 等待线程池里所有线程返回  有两种状态：阻塞 | 执行任务中
	std::unique_lock<std::mutex> lock(taskQueMtx_);
	notEmpty_.notify_all(); // 在获得锁后通知
	exitCond_.wait(lock, [&]()->bool { return threads_.size() == 0; });
}

// 设置线程池工作模式
void ThreadPool::setMode(PoolMode mode)
{
	if (checkRunningState())
		return;
	poolMode_ = mode;
}

// 设置task任务队列上限阈值
void ThreadPool::setTaskQueMaxThreshHold(int threshhold)
{
	if (checkRunningState())
		return;
	taskQueMaxThreshHold_ = threshhold;
}

// 设置线程池cached模式下线程的阈值
void ThreadPool::setThreadSizeThreshHold(int threshhold)
{
	if (checkRunningState())
		return;
	if (poolMode_ == PoolMode::MODE_CACHED)
		threadSizeThreshHold_ = threshhold;
}

// 给线程池提交任务 给用户调用，生产任务
Result ThreadPool::submitTask(std::shared_ptr<Task> sp)
{
	// 获取锁
	std::unique_lock<std::mutex> lock(taskQueMtx_);

	// 线程通信 等待任务队列有空余
	// 用户提交任务，最长不能阻塞超过1s，否则判断提交任务失败，返回
	/*
	while (taskQue_.size() == taskQueMaxThreshHold_) {
		notFull_.wait(lock);
	}*/
	if (!notFull_.wait_for(lock, std::chrono::seconds(1),
		[&]()->bool {return taskQue_.size() < (size_t)taskQueMaxThreshHold_; })) {
		// 表示notFull_等待1s后，条件依然没有满足
		std::cerr << "task queue is full, submit task fail." << std::endl;
		return Result(sp, false);
	}

	// 若有空余，将任务放入队列中
	taskQue_.emplace(sp);
	taskSize_++;

	// 任务队列不空，通知notEmpty_
	notEmpty_.notify_all();

	// cached模式，场景：小而快的任务，任务处理比较紧急
	// 需要根据任务的数量和空闲线程数量，判断是否需要添加新的线程
	if (poolMode_ == PoolMode::MODE_CACHED && taskSize_ > idleThreadSize_ && curThreadSize_ < threadSizeThreshHold_) {
		std::cout << ">>>create new thread" << std::endl;
		// 创建新的thread线程对象
		auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
		int threadId = ptr->getId();
		threads_.emplace(threadId, std::move(ptr));
		//threads_.emplace_back(std::move(ptr)); // unique_ptr 没有左值引用与赋值！！！
		threads_[threadId]->start(); // 启动线程
		curThreadSize_++;
		idleThreadSize_++;
	}

	// 返回任务的Result对象
	return Result(sp);
}

// 开启线程池
void ThreadPool::start(int initThreadSize)
{
	// 设置线程池的运行状态
	isPoolRunning_ = true;

	// 记录初始线程个数
	initThreadSize_ = initThreadSize;
	curThreadSize_ = initThreadSize;

	// 创建线程对象
	for (size_t i = 0; i < initThreadSize_; i++) {
		// 创建thread线程对象的时候，把线程函数给到thread线程对象
		auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
		int threadId = ptr->getId();
		threads_.emplace(threadId, std::move(ptr));
		//threads_.emplace_back(std::move(ptr)); // unique_ptr 没有左值引用与赋值！！！
	}

	// 启动所有线程
	for (size_t i = 0; i < initThreadSize_; i++) {
		threads_[i]->start();
		idleThreadSize_++; // 记录初始空闲线程的数量
	}
}

// 定义线程函数    消费任务
void ThreadPool::threadFunc(int threadid)
{
	auto lastTime = std::chrono::high_resolution_clock().now();

	// 所有任务执行完了之后，线程池再析构
	for (;;) {
		std::shared_ptr<Task> task;
		{
			// 先获取锁
			std::unique_lock<std::mutex> lock(taskQueMtx_);

			std::cout << "tid: " << std::this_thread::get_id() << " 尝试获取任务..." << std::endl;

			// cached模式下，空闲时间超过60s的超过initThreadSize_的多余线程需要回收
			// 当前时间 - 上一次线程执行的时间 > 60s
			
			// 锁 + 双重判断
			while (taskQue_.size() == 0) {
				// 线程池结束
				if (!isPoolRunning_) {
					// 线程池结束，回收线程资源
					threads_.erase(threadid);
					curThreadSize_--;
					idleThreadSize_--;

					std::cout << "threadid: " << std::this_thread::get_id() << "exit!" << std::endl;
					exitCond_.notify_all();
					return; // 结束线程函数就是结束当前线程了
				}

				if (poolMode_ == PoolMode::MODE_CACHED) {
					// 条件变量超时返回
					if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1))) {
						auto now = std::chrono::high_resolution_clock().now();
						auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
						if (dur.count() >= THREAD_MAX_IDLE_TIME
							&& curThreadSize_ > initThreadSize_) {
							// 开始回收当前线程
							// 记录线程数量的相关值修改
							// 把线程对象从线程列表中删除
							threads_.erase(threadid);
							curThreadSize_--;
							idleThreadSize_--;

							std::cout << "threadid: " << std::this_thread::get_id() << "exit!" << std::endl;
							return; // 结束线程函数就是结束当前线程了
						}
					}
				}
				else {
					// 等待notEmpty条件
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

			std::cout << "tid: " << std::this_thread::get_id() << " 获取任务成功..." << std::endl;

			// 从任务队列中取一个任务出来
			task = taskQue_.front();
			taskQue_.pop();
			taskSize_--;

			// 如果依然有剩余任务，继续通知其他线程执行任务
			if (taskQue_.size() > 0) {
				notEmpty_.notify_all();
			}

			// 通知notFull_
			notFull_.notify_all();
		}// 将锁释放

		// 当前线程负责执行这个任务
		if (task != nullptr) {
			//task->run(); // 执行任务，将任务的返回值通过setVal方法给Result
			task->exec();
		}
		idleThreadSize_++;
		lastTime = std::chrono::high_resolution_clock().now(); // 更新线程执行完任务的时间
	}

	
	return;
}

bool ThreadPool::checkRunningState() const
{
	return isPoolRunning_;
}

////////////////////////////////// 线程方法实现
int Thread::generateId_ = 0;

Thread::Thread(ThreadFunc func)
	:func_(func)
	, threadId_(generateId_++)
{}

Thread::~Thread()
{}

// 启动线程
void Thread::start()
{
	// 创建一个线程来执行线程函数
	std::thread t(func_, threadId_); // C++11 线程对象t 线程函数func_
	t.detach(); // 设置分离线程
}

int Thread::getId() const
{
	return threadId_;
}

///////////////////////////////// Task方法实现
Task::Task()
	: result_(nullptr)
{}

void Task::exec()
{
	if (result_ != nullptr) {
		result_->setVal(run()); // 这里发生多态的调用
	}
}

void Task::setResult(Result* res)
{
	result_ = res;
}

///////////////////////////////// Result方法的实现
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

	sem_.wait(); // task任务如果没有执行完，这里会阻塞用户的线程
	return std::move(any_);
}

void Result::setVal(Any any)
{
	// 存储task的返回值
	this->any_ = std::move(any);
	sem_.post(); // 已经获取了任务的返回值，增加信号量资源
}