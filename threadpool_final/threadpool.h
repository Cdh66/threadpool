#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <iostream>
#include <vector>
#include <queue>
#include <memory>
#include <atomic> // ԭ�����ͣ�ȷ���̰߳�ȫ
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>
#include <thread>
#include <future>

const int TASK_MAX_THRESHHOLD = 1024;
const int THREAD_MAX_THRESHHOLD = 10;
const int THREAD_MAX_IDLE_TIME = 10; // ��λ��s

// �̳߳�֧�ֵ�ģʽ
enum class PoolMode
{
	MODE_FIXED, // �̶��������߳�
	MODE_CACHED, // �߳������ɶ�̬����
};

// �߳�����
class Thread
{
public:
	// �̺߳�����������
	using ThreadFunc = std::function<void(int)>;

	Thread(ThreadFunc func)
		:func_(func)
		, threadId_(generateId_++)
	{}
	~Thread() = default;

	// �����߳�
	void start()
	{
		// ����һ���߳���ִ���̺߳���
		std::thread t(func_, threadId_); // C++11 �̶߳���t �̺߳���func_
		t.detach(); // ���÷����߳�
	}

	// ��ȡ�߳�id
	int getId() const
	{
		return threadId_;
	}
private:
	ThreadFunc func_;
	static int generateId_;
	int threadId_; // �����߳�id

};

int Thread::generateId_ = 0;


// �̳߳�����
class ThreadPool
{
public:
	ThreadPool()
		: initThreadSize_(4)
		, taskSize_(0)
		, taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
		, threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
		, poolMode_(PoolMode::MODE_FIXED)
		, isPoolRunning_(false)
		, idleThreadSize_(0)
		, curThreadSize_(0)
	{}

	~ThreadPool()
	{
		isPoolRunning_ = false;

		// �ȴ��̳߳��������̷߳���  ������״̬������ | ִ��������
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		notEmpty_.notify_all(); // �ڻ������֪ͨ
		exitCond_.wait(lock, [&]()->bool { return threads_.size() == 0; });
	}

	// �����̳߳�
	void start(int initThreadSize = std::thread::hardware_concurrency())
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

	// �����̳߳ع���ģʽ
	void setMode(PoolMode mode)
	{
		if (checkRunningState())
			return;
		poolMode_ = mode;
	}

	// ����task�������������ֵ
	void setTaskQueMaxThreshHold(int threshhold)
	{
		if (checkRunningState())
			return;
		taskQueMaxThreshHold_ = threshhold;
	}

	// �����̳߳�cachedģʽ���̵߳���ֵ
	void setThreadSizeThreshHold(int threshhold)
	{
		if (checkRunningState())
			return;
		if (poolMode_ == PoolMode::MODE_CACHED)
			threadSizeThreshHold_ = threshhold;
	}

	// ���̳߳��ύ����
	// ʹ�ÿɱ��ģ���̣���submitTask���Խ������������������������Ĳ���
	template<typename Func, typename... Args>
	auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))> // �Ƶ��������ķ���ֵ������
	{
		// ������񣬷����������
		using RType = decltype(func(args...));
		auto task = std::make_shared<std::packaged_task<RType()>>(
			std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
		std::future<RType> result = task->get_future();

		// ��ȡ��
		std::unique_lock<std::mutex> lock(taskQueMtx_);

		// �û��ύ�����������������1s�������ж��ύ����ʧ�ܣ�����
		if (!notFull_.wait_for(lock, std::chrono::seconds(1),
			[&]()->bool {return taskQue_.size() < (size_t)taskQueMaxThreshHold_; })) {
			// ��ʾnotFull_�ȴ�1s��������Ȼû������
			std::cerr << "task queue is full, submit task fail." << std::endl;
			auto task = std::make_shared<std::packaged_task<RType()>>(
				[]()->RType { return RType(); }); // ���ط���ֵ���Ͷ�Ӧ����ֵ
			(*task)();
			return task->get_future();
		}

		// ���п��࣬��������������
		//taskQue_.emplace(sp);
		taskQue_.emplace([task]() {
			(*task)();
			});
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
		return result;
	}

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;
private:
	std::unordered_map<int, std::unique_ptr<Thread>> threads_;

	size_t initThreadSize_; // ��ʼ�߳�����
	std::atomic_int curThreadSize_; // ��¼��ǰ�̳߳����̵߳�������
	std::atomic_int idleThreadSize_; // ��¼�����̵߳�����
	int threadSizeThreshHold_; // �߳�����������ֵ

	using Task = std::function<void()>;
	std::queue<Task> taskQue_; // �������
	std::atomic_uint taskSize_; // ���������
	int taskQueMaxThreshHold_; // �����������������ֵ

	std::mutex taskQueMtx_; // ��֤������е��̰߳�ȫ
	std::condition_variable notFull_; // ��ʾ������в���
	std::condition_variable notEmpty_; // ��ʾ������в���
	std::condition_variable exitCond_; // �ȴ��߳���Դȫ������

	PoolMode poolMode_; // ��ǰ�̳߳صĹ���ģʽ

	std::atomic_bool isPoolRunning_; // ��¼��ǰ�̳߳صĹ���״̬

	// �����̺߳���
	void threadFunc(int threadid)
	{
		auto lastTime = std::chrono::high_resolution_clock().now();

		// ��������ִ������֮���̳߳�������
		for (;;) {
			Task task;
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
				task(); // ִ��function<void()>
			}
			idleThreadSize_++;
			lastTime = std::chrono::high_resolution_clock().now(); // �����߳�ִ���������ʱ��
		}

		return;
	}

	// ���pool������״̬
	bool checkRunningState() const
	{
		return isPoolRunning_;
	}
};


#endif // !THREADPOOL_H

