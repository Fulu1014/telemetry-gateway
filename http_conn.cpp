#include "http_conn.h"
#include "log.h"
using json = nlohmann::json;
// 必须在类外定义静态成员变量，这才是真正分配内存的地方
int http_conn::m_epollfd = -1;
int http_conn::m_user_count = 0;

extern sort_timer_lst timer_lst;

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file\n";


http_conn::http_conn(){
    // 初始化所有成员变量为0
    m_sockfd = -1;
    m_read_idx = 0;
    m_content_length = 0;
    // 清空缓冲区
    memset(m_read_buf, 0, READ_BUFFER_SIZE);

    m_write_idx = 0;
    m_iv_count = 0;
    m_file_address = NULL;
}

http_conn::~http_conn(){
    // 释放mmap映射的内存
    if (m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = NULL;
    }
}

// 由线程池中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process(){
    //解析HTTP请求，使用到了有限状态机，一行一行的去解析这个数据。
    HTTP_CODE read_ret =  process_read();
    //NO_REQUEST：请求不完整，需要继续读取客户数据	
    if(read_ret == NO_REQUEST){
        //重新检测一下数据请求
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        return;
    }

    bool write_ret = process_write(read_ret);
    if(!write_ret){
        close_conn();
    }
    modfd(m_epollfd,m_sockfd,EPOLLOUT);
}

//设置文件描述符为非阻塞
void setnonblocking(int fd){
    int old_flag = fcntl(fd,F_GETFL);
    int new_flag = old_flag|O_NONBLOCK;
    fcntl(fd,F_SETFL,new_flag);
}

//添加文件描述符到epoll中。
void addfd(int epollfd,int fd,bool one_shot,bool et = false){
    // 1. 创建一个epoll事件结构体，用于描述要监听的事件和对应的文件描述符
    epoll_event event;

    // 2. 将需要监听的文件描述符fd存入事件结构体的data字段
    // 事件触发时，可以通过event.data.fd获取是哪个文件描述符产生了事件
    event.data.fd = fd;

    // 3. 设置要监听的事件类型（按位或组合多个事件）
    // EPOLLIN：监听读事件（对方发送数据过来 / 有新连接到达）
    // EPOLLRDHUP：监听对方关闭连接事件（TCP连接被对端关闭或半关闭）
    // 注意 修改点，在这里修改为边缘触发
    //event.events = EPOLLIN | EPOLLRDHUP;
    event.events = EPOLLIN | EPOLLRDHUP ;
    //判断是否开启边缘触发
    if(et){
        event.events |= EPOLLET;
    }

    // 4. 如果开启了EPOLLONESHOT模式
    if(one_shot){
        // EPOLLONESHOT：让该事件只触发一次，触发后自动从epoll监听列表中移除
        // 多线程环境下必须开启，防止同一个fd的事件被多个线程同时处理导致竞态条件
        event.events |= EPOLLONESHOT;
    }

    // 5. 将配置好的事件添加到epoll实例中
    // 参数说明：
    // epollfd：epoll实例的文件描述符
    // EPOLL_CTL_ADD：操作类型，表示添加新的事件
    // fd：要监听的文件描述符
    // &event：指向配置好的epoll_event结构体的指针
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);

    //设置文件描述符为非阻塞
    setnonblocking(fd);
}

//从epoll中删除文件描述符
void removefd(int epollfd, int fd){
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    close(fd);
}

//修改文件描述符,重置socket上的EPOLLONESHOT事件，确保下一次可读时，EPOLLIN事件能够触发。
void modfd(int epollfd,int fd, int ev){
    epoll_event event;
    event.data.fd = fd;
    event.events = ev |EPOLLET| EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&event);
}

//初始化连接
void http_conn::init(int sockfd,const sockaddr_in &addr){
    m_sockfd = sockfd;
    m_address = addr;

    // ✅ 每次新连接都重置读写指针
    m_read_idx = 0;;
    
    // ✅ 清空缓冲区
    memset(m_read_buf, 0, READ_BUFFER_SIZE);

    //设置端口复用
    int reuse = 1;//这里等于1才可以进行复用
    setsockopt(m_sockfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));

    //添加到epoll对象中
    addfd(m_epollfd,m_sockfd,true,true);
    m_user_count++;//总用户数+1

    init();
}

