#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>

#include "locker.h"

using namespace std;

// 实现线程池类
template< typename T>
class threadpool{
public:
    // thread_number 是线程池中线程数量，max_requests 是请求队列
    // 最大允许的等待请求数量
    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    // 向请求队列中添加任务
    bool append(T* request);

private:
    //工作线程运行的函数，不断由工作队列中取出任务并执行
    static void* worker(void* arg);
    void run();

private:
    int m_thread_number; // 线程池中的线程数
    int m_max_requests; // 请求队列中允许驻留的最大线程数
    pthread_t* m_threads; // 描述线程池的数组
    list<T*> m_workqueue; // 请求队列
    locker m_queuelocker; // 保护请求队列的互斥锁

    sem m_queuestat; // 用信号量表示是否还有任务需要处理
    bool m_stop; // 是否结束线程
};

template<typename T>
threadpool<T>::threadpool(int thread_number, int max_requests):m_stop(false), m_thread_number(thread_number),
    m_max_requests(max_requests){
    
    if(thread_number <= 0 || max_requests <= 0){
        throw exception();
    }

    // 创建 thread_number 个线程，并将它们设置为detach线程
    m_threads = new pthread_t[m_thread_number];
    if(!m_threads){
        throw exception();
    }
    for(int i=0;i<thread_number;i++){
        if(pthread_create(m_threads+i, NULL, worker, this) != 0){
            delete[] m_threads;
            throw exception();
        }
        if(pthread_detach(m_threads[i]) != 0){
            delete[] m_threads;
            throw exception();
        }
    }
}

template<typename T>
threadpool<T>::~threadpool(){
    delete[] m_threads;
    m_stop = true;
}

template<typename T>
void* threadpool<T>::worker(void* arg){
    threadpool* pool = (threadpool*) arg;
    pool->run();
    return pool;
}

// 由工作队列中取出任务并执行
template<typename T>
void threadpool<T>::run(){
    while(!m_stop){
        m_queuestat.wait(); // 等待信号量
        m_queuelocker.lock();
        if(m_workqueue.empty()){
            m_queuelocker.unlock();
            continue;
        }
        T* curtask = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if(!curtask){
            continue;
        }
        curtask->process();
    }
}

template<typename T>
bool threadpool<T>::append(T* request){
    if(!request){
        return false;
    }
    m_queuelocker.lock();
    // 判断请求队列大小是否过大
    if(m_workqueue.size() >= m_max_requests){
        m_queuelocker.unlock();
        return false;
    }
    
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post(); // 要处理的任务增加了，用信号量通知线程,EL模式
    return true;
}








#endif