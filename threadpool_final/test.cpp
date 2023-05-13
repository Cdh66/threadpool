#include <iostream>
#include <future>
#include "threadpool.h"
using namespace std;

int sum1(int a, int b)
{
	return a + b;
}

int sum2(int a, int b, int c)
{
	return a + b + c;
}

int main()
{
	ThreadPool pool;
	pool.setMode(PoolMode::MODE_CACHED);
	pool.start(2);
	future<int> r1 = pool.submitTask(sum1, 2, 3);
	future<int> r2 = pool.submitTask(sum2, 2, 3, 4);
	future<int> r3 = pool.submitTask(sum2, 3, 4, 5);
	cout << r1.get() << endl;
	cout << r2.get() << endl;
	cout << r3.get() << endl;

	return 0;
}