//初始化其余的信息
void http_conn::init(){
    m_check_state = CHECK_STATE_REQUESTLINE;    //初始化状态为解析请求首行
    m_checked_index = 0;
    m_start_line = 0;                           //当前正在解析的行的索引
    m_read_idx = 0;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_linger = false;
    m_content_length = 0;
    memset(m_read_buf, 0, READ_BUFFER_SIZE);     // ✅ 清空缓冲区

    m_write_idx = 0;
    m_iv_count = 0;
    m_file_address = NULL;
    memset(m_write_buf, 0, sizeof(m_write_buf));

}

//关闭连接
void http_conn::close_conn(){
    
    if(m_sockfd != -1){
        //关闭连接时，删除定时器
        if(timer){
            timer_lst.del_timer(timer);
            timer = nullptr;
        }

        removefd(m_epollfd,m_sockfd);
        m_sockfd = -1;
        m_user_count--; //关闭一个链接，客户总数了-1
    }
}

//非阻塞的读数据，循环的读取数据直到没有数据可读或者是客户端断开了连接。
bool http_conn::read(){

    //判断当前缓冲是否已经满了，是否需要等待下一次去读取数据。
    if(m_read_idx >= READ_BUFFER_SIZE){
        return false;
    }
    //已经读取到的字节
    int bytes_read = 0;
    while(true){
        //读取数据，从上一次开始的位置进行读取，数据的大小为总的数据大小减去已经读取的数据大小
        bytes_read = recv(m_sockfd,(m_read_buf+m_read_idx),READ_BUFFER_SIZE- m_read_idx,0);
        if(bytes_read == -1){
            //非阻塞文件描述符执行相同操作时，如果无法立即完成，会立即返回 - 1，并将 errno 设置为 EAGAIN/EWOULDBLOCK
            if(errno == EAGAIN||errno==EWOULDBLOCK){
                //没有数据
                break;
            }
            return false;
        }else if(bytes_read == 0){
            //对方关闭连接
            return false;
        }
        //修改下一次要读取的数据位置标识为当前所指加上再一次已读。
        m_read_idx += bytes_read;
    }

    // ✅ 关键修复：在读取到的数据末尾添加字符串结束符
    // 注意：要确保缓冲区有空间放这个'\0'，所以READ_BUFFER_SIZE要比实际最大读取量大1
    if(m_read_idx < READ_BUFFER_SIZE){
        m_read_buf[m_read_idx] = '\0';
    } else {
        // 缓冲区满了，最后一个字节强制设为'\0'
        m_read_buf[READ_BUFFER_SIZE - 1] = '\0';
    }

    printf("读取到了 %d 字节数据，数据:\n %s\n",m_read_idx,m_read_buf);
    return true;
}

