#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "locker.h" // 复用你项目中现有的线程同步封装类

using namespace std;

/*
单例模式 (GetInstance)：连接池在整个服务器生命周期中只能有一个，不能被到处实例化，所以构造函数是私有的。

信号量 (sem reserve)：把它想象成停车场的车位显示牌。如果有空闲连接，直接获取（车入库）；
    如果连接都被别的线程拿光了，当前线程会自动阻塞等待（在门口排队），直到别人还了一个连接。
    这避免了线程空转浪费 CPU。

RAII 机制 (connectionRAII)：这是 C++ 的神技。
    如果程序员手动调用 GetConnection()，用完之后忘记调用 ReleaseConnection()，连接就会永久丢失（连接泄漏）。
    用了 RAII 类，我们只要在函数里实例化这个对象，函数执行完毕（比如发生异常提前 return），局部对象自动销毁，
    析构函数就会自动帮你归还连接，非常安全。
*/


class connection_pool {
public:
    // 获取数据库连接
    MYSQL *GetConnection();
    
    // 释放连接（归还给连接池）
    bool ReleaseConnection(MYSQL *conn);
    
    // 获取当前空闲的连接数
    int GetFreeConn();
    
    // 销毁所有连接
    void DestroyPool();

    // 单例模式：提供一个全局唯一的访问点
    static connection_pool *GetInstance();

    // 初始化连接池属性
    void init(string url, string User, string PassWord, string DataBaseName, int Port, int MaxConn);

private:
    // 构造和析构设为私有，确保单例模式
    connection_pool();
    ~connection_pool();

    int m_MaxConn;  // 最大连接数
    int m_CurConn;  // 当前已使用的连接数
    int m_FreeConn; // 当前空闲的连接数

    locker lock;    // 互斥锁：保护连接池队列，防止多线程同时操作出错
    list<MYSQL *> connList; // 连接池队列
    sem reserve;    // 信号量：表示当前还有多少个空闲连接可用

public:
    string m_url;          // 主机地址
    string m_Port;         // 数据库端口号
    string m_User;         // 登陆数据库用户名
    string m_PassWord;     // 登陆数据库密码
    string m_DatabaseName; // 使用数据库名
};

// RAII 机制：将数据库连接的获取与释放绑定到对象的生命周期上
// 类似智能指针，对象创建时获取连接，对象销毁时自动归还连接
class connectionRAII {
public:
    // 双指针对接，修改外层的 MYSQL 指针
    connectionRAII(MYSQL **con, connection_pool *connPool);
    ~connectionRAII();

private:
    MYSQL *conRAII;
    connection_pool *poolRAII;
};

#endif