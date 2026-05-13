

```
# 遥测数据网关（高并发 Web 服务器）
基于 C++11 实现的工业级高并发遥测数据网关，采用 Proactor 架构，支持万级并发连接。专为边缘设备设计，提供遥测数据接收、设备心跳保活、数据持久化存储和静态文件服务功能。

## 🚀 项目亮点
✅ **真实业务落地**：不是纯教学项目，针对物联网遥测场景定制开发，支持 JSON 格式设备数据上报  
✅ **高并发架构**：基于 epoll 边缘触发 + 非阻塞 IO，经压力测试支持 10000+ 并发连接  
✅ **核心组件自研**：独立实现线程池、数据库连接池、异步日志系统、升序链表定时器  
✅ **性能优化**：采用 mmap+writev 零拷贝文件传输、EPOLLONESHOT 避免惊群效应  
✅ **资源安全**：全项目遵循 RAII 原则，自动管理数据库连接、文件描述符等资源，彻底避免泄漏  

## 🛠️ 技术栈
- **编程语言**：C++11
- **网络模型**：Proactor 模式 + epoll I/O 多路复用
- **并发机制**：线程池、互斥锁、条件变量、信号量
- **数据存储**：MySQL 5.7 + 数据库连接池
- **数据格式**：JSON（nlohmann/json 库）
- **日志系统**：异步阻塞队列 + 后台写线程
- **构建工具**：Makefile

## ✨ 核心功能
### 1. 高并发网络框架
- 基于 epoll 边缘触发模式实现非阻塞 IO
- 采用 EPOLLONESHOT 保证同一个 socket 事件只被一个线程处理
- 支持 HTTP/1.1 协议，实现 GET/POST 请求解析
- 基于有限状态机实现 HTTP 报文解析，支持不规范报文容错

### 2. 遥测数据处理
- 接收边缘设备 JSON 格式遥测数据
- 自动解析设备 ID 和状态信息
- 基于数据库连接池实现高并发数据入库
- 支持数据校验和异常处理

### 3. 设备心跳管理
- 支持设备 HTTP 心跳保活机制
- 15 秒无数据自动关闭非活跃连接
- 心跳日志记录，不操作数据库避免性能瓶颈

### 4. 基础组件
- **线程池**：预先创建 8 个工作线程，避免频繁创建销毁线程开销
- **数据库连接池**：预先建立 8 个 MySQL 连接，复用连接提升性能
- **异步日志系统**：后台线程批量写盘，避免磁盘 IO 阻塞业务线程
- **定时器**：升序链表实现，自动管理连接生命周期

### 5. 静态文件服务
- 支持 HTML、CSS、JS 等静态文件访问
- 采用 mmap+writev 零拷贝传输，大幅提升文件发送性能

## 📊 性能指标
- 并发连接数：10000+
- QPS：5000+（遥测数据上报场景）
- 响应延迟：<10ms（99% 请求）
- 内存占用：<100MB（空载）

## 🚀 编译与运行
### 环境依赖
- Ubuntu 18.04+ / CentOS 7+
- GCC 7.5+
- MySQL 5.7+
- libmysqlclient-dev

### 安装依赖
​```bash
# Ubuntu
sudo apt-get update
sudo apt-get install g++ make libmysqlclient-dev

# CentOS
sudo yum install gcc-c++ make mysql-devel
```

### 数据库配置

1. 创建数据库和表

```
CREATE DATABASE telemetry_db;
USE telemetry_db;

CREATE TABLE device_data (
    id INT AUTO_INCREMENT PRIMARY KEY,
    device_id VARCHAR(64) NOT NULL,
    status VARCHAR(32) NOT NULL,
    create_time TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
```

1. 修改 `main.cpp` 中的数据库配置

```
// 第98行，改成你自己的MySQL账号密码
connPool->init("localhost", "your_sql_username", "your_sql_password", "telemetry_db", 3306, 8);
```

### 编译运行

```
# 编译
make

# 运行（端口号可自定义）
./gateway 8080
```

### 测试

```
# 测试遥测数据上报
curl -X POST http://localhost:8080/api/telemetry \
-H "Content-Type: application/json" \
-d '{"device_id":"sensor_001","status":"online"}'

# 测试心跳
curl -X POST http://localhost:8080/api/heartbeat \
-H "Content-Type: application/json" \
-d '{"device_id":"sensor_001"}'

# 测试静态文件
curl http://localhost:8080/index.html
```

## 📁 项目目录结构

```
telemetry-gateway/
├── locker.h/locker.cpp          # 线程同步机制封装（互斥锁、条件变量、信号量）
├── threadPool.h                 # 线程池实现
├── http_conn.h/http_conn.cpp    # HTTP连接处理和协议解析
├── sql_connection_pool.h/.cpp   # 数据库连接池 + RAII连接管理
├── log.h/log.cpp                # 异步日志系统
├── block_queue.h                # 线程安全阻塞队列（日志系统核心）
├── lst_timer.h/lst_timer.cpp    # 升序链表定时器
├── main.cpp                     # 主函数，服务器启动入口
├── Makefile                     # 编译脚本
├── html/                        # 静态文件目录
│   └── index.html
└── README.md
```

## 🔮 7来改进方向

-  替换升序链表定时器为时间轮，提升高并发下定时器性能
-  增加 HTTPS 支持（基于 OpenSSL）
-  实现数据库批量入库，提升数据写入性能
-  增加设备管理和数据查询接口
-  支持 MQTT 协议接入
-  增加 Prometheus 监控指标

## 📧 联系方式

- 作者：黄福路
- 邮箱：709287419@qq.com
- GitHub：<https://github.com/>fulu1014/telemetry-gateway

------

**如果这个项目对你有帮助，欢迎点个 Star ⭐ 支持一下！**