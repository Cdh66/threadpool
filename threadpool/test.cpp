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

	Any run() // run�����������̳߳ط�����߳���ִ��
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
		// �û��Լ������̳߳صĹ���ģʽ
		pool.setMode(PoolMode::MODE_CACHED);
		// ��ʼ�����̳߳�
		pool.start(4);

		Result res1 = pool.submitTask(std::make_shared<MyTask>(1, 1000));

		int sum1 = res1.get().cast_<int>();

		std::cout << sum1 << std::endl;
	} // ���������̳߳�����

	getchar();

	return 0;
}