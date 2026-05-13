#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

// 构造函数
connection_pool::connection_pool() {
    m_CurConn = 0;
    m_FreeConn = 0;
}

// 懒汉式单例模式：只有第一次调用时才创建实例
connection_pool *connection_pool::GetInstance() {
    static connection_pool connPool;
    return &connPool;
}

// 初始化连接池
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, int MaxConn) {
    m_url = url;
    m_Port = Port;
    m_User = User;
    m_PassWord = PassWord;
    m_DatabaseName = DBName;

    // 创建 MaxConn 条数据库连接
    for (int i = 0; i < MaxConn; i++) {
        MYSQL *con = NULL;
        con = mysql_init(con);

        if (con == NULL) {
            cout << "MySQL Error: Initialization failed" << endl;
            exit(1);
        }

        if (mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0) == NULL) {
            cout << "MySQL 真实报错原因: " << mysql_error(con) << endl; 
            exit(1);
        }

        // 把建立好的连接放进队列
        connList.push_back(con);
        ++m_FreeConn;
    }

    // 初始化信号量为当前空闲连接数
    reserve = sem(m_FreeConn);
    m_MaxConn = m_FreeConn;
}

// 获取一个可用的数据库连接
MYSQL *connection_pool::GetConnection() {
    MYSQL *con = NULL;

    if (0 == connList.size()) return NULL;

    // 1. 信号量减 1。如果信号量为0，说明没有空闲连接了，线程会在这里阻塞等待
    reserve.wait();
    
    // 2. 加锁，保护 connList，防止多个工作线程同时取同一个连接
    lock.lock();

    con = connList.front();
    connList.pop_front();

    --m_FreeConn;
    ++m_CurConn;

    lock.unlock(); // 解锁

    return con;
}

// 释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL *con) {
    if (NULL == con) return false;

    lock.lock();

    connList.push_back(con);
    ++m_FreeConn;
    --m_CurConn;

    lock.unlock();

    // 释放连接后，信号量加 1，唤醒可能在等待连接的其他线程
    reserve.post();
    return true;
}

// 销毁连接池
void connection_pool::DestroyPool() {
    lock.lock();
    if (connList.size() > 0) {
        list<MYSQL *>::iterator it;
        for (it = connList.begin(); it != connList.end(); ++it) {
            MYSQL *con = *it;
            mysql_close(con);
        }
        m_CurConn = 0;
        m_FreeConn = 0;
        connList.clear();
    }
    lock.unlock();
}

int connection_pool::GetFreeConn() {
    return this->m_FreeConn;
}

connection_pool::~connection_pool() {
    DestroyPool();
}

// ====== RAII 机制实现 ======
// 当 connectionRAII 对象被创建时，自动从连接池获取一个连接
connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool) {
    *SQL = connPool->GetConnection();
    
    conRAII = *SQL;
    poolRAII = connPool;
}

// 当 connectionRAII 对象离开作用域（销毁）时，自动把连接归还给连接池
connectionRAII::~connectionRAII() {
    poolRAII->ReleaseConnection(conRAII);
}