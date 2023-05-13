#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <queue>
#include <memory>
#include <atomic> // 原子类型，确保线程安全
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>

// Any类型：可以接收任意数据的类型
// 模板函数的声明和定义都需要放在头文件中！！！
class Any
{
public:
	Any() = default;
	~Any() = default;
	Any(const Any&) = delete;
	Any& operator=(const Any&) = delete;
	Any(Any&&) = default;
	Any& operator=(Any&&) = default;
	
	// 这个构造函数可以让Any接收任意其他类型
	template<typename T>
	Any(T data) : base_(std::make_unique<Derive<T>>(data))
	{}

	// 这个方法把Any对象里存储的data数据提取出来
	template<typename T>
	T cast_()
	{
		// 基类指针转为派生类指针
		Derive<T>* pd = dynamic_cast<Derive<T>*>(base_.get());
		if (pd == nullptr) {
			throw "type is unmatch!";
		}
		return pd->data_;
	}
private:
	// 基类类型
	class Base
	{
	public:
		virtual ~Base() = default;
	};

	// 派生类类型
	template<typename T>
	class Derive : public Base
	{
	public:
		Derive(T data) : data_(data)
		{}
		T data_; // 保存了任意的其他类型
	};

	std::unique_ptr<Base> base_;
};

// 实现一个信号量类
class Semaphore
{
public:
	Semaphore(int limit = 0)
		:resLimit_(limit)
		, isExit_(false)
	{}
	~Semaphore()
	{
		isExit_ = true;
	}

	// 获取一个信号量资源
	void wait()
	{
		if (isExit_)
			return;
		std::unique_lock<std::mutex> lock(mtx_);
		// 等待信号量有资源，没有资源的话，会阻塞当前线程
		cond_.wait(lock, [&]()->bool { return resLimit_ > 0; });
		resLimit_--;
	}

	// 增加一个信号量资源
	void post()
	{
		if (isExit_)
			return;
		std::unique_lock<std::mutex> lock(mtx_);
		resLimit_++;
		cond_.notify_all();
	}
private:
	std::atomic_bool isExit_;
	int resLimit_;
	std::mutex mtx_;
	std::condition_variable cond_;
};

// Task类型的前置声明
class Task;

// 实现接收提交到线程池的task任务执行完成后的返回值类型Result
class Result
{
public:
	Result(std::shared_ptr<Task> task, bool isVaild = true);
	~Result() = default;

	// setVal 方法，获取任务执行完的返回值
	void setVal(Any any);

	// get方法，用户调用这个方法获取task的返回值
	Any get();

private:
	Any any_; // 存储任务的返回值
	Semaphore sem_; // 线程通信信号量
	std::shared_ptr<Task> task_; // 指向对应获取返回值的任务对象
	std::atomic_bool isVaild_; // 任务返回值是否有效
};

// 任务抽象基类
class Task
{
public:
	Task();
	~Task() = default;

	void exec();
	void setResult(Result* res);
	// 用户可自定义任意任务类型，从Task继承，重写run方法，实现自定义任务处理
	virtual Any run() = 0;

private:
	Result* result_; // Result对象的生命周期 >> Task对象的生命周期
};

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

	Thread(ThreadFunc func);
	~Thread();

	// 启动线程
	void start();

	// 获取线程id
	int getId() const;
private:
	ThreadFunc func_;
	static int generateId_;
	int threadId_; // 保存线程id

};
/*
example:
ThreadPool pool;
pool.start(4);

class MyTask : public Task
{
public:
	void run() {...}
};

pool.submitTask(std::make_shared<MyTask>());
*/
// 线程池类型
class ThreadPool
{
public:
	ThreadPool();
	~ThreadPool();

	// 开启线程池
	void start(int initThreadSize = std::thread::hardware_concurrency());

	// 设置线程池工作模式
	void setMode(PoolMode mode);

	// 设置task任务队列上限阈值
	void setTaskQueMaxThreshHold(int threshhold);

	// 设置线程池cached模式下线程的阈值
	void setThreadSizeThreshHold(int threshhold);

	// 给线程池提交任务
	Result submitTask(std::shared_ptr<Task> sp);

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;
private:
	//std::vector<std::unique_ptr<Thread>> threads_; // 线程列表
	std::unordered_map<int, std::unique_ptr<Thread>> threads_;

	size_t initThreadSize_; // 初始线程数量
	std::atomic_int curThreadSize_; // 记录当前线程池里线程的总数量
	std::atomic_int idleThreadSize_; // 记录空闲线程的数量
	int threadSizeThreshHold_; // 线程数量上限阈值

	// 防止用户传入一个临时对象的指针，此处不用裸指针
	std::queue<std::shared_ptr<Task>> taskQue_; // 任务队列
	std::atomic_uint taskSize_; // 任务的数量
	int taskQueMaxThreshHold_; // 任务队列数量上限阈值
	
	std::mutex taskQueMtx_; // 保证任务队列的线程安全
	std::condition_variable notFull_; // 表示任务队列不满
	std::condition_variable notEmpty_; // 表示任务队列不空
	std::condition_variable exitCond_; // 等待线程资源全部回收

	PoolMode poolMode_; // 当前线程池的工作模式

	std::atomic_bool isPoolRunning_; // 记录当前线程池的工作状态

	// 定义线程函数
	void threadFunc(int threadid);

	// 检查pool的运行状态
	bool checkRunningState() const;
};

#endif // !THREADPOOL_H

