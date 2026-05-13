#ifndef HTTP_CONN_H
#define HTTP_CONN_H

/*
    一开始不知道类里面有什么，只是先将其框架写出，等到后面使用的时候再往里面进行添加。
*/
#include <sys/epoll.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/uio.h>
#include <cstring>
#include "locker.h"
#include "noactive/lst_timer.h"
#include <string>
#include "sql_connection_pool.h" 
#include "json.hpp"

// 文件路径的最大长度
#define FILENAME_LEN 200

// 网站根目录，根据你的实际情况修改
#define ROOT "/home/fulu/Web_Server/html"


void addfd(int epollfd, int fd, bool one_shot);
void modfd(int epollfd, int fd, int ev);
void removefd(int epollfd, int fd);


class http_conn{

public:

    //所有的socket上的事件都被注册到同一个epoll对象中。
    static int m_epollfd;
    //统计用户的数量
    static int m_user_count;
    //读缓冲区的大小
    static const int READ_BUFFER_SIZE = 2048; 
    //写缓冲区的大小
    static const int WRITE_BUFFER_SIZE = 1024;
   
    // 服务器上实际文件的完整路径
    char m_real_file[200];  
    // 文件的状态信息（大小、权限等）     
    struct stat m_file_stat;  
    // mmap映射后的文件内存地址   
    char *m_file_address;        

    // 写缓冲区相关
    char m_write_buf[1024];       // 响应头缓冲区
    int m_write_idx;              // 写缓冲区中已写入的字节数

    // 分散写结构体（用于同时发送响应头和文件内容）
    struct iovec m_iv[2];
    int m_iv_count;               // iovec数组的元素个数

    //HTTP请求方法，
    enum METHOD{GET=0,POST,HEAD,PUT,DELETE,TRACE,OPTIONS,CONNECT};
    
    /*解析客户端请求时，主状态机的状态
        CHECK_STATE_REQUESTLINE  当前正在分析请求行
        CHECK_STATE_HEADER   当前正在分析头部字段
        CHECK_STATE_CONTENT   当前正在解析请求体
    */
    enum CHECK_STATE{CHECK_STATE_REQUESTLINE=0,CHECK_STATE_HEADER,CHECK_STATE_CONTENT};
    
    /*服务器处理HTTP请求的可能结果，报文解析的结果
        NO_REQUEST	：请求不完整，需要继续读取客户数据	
        GET_REQUEST	：表示获得了一个完成的客户请求	
        BAD_REQUEST	：表示客户请求语法错误	
        NO_RESOURCE	：表示服务器没有资源	
        FORBIDDEN_REQUEST：表示客户对资源没有足够的访问权限
        FILE_REQUEST	：文件请求，获取文件成功	
        JSON_REQUEST   :  JSON请求
        INTERNAL_ERROR：表示服务器内部错误
        CLOSED_CONNECTION：表示客户端已经关闭连接了*/    
    enum HTTP_CODE{NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, JSON_REQUEST, HEARTBEAT_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION};
    //从状态机的三种可能状态，即行的读取状态，分别表示
    //1。读取到一个完整的行2。行出错3。行数据尚且不完整
    enum LINE_STATUS{LINE_OK=0,LINE_BAD,LINE_OPEN};

    //定时器指针
    util_timer* timer;


    //构造函数
    http_conn();
    //析构函数
    ~http_conn();
    //任务执行函数process,处理客户端请求;
    void process();
    //初始化新接收的连接。
    void init(int sockfd,const sockaddr_in &addr);
    //关闭连接
    void close_conn();
    //非阻塞的读数据
    bool read();
    //非阻塞的写数据
    bool write();

    //获取socket文件描述符
    int get_sockfd() const {
        return m_sockfd;
    }

private:
    //该HTTP连接的socket
    int m_sockfd;
    //保存客户端的地址信息
    sockaddr_in m_address;
    //读缓冲区
    char m_read_buf[READ_BUFFER_SIZE];
    //标识读缓冲区中已经读入的客户端数据的最后一个字节的下一个位置
    int m_read_idx;
    // 当前正在分析的字符在都缓冲区的位置
    int m_checked_index;
    // 当前正在解析的行的起始位置
    int m_start_line;
    //请求目标文件的文件名
    char * m_url;
    //协议版本，只支持HTTP1.1
    char * m_version;
    //主机名
    char * m_host;
    //判断HTTP请求是否要保持连接
    bool m_linger;
    // POST请求体的长度
    int m_content_length;   
    //请求方法
    METHOD m_method;
    //主状态机当前所处的状态
    CHECK_STATE m_check_state;


    //初始化连接其余的信息
    void init();
    //解析HTTP请求
    HTTP_CODE process_read();
    //解析请求首行
    HTTP_CODE parse_request_line(char * text);
    //解析请求头
    HTTP_CODE parse_headers(char * text);
    //解析请求体
    HTTP_CODE parse_content(char * text);
    //解析行
    LINE_STATUS parse_line();

    HTTP_CODE do_request();

    //向写缓冲区添加格式化字符串
    bool add_response(const char *format, ...);

    //添加状态行：HTTP/1.1 200 OK
    bool add_status_line(int status, const char *title);

    // 添加响应头
    bool add_headers(int content_len);

    // 添加响应体
    bool add_content(const char *content);

    //生成HTTP响应
    bool process_write(HTTP_CODE ret);

    char * get_line(){
        return m_read_buf + m_start_line;
    }

};


#endif