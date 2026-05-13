#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "log.h"
#include <pthread.h>

using namespace std;

Log::Log() {
    m_is_async = false;
    m_fp = NULL;
}

Log::~Log() {
    if (m_fp != NULL) fclose(m_fp);
    if (m_buf != NULL) delete[] m_buf;
    if (m_log_queue != NULL) delete m_log_queue;
}

bool Log::init(const char *file_name, int log_buf_size, int max_queue_size) {
    if (max_queue_size >= 1) {
        m_is_async = true;
        m_log_queue = new block_queue<string>(max_queue_size);
        
        // 创建后台写日志的线程（这就是生产者-消费者里的那个“消费者”）
        pthread_t tid;
        pthread_create(&tid, NULL, flush_log_thread, NULL);
        pthread_detach(tid);
    }

    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', m_log_buf_size);

    m_fp = fopen(file_name, "a");
    if (m_fp == NULL) return false;
    
    return true;
}

void Log::write_log(int level, const char *format, ...) {
    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);
    time_t t = now.tv_sec;
    struct tm *sys_tm = localtime(&t);

    char s[16] = {0};
    switch (level) {
        case 0: strcpy(s, "[debug]: "); break;
        case 1: strcpy(s, "[info ]: "); break;
        case 2: strcpy(s, "[warn ]: "); break;
        case 3: strcpy(s, "[erro ]: "); break;
        default: strcpy(s, "[info ]: "); break;
    }

    m_mutex.lock();
    // 写入精确到微秒的时间前缀
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s",
                     sys_tm->tm_year + 1900, sys_tm->tm_mon + 1, sys_tm->tm_mday,
                     sys_tm->tm_hour, sys_tm->tm_min, sys_tm->tm_sec, now.tv_usec, s);

    va_list valst;
    va_start(valst, format);
    // 写入实际日志内容
    int m = vsnprintf(m_buf + n, m_log_buf_size - n - 1, format, valst);
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    string log_str = m_buf;
    m_mutex.unlock();

    // 如果开启了异步且队列未满，塞入队列（极快）；否则退化为同步直接写盘
    if (m_is_async && !m_log_queue->full()) {
        m_log_queue->push(log_str);
    } else {
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);
        fflush(m_fp);
        m_mutex.unlock();
    }
    va_end(valst);
}

void Log::flush(void) {
    m_mutex.lock();
    fflush(m_fp);
    m_mutex.unlock();
}