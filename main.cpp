#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <libgen.h>
#include <signal.h>
#include <cstring>
#include "locker.h"
#include "threadPool.h"
#include "http_conn.h"
#include "sql_connection_pool.h"
#include "log.h"



/**
 *  编写程序模拟Proactor模式
 *  在主线程(main)中，监听有没有数据的到达，比如客户端连接，读数据事件，在主线程一次性将数据读出，封装成一个任务对象，交给工作线程(线程池)，线程池中的线程在取任务并执行。
 **/

 //宏的定义一定不可以有分号。
#define MAX_FD 65535 //最大的文件描述符个数
#define MAX_EVENT_NUMBER 1000 //一次监听的最大的数量

//定义定时器全局变量
#define TIMESLOT 5
static int pipefd[2];
sort_timer_lst timer_lst;
//服务器关闭标识
static bool stop_server = false;



//添加信号捕捉
void addsig(int sig,void(handler)(int)){
    //创建一个结构体，告诉内核，当某个信号来的时候，应该怎么处理
    struct sigaction sa;
    //清空结构体
    memset(&sa,'\0',sizeof(sa));
    // 1. 设置信号处理函数：当信号触发时，调用我们自定义的 handler 函数
    sa.sa_handler = handler;
    //保证信号中断后系统调用重启
    sa.sa_flags |= SA_RESTART;
    // 2. 将所有信号添加到信号屏蔽集：在执行当前信号处理函数期间，屏蔽其他所有信号，防止中断嵌套
    sigfillset(&sa.sa_mask);

    // 3. 注册信号处理动作：将配置好的 sigaction 结构体绑定到指定信号 sig
    sigaction(sig, &sa, NULL);    
}
//信号处理函数(将信号写入管道，交给主线程统一处理)
void sig_handler(int sig) {
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char*)&msg, 1, 0);
    errno = save_errno;
}

// 【新增】定时器超时回调函数（关闭非活跃连接）
void cb_func(http_conn* user) {
    if (!user) return;
    user->close_conn();
    printf("【定时器】超时关闭非活跃连接，fd=%d\n", user->get_sockfd());
}

//extern关键字---声明一个变量 / 函数，告诉编译器它的定义在别的文件里，当前文件先别报错。
//添加文件描述符到epoll中。
extern void addfd(int epollfd,int fd,bool one_shot,bool et);
//从epoll中删除文件描述符
extern void removefd(int epollfd, int fd);
//修改文件描述符
extern void modfd(int epollfd,int fd, int ev);

