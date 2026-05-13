#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "block_queue.h"

using namespace std;

class Log {
public:
    // C++11 单例模式
    static Log *get_instance() {
        static Log instance;
        return &instance;
    }

    // 后台异步写日志的线程回调函数
    static void *flush_log_thread(void *args) {
        Log::get_instance()->async_write_log();
        return NULL;
    }

    // 初始化：文件名、单条日志缓冲区大小、最大队列容量(大于0即开启异步)
    bool init(const char *file_name, int log_buf_size = 8192, int max_queue_size = 1000);

    // 格式化输出日志
    void write_log(int level, const char *format, ...);
    void flush(void);

private:
    Log();
    virtual ~Log();

    // 异步线程真正执行的写盘操作
    void *async_write_log() {
        string single_log;
        while (m_log_queue->pop(single_log)) {
            m_mutex.lock();
            fputs(single_log.c_str(), m_fp);
            fflush(m_fp);
            m_mutex.unlock();
        }
        return NULL;
    }

private:
    locker m_mutex;
    int m_log_buf_size;
    FILE *m_fp;
    char *m_buf;
    block_queue<string> *m_log_queue;
    bool m_is_async;
};

// 提供给外部调用的四个宏定义（有了它，以后打日志就像用 printf 一样简单）
#define LOG_DEBUG(format, ...) Log::get_instance()->write_log(0, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...) Log::get_instance()->write_log(1, format, ##__VA_ARGS__)
#define LOG_WARN(format, ...) Log::get_instance()->write_log(2, format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) Log::get_instance()->write_log(3, format, ##__VA_ARGS__)

#endif