//主状态机,解析请求
http_conn::HTTP_CODE http_conn::process_read(){
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char * text = 0;
    //一行一行的循环解析数据,如果最后返回ok，表示正常的解析
    while(((m_check_state == CHECK_STATE_CONTENT)&&(line_status == LINE_OK))||((line_status = parse_line()) == LINE_OK)){
        //解析到了一行完整的数据，或者解析到了请求体，也是一个完整的数据
        //获取一行数据
        text = get_line();

        //下一行的开始，为上一次已经检测过的
        m_start_line = m_checked_index;
        printf("got 1 http line : %s\n",text);
        //switch()判断
        switch(m_check_state){
            case CHECK_STATE_REQUESTLINE:
            {
                ret = parse_request_line(text);
                if(ret == BAD_REQUEST){
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER:
            {
                ret = parse_headers(text);
                if(ret == BAD_REQUEST){
                    return BAD_REQUEST;
                }else if(ret == GET_REQUEST){
                    //解析具体的请求信息
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT:
            {
                ret = parse_content(text);
                if(ret == GET_REQUEST){
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
            default:
            {
                return INTERNAL_ERROR;
            }
        }
    }

    return NO_REQUEST;
}

//解析请求行,获取请求方法，目标URL，HTTP版本
/**
 *  1. 拆分"请求方法"和"URL+版本"
    2. 验证请求方法是否为GET
    3. 拆分"URL"和"HTTP版本"
    4. 验证HTTP版本是否为HTTP/1.1
    5. 兼容处理完整URL格式（http://xxx/xxx）
    6. 验证URL是否以/开头
    7. 切换状态机到解析请求头
    8. 返回NO_REQUEST
 */
http_conn::HTTP_CODE http_conn::parse_request_line(char * text){
    // GET /index.html HTTP/1.1
    m_url = strpbrk(text," \t");
    // GET\0/index.html HTTP/1.1
    // ✅ 修复：添加空指针检查
    if(!m_url){
        return BAD_REQUEST;
    }

    *m_url ++= '\0';

    char * method = text;
    if(strcasecmp(method,"GET")==0){
        m_method = GET;
    }else if(strcasecmp(method,"POST")==0){
        m_method = POST;
    }else{
        return BAD_REQUEST;
    }
    // /index.html HTTP/1.1
    m_version = strpbrk(m_url," \t");
    if(!m_version){
        return BAD_REQUEST;
    }
    // /index.html\0HTTP/1.1
    *m_version++ = '\0';
    if(strcasecmp(m_version,"HTTP/1.1")!=0){
        return BAD_REQUEST;
    }

    // http://192.168.1.1:10000/index.html
    if(strncasecmp(m_url,"http://", 7) == 0){
        m_url += 7;
        //192.168.1.1:10000/index.html
        m_url = strchr(m_url,'/'); // /index.html

        // ✅ 修复：如果域名后面没有路径，默认使用根路径/
        if(!m_url){
            m_url = (char*)"/";
        }
    }else if(strncasecmp(m_url,"https://", 8) == 0){
        m_url += 8;
        m_url = strchr(m_url,'/');
        if(!m_url){
            m_url = (char*)"/";
        }
    }

    if(!m_url || m_url[0]!='/'){
        return BAD_REQUEST;
    }

    //主状态机检查状态变成检查请求头
    m_check_state = CHECK_STATE_HEADER;

    return NO_REQUEST;
}

// 解析请求头
http_conn::HTTP_CODE http_conn::parse_headers(char * text) {
    // 情况1：遇到空行，表示请求头解析完毕
    if (text[0] == '\0') {
        // 如果有Content-Length，说明有POST请求体需要解析
        if (m_content_length != 0) {
            // 切换主状态机到解析请求体状态
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 没有请求体，整个HTTP请求解析完成
        return GET_REQUEST;
    }

    // 情况2：解析Connection字段（判断是否是长连接）
    //strncasecmp函数忽略大小写，只比较两个字符串的前 11 个字符
    else if (strncasecmp(text, "Connection:", 11) == 0) {
        text += 11;
        // 跳过前面的空格
        while (*text == ' ' || *text == '\t') {
            text++;
        }
        if (strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;
        }
    }

    // 情况3：解析Content-Length字段（获取POST请求体的长度）
    else if (strncasecmp(text, "Content-Length:", 15) == 0) {
        text += 15;
        while (*text == ' ' || *text == '\t') {
            text++;
        }
        // 将字符串转换为整数
        m_content_length = atoi(text);
        // 防御性检查：Content-Length不能为负数
        if (m_content_length < 0) {
            return BAD_REQUEST;
        }
    }

    // 情况4：解析Host字段（可选，用于虚拟主机）
    else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        while (*text == ' ' || *text == '\t') {
            text++;
        }
        // 可以将Host保存到成员变量中，这里暂时不处理
    }

    // 其他请求头字段可以根据需要添加解析逻辑
    // 比如：User-Agent、Accept、Cookie等

    // 继续解析下一行请求头
    return NO_REQUEST;
}

// 解析请求体（仅POST请求会调用）
http_conn::HTTP_CODE http_conn::parse_content(char * text) {
    // 检查是否已经读取到了完整的请求体
    // m_read_idx：缓冲区中已读取的总字节数
    // m_checked_index：已经解析到的位置
    // 两者的差就是请求体的长度
    if (m_read_idx >= (m_checked_index + m_content_length)) {
        // 将请求体的起始地址保存到text指针
        text = m_read_buf + m_checked_index;
        // 整个HTTP请求解析完成
        return GET_REQUEST;
    }

    // 数据不完整，还没收到完整的请求体
    return NO_REQUEST;
}

/**解析一行的流程分析
    从 m_checked_index 开始，逐个字节检查：
    如果遇到 '\r'：
        如果下一个字节就是数据末尾 → 数据不完整，返回 LINE_OPEN
        如果下一个字节是 '\n' → 找到完整行，处理后返回 LINE_OK
        其他情况 → 行格式错误，返回 LINE_BAD
        
    如果遇到 '\n'：
        如果前一个字节是 '\r' → 容错处理，返回 LINE_OK
        其他情况 → 行格式错误，返回 LINE_BAD
        
    检查完所有数据还没找到行尾 → 返回 LINE_OPEN
 */
http_conn::LINE_STATUS http_conn::parse_line(){
    char temp;
    
    // 核心循环：从上次检查到的位置开始，逐个字节检查
    // 只检查已经读取到的数据（m_checked_index < m_read_idx）
    for(; m_checked_index < m_read_idx; ++m_checked_index){
        
        // 取出当前检查的字节
        temp = m_read_buf[m_checked_index];
        
        // 情况1：遇到了回车符 '\r'
        if(temp == '\r'){
            
            // 子情况1.1：'\r' 刚好在数据的最后一个字节
            // 说明 '\n' 还没收到，数据不完整
            if((m_checked_index + 1) == m_read_idx){
                return LINE_OPEN;
            }
            // 子情况1.2：'\r' 后面跟着 '\n' → 找到了完整的行尾！
            else if(m_read_buf[m_checked_index + 1] == '\n'){
                
                // ✅ 最关键的一步：把 '\r' 和 '\n' 都替换成 '\0'
                // 这样原来的 "GET / HTTP/1.1\r\n" 就变成了 "GET / HTTP/1.1\0\0"
                // 变成了一个标准的C风格字符串，后面可以直接用 strcmp、strtok 等函数
                m_read_buf[m_checked_index++] = '\0';
                m_read_buf[m_checked_index++] = '\0';
                
                // 成功解析一行
                return LINE_OK;
            }
            
            // 子情况1.3：'\r' 后面不是 '\n' → 行格式错误
            return LINE_BAD;
        }
        // 情况2：遇到了换行符 '\n'
        else if(temp == '\n'){
            
            // 容错处理：有些不规范的客户端可能只发 '\n' 不发 '\r'
            // 检查前一个字节是不是 '\r'
            if((m_checked_index > 1) && (m_read_buf[m_checked_index - 1] == '\r')){
                
                // 同样把 '\r' 和 '\n' 替换成 '\0'
                m_read_buf[m_checked_index - 1] = '\0';
                m_read_buf[m_checked_index++] = '\0';
                
                return LINE_OK;                
            }
            
            // 单独的 '\n' 不是合法的行尾 → 格式错误
            return LINE_BAD;
        }
    
    }

    // 所有数据都检查完了，还没找到行尾 → 数据不完整
    return LINE_OPEN;
}

/**核心原理
1. 拼接网站根目录和请求的URL，得到实际文件路径
2. 调用stat()获取文件的状态信息
3. 检查文件是否存在、是否是目录、是否有读权限
4. 调用open()打开文件
5. 调用mmap()将文件映射到进程的虚拟内存空间
6. 关闭文件描述符
7. 返回FILE_REQUEST，表示文件准备就绪，可以发送
 */
// 处理HTTP请求，找到对应的文件并映射到内存
http_conn::HTTP_CODE http_conn::do_request() {
    
    /**
        strcpy(m_real_file, ROOT)：先把网站根目录复制到m_real_file中
        strncpy(m_real_file + len, m_url, ...)：再把请求的 URL 拼接到根目录后面
        例如：如果ROOT="/var/www/html"，m_url="/index.html"，那么m_real_file就变成"/var/www/html/index.html"
        使用strncpy而不是strcpy是为了防止缓冲区溢出
    */

    // ====== 【新增代码：拦截边缘设备遥测数据 POST 请求】 ======
    if (m_method == POST && strncasecmp(m_url, "/api/telemetry", 14) == 0) {
        // 1. 获取请求体 (m_read_buf + m_checked_index 是 body 的起始位置)
        char* body = m_read_buf + m_checked_index;
        // 为了安全解析 JSON，临时将末尾置为字符串结束符 '\0'
        char temp = body[m_content_length];
        body[m_content_length] = '\0';

        try {
            // 2. 解析 JSON 数据
            json j = json::parse(body);
            std::string device_id = j["device_id"];
            std::string status = j["status"];

            // 3. 使用 RAII 机制从连接池获取数据库连接 (全自动借还)
            MYSQL *mysql = NULL;
            connectionRAII mysqlcon(&mysql, connection_pool::GetInstance());

            if (mysql != NULL) {
                // 4. 组装并执行 SQL 语句入库
                std::string sql_insert = "INSERT INTO device_data(device_id, status) VALUES('" + device_id + "', '" + status + "')";
                if (mysql_query(mysql, sql_insert.c_str()) != 0) {
                    LOG_ERROR("数据入库失败: %s", mysql_error(mysql));
                    body[m_content_length] = temp; // 恢复原始字符
                    return INTERNAL_ERROR;
                }
                LOG_INFO("成功入库: 设备[%s], 状态[%s]", device_id.c_str(), status.c_str());
            } else {
                return INTERNAL_ERROR; // 连接池已被耗尽或获取失败
            }
            
            body[m_content_length] = temp; // 恢复原始字符
            return JSON_REQUEST; // 告诉写缓冲区，去构造 JSON 成功响应
            
        } catch (...) {
            printf("JSON 解析失败，请求体格式异常！\n");
            body[m_content_length] = temp; 
            return BAD_REQUEST;
        }
    }

    // ====== 【新增代码：处理设备心跳保活 (Heartbeat)】 ======
    if (m_method == POST && strncasecmp(m_url, "/api/heartbeat", 14) == 0) {
        char* body = m_read_buf + m_checked_index;
        char temp = body[m_content_length];
        body[m_content_length] = '\0';

        try {
            json j = json::parse(body);
            std::string device_id = j["device_id"];

            // 记录一条心跳日志（不操作数据库，因为心跳频率极高，写库会压垮系统）
            LOG_INFO("收到设备[%s]的心跳探测，连接生命周期已重置", device_id.c_str());

            body[m_content_length] = temp;
            return HEARTBEAT_REQUEST; // 告诉写缓冲区，去构造心跳响应
            
        } catch (...) {
            LOG_ERROR("心跳包 JSON 解析失败！");
            body[m_content_length] = temp; 
            return BAD_REQUEST;
        }
    }
    // ====== 【新增代码结束】 ======

    // 1. 拼接实际文件路径：根目录 + 请求的URL
    strcpy(m_real_file, ROOT);
    int len = strlen(ROOT);
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    /**
        stat()函数：获取文件的详细信息，包括大小、权限、修改时间等
        如果返回值小于 0，说明文件不存在，返回NO_RESOURCE（404 Not Found）
     */
    // 2. 获取文件的状态信息
    if (stat(m_real_file, &m_file_stat) < 0) {
        // 文件不存在
        return NO_RESOURCE;
    }

    /*
        m_file_stat.st_mode：文件的权限位
        S_IROTH：其他用户的读权限
        如果文件没有其他用户的读权限，返回FORBIDDEN_REQUEST（403 Forbidden）
    */
    // 3. 检查文件是否有读权限
    if (!(m_file_stat.st_mode & S_IROTH)) {
        // 没有读权限
        return FORBIDDEN_REQUEST;
    }

    /*
        S_ISDIR()宏：判断文件是否是目录
        如果是目录，返回BAD_REQUEST（400 Bad Request）
    */
    // 4. 检查是否是目录
    if (S_ISDIR(m_file_stat.st_mode)) {
        // 不能直接访问目录
        return BAD_REQUEST;
    }

    /**
        以只读模式打开文件
        如果打开失败，返回NO_RESOURCE
     */
    // 5. 打开文件
    int fd = open(m_real_file, O_RDONLY);
    if (fd < 0) {
        // 打开文件失败
        return NO_RESOURCE;
    }


    /*
        这是整个函数最核心的一步
        mmap()函数：将一个文件映射到进程的虚拟内存空间
        参数说明：
            0：让内核自动选择映射地址
            m_file_stat.st_size：映射的大小（整个文件）
            PROT_READ：映射区域可读
            MAP_PRIVATE：创建一个私有的写时复制映射
            fd：要映射的文件描述符
            0：从文件开头开始映射
        映射完成后，我们就可以像访问内存一样访问文件内容，不需要调用read()函数，效率非常高
    */
    // 6. 将文件映射到进程的虚拟内存空间
    // 这样我们就可以直接通过指针访问文件内容，不需要read()
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

    // 7. 关闭文件描述符（映射完成后就不需要了）
    close(fd);

    // 8. 文件准备就绪，可以发送
    return FILE_REQUEST;
}

// 核心作用：安全地向写缓冲区写入格式化字符串，是所有响应生成的基础。
bool http_conn::add_response(const char *format, ...) {
/*
const char *format：格式化字符串，和printf用法完全一致
...：可变参数列表，可以传入任意多个参数
返回值：true= 写入成功，false= 缓冲区已满
*/

    if (m_write_idx >= sizeof(m_write_buf)) {
        return false;
    }
    /*
    先做边界检查：如果写缓冲区已经满了，直接返回失败
    m_write_idx：写缓冲区中已经写入的字节数（下一个写入位置的索引）
    防止缓冲区溢出，这是 C 语言字符串处理的基本安全原则
    */

    va_list arg_list;
    va_start(arg_list, format);
    /*
    C 语言可变参数的标准初始化流程
    va_list：定义一个可变参数列表变量
    va_start：让arg_list指向第一个可变参数的地址
    必须和后面的va_end成对使用，否则会有内存泄漏
    */

    int len = vsnprintf(m_write_buf + m_write_idx, 
                        sizeof(m_write_buf) - m_write_idx, 
                        format, arg_list);
    /*
    vsnprintf：专门处理可变参数的安全格式化函数
    参数详解：
        m_write_buf + m_write_idx：写入的起始地址（从缓冲区的末尾开始写）
        sizeof(m_write_buf) - m_write_idx：最多允许写入的字节数（剩余空间）
        format：格式化字符串
        arg_list：可变参数列表
    返回值：实际写入的字节数（不包括末尾的\0）
    为什么不用sprintf？：sprintf不检查缓冲区大小，极易导致缓冲区溢出攻击
    */
    
    if (len >= (sizeof(m_write_buf) - m_write_idx)) {
        va_end(arg_list);
        return false;
    }
    /*
    检查是否写入了超过缓冲区剩余空间的内容
    如果是，说明内容被截断了，返回失败
    注意：这里必须先调用va_end清理可变参数，再返回
    */

    m_write_idx += len;
    va_end(arg_list);
    return true;
    /*
    更新m_write_idx，指向缓冲区新的末尾
    调用va_end清理可变参数列表
    返回true表示写入成功
     */
}

// 添加状态行：HTTP/1.1 200 OK
bool http_conn::add_status_line(int status, const char *title) {
    return add_response("HTTP/1.1 %d %s\r\n", status, title);
    /*
    生成标准的 HTTP 状态行，格式严格遵循 HTTP/1.1 协议
    示例：add_status_line(200, "OK") → 生成 "HTTP/1.1 200 OK\r\n"
    末尾必须加\r\n，这是 HTTP 协议规定的行结束符
    */
}

// 添加响应头
bool http_conn::add_headers(int content_len) {
    //content_len：响应体的字节长度，用于设置Content-Length头

    // 添加Content-Length
    add_response("Content-Length: %d\r\n", content_len);
    /*
    最重要的响应头：告诉客户端响应体的准确长度
    客户端根据这个值判断什么时候接收完所有数据
    如果没有这个头，客户端无法知道响应何时结束
     */
    // 添加Connection
    add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
    /*
    告诉客户端是否保持长连接
    m_linger是在parse_headers()中解析出来的：
    如果客户端发送了Connection: keep-alive → m_linger = true
    否则 → m_linger = false
    长连接：发送完响应后不关闭连接，等待下一个请求
    短连接：发送完响应后立即关闭连接
    */

    // 添加空行，表示响应头结束
    add_response("\r\n");
    /*
    最容易被忽略但极其重要的一行
    HTTP 协议规定：响应头和响应体之间必须用一个空行（\r\n\r\n）分隔
    这里的\r\n加上上一行末尾的\r\n，正好组成了\r\n\r\n
    客户端看到这个空行，就知道响应头结束了，后面的所有内容都是响应体
     */
    return true;
}

// 添加响应体
bool http_conn::add_content(const char *content) {
    return add_response("%s", content);
    /*
    最简单的函数，直接把响应体内容写入写缓冲区
    注意：这个函数只用于错误响应
    正常的文件请求不会用这个函数，因为文件内容是通过mmap映射到内存的，我们会用writev直接发送，不需要拷贝到写缓冲区
    */
}

// 根据do_request()返回的处理结果，生成完整的 HTTP 响应，并准备好用于发送的iovec数组。
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret) {
        case INTERNAL_ERROR: {
            // 500 服务器内部错误
            add_status_line(500, "Internal Server Error");
            add_headers(strlen("<html><body>500 Internal Server Error</body></html>"));
            add_content("<html><body>500 Internal Server Error</body></html>");
            break;
        }

        case BAD_REQUEST: {
            // 400 请求错误
            add_status_line(400, "Bad Request");
            add_headers(strlen("<html><body>400 Bad Request</body></html>"));
            add_content("<html><body>400 Bad Request</body></html>");
            break;
        }

        case NO_RESOURCE: {
            // 404 文件不存在
            add_status_line(404, "Not Found");
            add_headers(strlen("<html><body>404 Not Found</body></html>"));
            add_content("<html><body>404 Not Found</body></html>");
            break;
        }

        case FORBIDDEN_REQUEST: {
            // 403 禁止访问
            add_status_line(403, "Forbidden");
            add_headers(strlen("<html><body>403 Forbidden</body></html>"));
            add_content("<html><body>403 Forbidden</body></html>");
            break;
        }

        case FILE_REQUEST: {
            // 200 请求成功，返回文件
            add_status_line(200, "OK");
            if (m_file_stat.st_size != 0) {
                // 添加响应头
                add_headers(m_file_stat.st_size);
                /*
                先添加 200 状态行
                如果文件不为空，添加响应头，响应体长度是文件的大小
                */

                
                // 第一个iovec：响应头
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;

                // 第二个iovec：文件内容（mmap映射的内存）
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;

                m_iv_count = 2;
                return true;
                /*
                这是整个函数最核心的优化点：使用分散写技术
                iovec是 Linux 系统用于分散写的结构体，包含两个字段：
                iov_base：内存块的起始地址
                iov_len：内存块的长度
                我们准备了两个内存块：
                第一个块：响应头，存在m_write_buf里
                第二个块：文件内容，存在mmap映射的内存里
                设置m_iv_count = 2，告诉writev()要发送两个内存块
                为什么这么做？：避免了把文件内容拷贝到写缓冲区的操作，实现了零拷贝传输，大大提高了性能
                */

            } else {
                // 文件为空
                const char *ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                add_content(ok_string);
                //如果文件为空，返回一个空的 HTML 页面
            }
            break;
            
        }
        case JSON_REQUEST: {
            add_status_line(200, "OK");
            const char *json_response = "{\"code\": 200, \"msg\": \"telemetry data saved successfully\"}";
            add_headers(strlen(json_response));
            add_content(json_response);
            
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv_count = 1; // 纯文本响应，不需要 mmap 映射的第二块内存
            return true;
        }
        // ====== 【新增代码：生成心跳成功响应】 ======
        case HEARTBEAT_REQUEST: {
            add_status_line(200, "OK");
            // 返回一个极简的 pong 响应，节省带宽
            const char *pong_response = "{\"code\": 200, \"msg\": \"pong\"}";
            add_headers(strlen(pong_response));
            add_content(pong_response);
            
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv_count = 1;
            return true;
        }
        // ====== 【新增代码结束】 ======
        
        default: {
            return false;
        }
    }

    // 错误响应只有一个iovec（响应头+响应体都在m_write_buf里）
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
    /*
    所有错误响应的响应头和响应体都在同一个m_write_buf缓冲区中
    所以只需要一个iovec元素
    设置m_iv_count = 1
    返回true表示响应生成成功
     */

}

//非阻塞地发送process_write()准备好的数据，处理边缘触发下的 "一次性发不完" 的情况。
bool http_conn::write() {
//返回值：true= 发送完成或需要等待下一次 EPOLLOUT 事件；false= 发送失败，需要关闭连接
    int bytes_sent = 0;
    int bytes_to_send = m_iv[0].iov_len + m_iv[1].iov_len;
    /**
     * bytes_sent：本次调用writev()实际发送的字节数
     * bytes_to_send：总共需要发送的字节数（所有 iovec 的长度之和）
     */


    if (bytes_to_send == 0) {
        // 没有数据要发送，重新注册读事件
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init(); // 重置连接状态，准备接收下一个请求
        return true;
    }
    /*
    边界情况：如果没有数据要发送
    重新注册 EPOLLIN 事件，等待客户端的下一个请求
    调用init()重置所有成员变量，为下一个请求做准备
    */

    while (true) {//边缘触发的强制要求，利用循环一次性把数据写完
        // 分散写，一次性发送多个iovec
        /*这里用到了写分散技术，可以一次性发送多个不连续的内存块
            响应头存在m_write_buf里
            文件内容存在m_file_address指向的 mmap 内存里
            用writev可以一次把这两部分都发出去，不需要先把它们拷贝到同一个缓冲区
            减少了内存拷贝次数，提高了性能
        */
        bytes_sent = writev(m_sockfd, m_iv, m_iv_count);
        /*
        真正发送数据的系统调用
        writev()：Linux 系统提供的分散写函数，可以一次性发送多个不连续的内存块
            参数详解：
                m_sockfd：客户端的 socket 文件描述符
                m_iv：iovec 数组，包含要发送的所有内存块
                m_iv_count：iovec 数组的元素个数
            返回值：实际发送的字节数
        */

        if (bytes_sent == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 发送缓冲区满了，等待下一次EPOLLOUT事件
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            /*
            非阻塞 IO 的正常情况：发送缓冲区满了，暂时发不出去
            这不是错误，只是需要等待内核把发送缓冲区的数据发出去
            重新注册 EPOLLOUT 事件，当发送缓冲区有空间时，epoll 会再次通知我们
            返回true，表示不需要关闭连接
            */


            // 真正的错误，释放资源并关闭连接
            munmap(m_file_address, m_file_stat.st_size);
            m_file_address = NULL;
            return false;
            /*
            其他错误（比如客户端已经关闭连接）
            立即释放mmap映射的内存，防止内存泄漏
            返回false，告诉上层调用者关闭连接
            */
        }


        bytes_to_send -= bytes_sent;//更新剩余需要发送的字节数

        if (bytes_to_send <= 0) { 
            // 所有数据都发送完毕
            munmap(m_file_address, m_file_stat.st_size);
            m_file_address = NULL;
            /*
            释放文件映射的内存，这是必须的，否则会导致严重的内存泄漏
            mmap映射的内存不会自动释放，必须手动调用munmap()
            */

            if (m_linger) {
                // 长连接：重置状态，等待下一个请求
                init();
                modfd(m_epollfd, m_sockfd, EPOLLIN);
                return true;
            } else {
                // 短连接：关闭连接
                return false;
            }
            /*
            根据m_linger的值决定是长连接还是短连接
            长连接：
                调用init()重置所有成员变量
                重新注册 EPOLLIN 事件，等待下一个请求
                返回true，不关闭连接
            短连接：
                返回false，告诉上层调用者关闭连接
            */
        }
        // ✅ 添加：处理部分发送的情况
        if (bytes_sent >= m_iv[0].iov_len) {
            // 第一个iovec已经发完了
            m_iv[0].iov_base = (char *)m_iv[1].iov_base + (bytes_sent - m_iv[0].iov_len);
            m_iv[0].iov_len = m_iv[1].iov_len - (bytes_sent - m_iv[0].iov_len);
            m_iv_count = 1;
        } else {
            // 第一个iovec还没发完
            m_iv[0].iov_base = (char *)m_iv[0].iov_base + bytes_sent;
            m_iv[0].iov_len -= bytes_sent;
        }
        /*
        最容易被忽略但极其重要的一段代码
        处理部分发送的情况：writev()不保证一次性发送完所有数据，它只会发送尽可能多的数据，然后返回实际发送的字节数
        如果我们不更新iovec的指针和长度，下次调用writev()时又会从头开始发送，导致数据重复或错乱
        举个例子：
            第一个 iovec 长度是 100（响应头），第二个是 1000（文件内容）
            writev()只发送了 150 个字节
            那么第一个 iovec 发完了，第二个 iovec 发了 50 个
            更新后：
            m_iv[0].iov_base指向第二个 iovec 的第 50 个字节
            m_iv[0].iov_len = 950
            m_iv_count = 1
            下次调用writev()时，就会从上次中断的地方继续发送
        */

    }
}


/** 完整响应流程总结
1. do_request() 处理请求，返回 HTTP_CODE
   ↓
2. process_write() 根据 HTTP_CODE 生成响应
   ↓
   ├─ 错误响应：响应头+响应体都写入 m_write_buf → 1个iovec
   └─ 正常响应：响应头写入 m_write_buf，文件内容在mmap → 2个iovec
   ↓
3. 主线程 epoll_wait 检测到 EPOLLOUT 事件
   ↓
4. 调用 write() 函数发送数据
   ↓
5. write() 用 writev() 发送所有iovec
   ↓
6. 处理部分发送的情况，更新iovec
   ↓
7. 发送完成后：
   ├─ 长连接：init() 重置状态，注册 EPOLLIN
   └─ 短连接：返回 false，关闭连接
 */



/*核心设计思想
分层设计：从底层工具函数到上层总控函数，层层调用，职责清晰
零拷贝优化：使用mmap+writev实现零拷贝文件传输，避免不必要的内存拷贝
非阻塞 IO：所有读写操作都是非阻塞的，配合 epoll 边缘触发模式，实现高并发
资源管理：mmap内存手动释放，防止内存泄漏
协议严格遵循：严格按照 HTTP/1.1 协议生成响应，保证兼容性
 */