int main(int argc,char* argv[]){

    //判断参数的个数，执行程序时需要两个参数
    if(argc <= 1){
        printf("按照如下格式允许:%s prot_number\n", basename(argv[0]));
        return 0;
    }

    //获取端口号,将字符串转换为整数
    int port = atoi(argv[1]);
    //往一个已经断开连接的socket里面写数据，内核会给进行发送一个SIGPIE信号，我们使用addsig()进行处理。
    addsig(SIGPIPE,SIG_IGN);

    //初始化数据库连接池
    // 1. 获取单例对象
    connection_pool *connPool = connection_pool::GetInstance();
    // 2. 初始化连接池
     // 参数依次是：IP地址, MySQL用户名, MySQL密码, 数据库名, 端口号, 最大连接数
     // ⚠️ 极其重要：请把 "root" 和 "your_password" 改成你虚拟机里真实的 MySQL 账号密码！
    connPool->init("localhost", "your_mysql_username", "your_mysql_password", "telemetry_db", 3306, 8);
    // 初始化异步日志系统（文件名为 gateway.log，缓冲区8192，队列容量800）
    Log::get_instance()->init("gateway.log", 8192, 800);
    LOG_INFO("数据库连接池初始化成功，已预先建立 8 个连接！");

    //创建线程池，并进行初始化。
    threadPool<http_conn> * pool = NULL;
    try{
        pool = new threadPool<http_conn>(8);
    } catch(...) {
        return 0;
    }

    //创建一个数组，用于保存所有的客户端信息。
    //MAX_FD表示的做多允许多少客户端连接。
    http_conn * users = new http_conn[MAX_FD];
    
    //创建用于监听的套接字
    int listenfd = socket(PF_INET,SOCK_STREAM,0);
    if(listenfd == -1){
        perror("socket");
        return 0;
    }
    //设置端口复用
    int reuse = 1;//这里等于1才可以进行复用
    setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));

    //绑定套接字
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    //这里的端口在前面已经获取。
    address.sin_port = htons(port);
    address.sin_addr.s_addr = INADDR_ANY;
    int red = bind(listenfd,(struct sockaddr*)&address,sizeof(address));
    if(red == -1){
        perror("bind");
        return 0;
    }

    //监听
    red = listen(listenfd,5);
    if(red == -1){
        perror("listen");
        return 0;
    }

    //创建epoll实现多路复用，事件数组，添加监听的文件描述符
    epoll_event events[MAX_EVENT_NUMBER];
    //创建epoll对象,epoll会直接在内核中创建，5表示这个epoll对象占多少个字节。
    int epollfd = epoll_create(5);
    //将监听的文件描述符添加到epoll对象中，方便以后添加，创建一个函数。
    addfd(epollfd,listenfd,false,false);
    
    //设置所有的socket上的事件都被注册到epollfd对象中。
    http_conn::m_epollfd = epollfd;
    //定时器初始化：管道+信号+定时
    int ret = socketpair(PF_UNIX,SOCK_STREAM,0,pipefd);
    fcntl(pipefd[1],F_SETFL,O_NONBLOCK);
    //管道读端假如epoll监听
    addfd(epollfd,pipefd[0],false,false);

    //注册定时器信号+服务器关闭信号
    addsig(SIGALRM,sig_handler);
    addsig(SIGTERM,sig_handler);
    alarm(TIMESLOT);//启动五秒定时
    bool timeout = false;

    //主线程循环检测有哪些事件发生。
    while(!stop_server){
        int num = epoll_wait(epollfd,events,MAX_EVENT_NUMBER,-1);
        if((num < 0) && (errno!=EINTR)){
            printf("epoll failure\n");
            break;
        }
        //循环遍历事件数组
        for(int i =0;i<num;i++){
            int sockfd = events[i].data.fd;
            //处理管道信号
            if (sockfd == pipefd[0] && (events[i].events & EPOLLIN)) {
                char signals[1024];
                int ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret <= 0) continue;

                for (int j = 0; j < ret; j++) {
                    switch (signals[j]) {
                        case SIGALRM:
                            timeout = true;  // 标记需要处理超时
                            break;
                        case SIGTERM:
                            stop_server = true;  // 关闭服务器
                            break;
                    }
                }
                continue;
            }
            if(sockfd == listenfd){
                //有客户端链接进来
                struct sockaddr_in client_address;
                socklen_t client_addrlen = sizeof(client_address);
                int connfd = accept(listenfd,(struct sockaddr*)&client_address,&client_addrlen);
                if(connfd == -1){
                    perror("accept");
                    return 0;
                }

                if(http_conn::m_user_count >= MAX_FD){
                    // 表示目前连接数满了。
                    // 给客户端写一个信息，服务器内部正忙

                    close(connfd);
                    continue;
                }
                // 将新的客户的数据初始化，放到数组当中
                users[connfd].init(connfd,client_address);

                //新连接绑定定时器
                util_timer* timer = new util_timer;
                timer->user_data = &users[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;  // 15秒超时
                users[connfd].timer = timer;
                timer_lst.add_timer(timer);

            } else if(events[i].events & (EPOLLRDHUP|EPOLLHUP|EPOLLERR)){
                //对方异常断开或者错误等事件
                users[sockfd].close_conn();
            } else if(events[i].events & EPOLLIN){
                //读的事件发送
                if(users[sockfd].read()){
                    //一次性把所有的数据都读完
                    pool->append(users + sockfd);
                    //读成功 → 刷新定时器
                    util_timer* timer = users[sockfd].timer;
                    if (timer) {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        timer_lst.adjust_timer(timer);
                    }
                }else{
                    users[sockfd].close_conn();
                }
            }else if(events[i].events & EPOLLOUT){
                //一次性写完所有的数据
                if(!users[sockfd].write()){
                    users[sockfd].close_conn();
                }else{
                    //写成功 → 刷新定时器
                    util_timer* timer = users[sockfd].timer;
                    if (timer) {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        timer_lst.adjust_timer(timer);
                    }
                }
            }
        }
        //处理超时连接
        if(timeout){
            timer_lst.tick();//关闭所有超时链接
            alarm(TIMESLOT);
            timeout = false;

        }


    }
    close(epollfd);
    close(listenfd);
    close(pipefd[0]);
    close(pipefd[1]);
    delete[] users;
    delete pool;


    return 0;
}
