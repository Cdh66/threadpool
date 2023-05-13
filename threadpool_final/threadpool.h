#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <iostream>
#include <vector>
#include <queue>
#include <memory>
#include <atomic> // 原子类型，确保线程安全
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>
#include <thread>
#include <future>

const int TASK_MAX_THRESHHOLD = 1024;
const int THREAD_MAX_THRESHHOLD = 10;
const int THREAD_MAX_IDLE_TIME = 10; // 单位：s

// 线程池支持的模式
enum class PoolMode
{
	MODE_FIXED, // 固定数量的线程
	MODE_CACHED, // 线程数量可动态增长
};

// 线程类型
class Thread
{
public:
	// 线程函数对象类型
	using ThreadFunc = std::function<void(int)>;

	Thread(ThreadFunc func)
		:func_(func)
		, threadId_(generateId_++)
	{}
	~Thread() = default;

	// 启动线程
	void start()
	{
		// 创建一个线程来执行线程函数
		std::thread t(func_, threadId_); // C++11 线程对象t 线程函数func_
		t.detach(); // 设置分离线程
	}

	// 获取线程id
	int getId() const
	{
		return threadId_;
	}
private:
	ThreadFunc func_;
	static int generateId_;
	int threadId_; // 保存线程id

};

int Thread::generateId_ = 0;


// 线程池类型
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

		// 等待线程池里所有线程返回  有两种状态：阻塞 | 执行任务中
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		notEmpty_.notify_all(); // 在获得锁后通知
		exitCond_.wait(lock, [&]()->bool { return threads_.size() == 0; });
	}

	// 开启线程池
	void start(int initThreadSize = std::thread::hardware_concurrency())
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

	// 设置线程池工作模式
	void setMode(PoolMode mode)
	{
		if (checkRunningState())
			return;
		poolMode_ = mode;
	}

	// 设置task任务队列上限阈值
	void setTaskQueMaxThreshHold(int threshhold)
	{
		if (checkRunningState())
			return;
		taskQueMaxThreshHold_ = threshhold;
	}

	// 设置线程池cached模式下线程的阈值
	void setThreadSizeThreshHold(int threshhold)
	{
		if (checkRunningState())
			return;
		if (poolMode_ == PoolMode::MODE_CACHED)
			threadSizeThreshHold_ = threshhold;
	}

	// 给线程池提交任务
	// 使用可变参模板编程，让submitTask可以接收任意任务函数和任意数量的参数
	template<typename Func, typename... Args>
	auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))> // 推导出函数的返回值！！！
	{
		// 打包任务，放入任务队列
		using RType = decltype(func(args...));
		auto task = std::make_shared<std::packaged_task<RType()>>(
			std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
		std::future<RType> result = task->get_future();

		// 获取锁
		std::unique_lock<std::mutex> lock(taskQueMtx_);

		// 用户提交任务，最长不能阻塞超过1s，否则判断提交任务失败，返回
		if (!notFull_.wait_for(lock, std::chrono::seconds(1),
			[&]()->bool {return taskQue_.size() < (size_t)taskQueMaxThreshHold_; })) {
			// 表示notFull_等待1s后，条件依然没有满足
			std::cerr << "task queue is full, submit task fail." << std::endl;
			auto task = std::make_shared<std::packaged_task<RType()>>(
				[]()->RType { return RType(); }); // 返回返回值类型对应的零值
			(*task)();
			return task->get_future();
		}

		// 若有空余，将任务放入队列中
		//taskQue_.emplace(sp);
		taskQue_.emplace([task]() {
			(*task)();
			});
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
		return result;
	}

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;
private:
	std::unordered_map<int, std::unique_ptr<Thread>> threads_;

	size_t initThreadSize_; // 初始线程数量
	std::atomic_int curThreadSize_; // 记录当前线程池里线程的总数量
	std::atomic_int idleThreadSize_; // 记录空闲线程的数量
	int threadSizeThreshHold_; // 线程数量上限阈值

	using Task = std::function<void()>;
	std::queue<Task> taskQue_; // 任务队列
	std::atomic_uint taskSize_; // 任务的数量
	int taskQueMaxThreshHold_; // 任务队列数量上限阈值

	std::mutex taskQueMtx_; // 保证任务队列的线程安全
	std::condition_variable notFull_; // 表示任务队列不满
	std::condition_variable notEmpty_; // 表示任务队列不空
	std::condition_variable exitCond_; // 等待线程资源全部回收

	PoolMode poolMode_; // 当前线程池的工作模式

	std::atomic_bool isPoolRunning_; // 记录当前线程池的工作状态

	// 定义线程函数
	void threadFunc(int threadid)
	{
		auto lastTime = std::chrono::high_resolution_clock().now();

		// 所有任务执行完了之后，线程池再析构
		for (;;) {
			Task task;
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
				task(); // 执行function<void()>
			}
			idleThreadSize_++;
			lastTime = std::chrono::high_resolution_clock().now(); // 更新线程执行完任务的时间
		}

		return;
	}

	// 检查pool的运行状态
	bool checkRunningState() const
	{
		return isPoolRunning_;
	}
};


#endif // !THREADPOOL_H

