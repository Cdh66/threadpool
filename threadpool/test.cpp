#include <iostream>
#include <chrono>
#include <thread>
#include "threadpool.h"
using uLong = unsigned long long;

class MyTask : public Task
{
public:
	MyTask(int begin, int end)
		: begin_(begin), end_(end)
	{}

	Any run() // run方法最终在线程池分配的线程中执行
	{
		std::cout << "tid: " << std::this_thread::get_id() << " begin!" << std::endl;
		std::this_thread::sleep_for(std::chrono::seconds(3));
		int sum = 0;
		for (int i = begin_; i <= end_; i++)
			sum += i;
		std::cout << "tid: " << std::this_thread::get_id() << " end!" << std::endl;
		return sum;
	}

private:
	int begin_;
	int end_;
};

int main()
{
	 

	{
		ThreadPool pool;
		// 用户自己设置线程池的工作模式
		pool.setMode(PoolMode::MODE_CACHED);
		// 开始启动线程池
		pool.start(4);

		Result res1 = pool.submitTask(std::make_shared<MyTask>(1, 1000));

		int sum1 = res1.get().cast_<int>();

		std::cout << sum1 << std::endl;
	} // 出作用域线程池析构

	getchar();

	return 0;
}