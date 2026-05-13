#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <list>
#include <exception>
#include <cstdio>
#include "locker.h"

//线程池类---因为要处理很多不同的数据，所以定义成模板类，提高代码的复用，模板参数T就是任务类
template <typename T>
class threadPool{

public:

    //构造函数，初始化线程的数量和最大允许请求数
    threadPool(int thread_number = 8,int max_request = 1000);
    //析构函数
    ~threadPool();

    //往队列中添加任务
    bool append(T* request);
private:
    static void* worker(void *arg);
    void run();

private:
    //线程的数量
    int m_thread_number;
    //一个数组装线程，即线程池数组，大小为线程的数量
    pthread_t * m_threads;

    //请求队列中最多允许的等待处理的请求数量
    int m_max_request;
    //请求队列,使用列表
    std::list< T*> m_workqueue;

    //互斥锁
    locker m_queuelocker;
    
    //信号量，用来判断是否有任务需要处理
    sem m_queuestat;

    //是否结束线程
    bool m_stop;

    

};

template<typename T>
threadPool<T>::threadPool(int thread_number,int max_request)
:m_thread_number(thread_number),m_max_request(max_request),m_stop(false),m_threads(NULL){
    
    //判断传递过来的线程数量和最大允许请求数量是否合规
    if((thread_number <= 0) || max_request <= 0){
        throw std::exception();
    }

    //创建存放线程的数组，大小为线程的数量
    m_threads = new pthread_t[m_thread_number];
    if(!m_threads){
        throw std::exception();
    }

    //创建thread_number个线程，并将它们设置为线程脱离
    for(int i = 0;i<thread_number;++i){
        //std::cout <<"create the" << i << "th thread" <<endl;
        printf("create the %dth thread\n",i);

        //这里需要注意，线程处理函数必须是一个静态函数
        //this参数表示将当前对象作为参数传递给worker方法。这样静态的worker就可以使用类中的非静态成员变量。
        if(pthread_create(m_threads +i,NULL,worker,this) != 0){
            delete[] m_threads;
            throw std::exception();
        }

        //设置线程分离。
        if(pthread_detach(m_threads[i])){
            delete[]  m_threads;
            throw std::exception();
        }
    }

}

template <typename T>
threadPool<T>::~threadPool(){
    //释放申请的内存空间
    delete[] m_threads;
    //在执行中会根据这个值判断是否停止线程。
    m_stop = true;
}

template <typename T>
bool threadPool<T>:: append(T * request){
    //请求锁
    m_queuelocker.lock();
    if(m_workqueue.size() > m_max_request){
        m_queuelocker.unlock();
        return false;
    }
    //将新的任务添加到队列的尾部
    m_workqueue.push_back(request);
    //解锁
    m_queuelocker.unlock();
    //增加信号量
    m_queuestat.post();

    return true;
}

template <typename T>
void* threadPool<T>:: worker(void * arg){
    //将void* 强转为threadPool类型。
    threadPool * pool = (threadPool *)arg;
    pool ->run();
    return pool;
}

template <typename T>
void threadPool<T>::run(){
    while(!m_stop){
        //请求一个信号量，表示有任务可做。
        m_queuestat.wait();
        m_queuelocker.lock();
        if(m_workqueue.empty()){
            m_queuelocker.unlock();
            continue;
        }
        //获取第一个任务执行
        T* request = m_workqueue.front();
        //从任务队列中删除
        m_workqueue.pop_front();
        //解锁
        m_queuelocker.unlock();

        if(!request){
            continue;
        }

        //调用这个任务的process()函数，去执行任务。                                                                                    
        request->process();
    }
}


#endif //THREADPOOL_H