#include "locker.h"
//构造函数，初始化互斥锁
locker::locker(){
    if(pthread_mutex_init(&m_mutex,NULL) != 0 ){
        //返回值不为零表示出现了错误,抛出异常对象。
        throw std::exception();

    }
}
//析构函数
locker::~locker(){
    pthread_mutex_destroy(&m_mutex);
}

    //上锁
bool locker::lock(){

        //调用包中的lock方法给互斥锁上锁。
    return pthread_mutex_lock(&m_mutex) == 0;
}

//解锁
bool locker::unlock(){
    return pthread_mutex_unlock(&m_mutex) == 0;
}

//get方法获取当前锁
pthread_mutex_t * locker::get(){
    return &m_mutex;
} 

//条件变量
cond::cond(){
    if(pthread_cond_init(&m_cond,NULL) != 0){
        throw std::exception();
    }
}

cond::~cond(){
    pthread_cond_destroy(&m_cond);
}

bool cond::wait(pthread_mutex_t * mutex){
    return pthread_cond_wait(&m_cond,mutex) == 0;
}

bool cond::timedwait(pthread_mutex_t * mutex,struct timespec t){
    return pthread_cond_timedwait(&m_cond,mutex,&t) == 0;
}

bool cond::signal(){
    return pthread_cond_signal(&m_cond) == 0;
}

bool cond::broadcast(){
    return pthread_cond_broadcast(&m_cond) == 0;
}

//信号量
sem::sem(){
    if(sem_init(&m_sem,0,0) != 0){
        throw std::exception();
    }
}
sem::sem(int num){
    if(sem_init(&m_sem,0,num) != 0){
        throw std::exception();
    }
}

sem::~sem(){
    sem_destroy(&m_sem);
}

bool sem::wait(){
    return sem_wait(&m_sem) == 0;
}

bool sem::post(){
    return sem_post(&m_sem) == 0;
}