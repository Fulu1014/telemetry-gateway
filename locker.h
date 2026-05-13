#ifndef LOCKER_H
#define LOCKER_H

// 线程同步机制封装类


#include <pthread.h>
#include <exception>
#include <semaphore.h>

// 互斥锁类
class locker {
public:
    // 构造函数，初始化互斥锁
    locker();

    // 析构函数，销毁互斥锁
    ~locker();

    // 上锁
    bool lock();

    // 解锁
    bool unlock();

    // 获取互斥锁指针
    pthread_mutex_t* get();

private:
    pthread_mutex_t m_mutex; // 互斥锁变量
};

//条件变量类
class cond{

public:
    //构造函数，初始化条件变量
    cond();
    //析构函数，销毁条件变量
    ~cond();
    //让当前线程无限期阻塞等待，直到其他线程调用signal()或broadcast()将其唤醒。
    bool wait(pthread_mutex_t * mutex);
    //让当前线程限时阻塞等待，要么被其他线程唤醒，要么到达指定的超时时间后自动返回。
    bool timedwait(pthread_mutex_t * mutex,struct timespec t);
    //唤醒至少一个正在等待该条件变量的线程。
    bool signal();
    //唤醒所有正在等待该条件变量的线程。
    bool broadcast();

private:
    pthread_cond_t m_cond;

};

//信号量类
class sem{

public:
    //构造函数，初始化信号量
    sem();
    //构造函数，初始化指定数量的信号量
    sem(int num);
    //析构函数，销毁信号量
    ~sem();
    //请求信号量
    bool wait();
    //释放信号量
    bool post();
    

private:
    sem_t m_sem;
};

#endif // LOCKER_H