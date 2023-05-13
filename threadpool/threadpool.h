#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <queue>
#include <memory>
#include <atomic> // ԭ�����ͣ�ȷ���̰߳�ȫ
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>

// Any���ͣ����Խ����������ݵ�����
// ģ�庯���������Ͷ��嶼��Ҫ����ͷ�ļ��У�����
class Any
{
public:
	Any() = default;
	~Any() = default;
	Any(const Any&) = delete;
	Any& operator=(const Any&) = delete;
	Any(Any&&) = default;
	Any& operator=(Any&&) = default;
	
	// ������캯��������Any����������������
	template<typename T>
	Any(T data) : base_(std::make_unique<Derive<T>>(data))
	{}

	// ���������Any������洢��data������ȡ����
	template<typename T>
	T cast_()
	{
		// ����ָ��תΪ������ָ��
		Derive<T>* pd = dynamic_cast<Derive<T>*>(base_.get());
		if (pd == nullptr) {
			throw "type is unmatch!";
		}
		return pd->data_;
	}
private:
	// ��������
	class Base
	{
	public:
		virtual ~Base() = default;
	};

	// ����������
	template<typename T>
	class Derive : public Base
	{
	public:
		Derive(T data) : data_(data)
		{}
		T data_; // �������������������
	};

	std::unique_ptr<Base> base_;
};

// ʵ��һ���ź�����
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

	// ��ȡһ���ź�����Դ
	void wait()
	{
		if (isExit_)
			return;
		std::unique_lock<std::mutex> lock(mtx_);
		// �ȴ��ź�������Դ��û����Դ�Ļ�����������ǰ�߳�
		cond_.wait(lock, [&]()->bool { return resLimit_ > 0; });
		resLimit_--;
	}

	// ����һ���ź�����Դ
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

// Task���͵�ǰ������
class Task;

// ʵ�ֽ����ύ���̳߳ص�task����ִ����ɺ�ķ���ֵ����Result
class Result
{
public:
	Result(std::shared_ptr<Task> task, bool isVaild = true);
	~Result() = default;

	// setVal ��������ȡ����ִ����ķ���ֵ
	void setVal(Any any);

	// get�������û��������������ȡtask�ķ���ֵ
	Any get();

private:
	Any any_; // �洢����ķ���ֵ
	Semaphore sem_; // �߳�ͨ���ź���
	std::shared_ptr<Task> task_; // ָ���Ӧ��ȡ����ֵ���������
	std::atomic_bool isVaild_; // ���񷵻�ֵ�Ƿ���Ч
};

// ����������
class Task
{
public:
	Task();
	~Task() = default;

	void exec();
	void setResult(Result* res);
	// �û����Զ��������������ͣ���Task�̳У���дrun������ʵ���Զ���������
	virtual Any run() = 0;

private:
	Result* result_; // Result������������� >> Task�������������
};

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

	Thread(ThreadFunc func);
	~Thread();

	// �����߳�
	void start();

	// ��ȡ�߳�id
	int getId() const;
private:
	ThreadFunc func_;
	static int generateId_;
	int threadId_; // �����߳�id

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
// �̳߳�����
class ThreadPool
{
public:
	ThreadPool();
	~ThreadPool();

	// �����̳߳�
	void start(int initThreadSize = std::thread::hardware_concurrency());

	// �����̳߳ع���ģʽ
	void setMode(PoolMode mode);

	// ����task�������������ֵ
	void setTaskQueMaxThreshHold(int threshhold);

	// �����̳߳�cachedģʽ���̵߳���ֵ
	void setThreadSizeThreshHold(int threshhold);

	// ���̳߳��ύ����
	Result submitTask(std::shared_ptr<Task> sp);

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;
private:
	//std::vector<std::unique_ptr<Thread>> threads_; // �߳��б�
	std::unordered_map<int, std::unique_ptr<Thread>> threads_;

	size_t initThreadSize_; // ��ʼ�߳�����
	std::atomic_int curThreadSize_; // ��¼��ǰ�̳߳����̵߳�������
	std::atomic_int idleThreadSize_; // ��¼�����̵߳�����
	int threadSizeThreshHold_; // �߳�����������ֵ

	// ��ֹ�û�����һ����ʱ�����ָ�룬�˴�������ָ��
	std::queue<std::shared_ptr<Task>> taskQue_; // �������
	std::atomic_uint taskSize_; // ���������
	int taskQueMaxThreshHold_; // �����������������ֵ
	
	std::mutex taskQueMtx_; // ��֤������е��̰߳�ȫ
	std::condition_variable notFull_; // ��ʾ������в���
	std::condition_variable notEmpty_; // ��ʾ������в���
	std::condition_variable exitCond_; // �ȴ��߳���Դȫ������

	PoolMode poolMode_; // ��ǰ�̳߳صĹ���ģʽ

	std::atomic_bool isPoolRunning_; // ��¼��ǰ�̳߳صĹ���״̬

	// �����̺߳���
	void threadFunc(int threadid);

	// ���pool������״̬
	bool checkRunningState() const;
};

#endif // !THREADPOOL_H

