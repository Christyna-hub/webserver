#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <list>
#include <exception>
#include <stdio.h>
#include "locker.h"
// 线程池类，定义成模板类是为了代码的复用，
// 模板参数T是任务类
template<typename T>
class threadpool {
public:
    // 初始化函数，默认指定线程池中线程的数量8。
    // 线程池中线程的数量是服务器启动之初就创建好的
    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T* request);

private:
    // 子线程运行逻辑代码;注意要加上static
    static void * worker (void * arg);
    // 线程池运行
    void run();
private:
    // 线程的数量
    int m_thread_number;

    // 线程池数组，大小为m_thread_number
    // 动态创建
    pthread_t * m_threads;

    // 请求队列中最多允许的，等待处理的请求数量
    int m_max_requests;

    // 请求队列，待处理的任务
    std::list<T*> m_workqueue;

    // 互斥锁
    locker m_queuelocker;

    // 信号量用来判断是否有任务需要处理
    sem m_queuestat;

    // 是否结束线程
    bool m_stop;

};

template<typename T>
threadpool<T>::threadpool(int thread_number, int max_requests) :
    m_thread_number(thread_number), m_max_requests(max_requests),
    m_stop(false), m_threads(NULL) {

        if((thread_number) <= 0 || (max_requests <= 0)) {
            throw std::exception();
        }

        m_threads = new pthread_t[m_thread_number]; // 动态创建线程池
        if(!m_threads) {
            throw std::exception();
        }
        // 创建thread_number个线程，并将它们设置为线程脱离
        for (int i = 0; i < thread_number; ++i) {
            printf("create the %dth thread\n", i);

            // worker为静态函数，不可直接访问动态资源，通过参数this来使用动态资源
            if(pthread_create(m_threads + i, NULL , worker, this) != 0) {
                delete [] m_threads;
                throw std::exception();
            }
            
            if(pthread_detach(m_threads[i])) {
                delete[] m_threads;
                throw std::exception();
            }
        }
    }

template<typename T>
threadpool<T>::~threadpool() {
    // 析构 释放线程池，结束主线程
    delete[] m_threads;
    m_stop = true;
}

template<typename T>
bool threadpool<T>::append(T * request) {
    // 在请求队列中追加事件

    // 首先，对请求队列上锁
    m_queuelocker.lock();
    // 若请求队列中请求的大小超出设置的最大请求数量，解锁并返回，不能加入到工作队列中，直接舍弃
    if (m_workqueue.size() >= m_max_requests) {
        m_queuelocker.unlock();
        return false;
    }

    // 工作队列未满，可将请求加入工作队列中
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post(); // 请求+1，信号量增加，待处理的线程增多，线程取数据的时候，根据信号量判断线程是阻塞还是继续执行，信号量用于同步
    return true;
}

template<typename T>
void * threadpool<T>::worker(void * arg) {
    // 工作线程调用函数
    threadpool * pool = (threadpool *) arg;
    pool->run();
    return pool;
}

template<typename T>
void threadpool<T>::run() {
    while(!m_stop) {
         // 有数据/资源处理的时候，线程才不会阻塞
         // 否则线程阻塞在wait处
         m_queuestat.wait();
        // 继续执行，代表有资源待处理
        // 上锁
        m_queuelocker.lock();
        // 信号量不是随着请求资源的增加而增加嘛？？为什么信号量非空时，请求队列为空呢？？
        if (m_workqueue.empty()) {
            m_queuelocker.unlock();
            continue;
        }

        // 有请求
        T * request = m_workqueue.front();
        m_workqueue.pop_front();
        
        if (!request) {
            continue;
        }

        request->process();
    }
}
#endif