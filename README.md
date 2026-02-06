# High-concurrency-servers

## Reactor 模型

服务端程序处理传入多路请求，是指通过一个或多个输入同时传递给服务器进行请求处理时的事件驱动处理模式。  
并将它们同步分派给请求对应的处理线程，Reactor 模式也叫 Dispatcher 模式。  

简单理解就是使用 **I/O 多路复用** 统一监听事件，收到事件后分发给处理进程或线程。

---

## Reactor 模型的常见实现方式

### 单 Reactor 单线程

#### 工作流程
1. 通过 I/O 多路复用模型进行客户端请求监控  
2. 触发事件后，进行事件处理  
   - 如果是新建连接请求，则获取新建连接，并添加至多路复用模型进行事件监控  
   - 如果是数据通信请求，则进行对应数据处理（接收数据，处理数据，发送响应）

#### 优点
- 所有操作均在同一线程中完成
- 思想流程简单
- 不涉及进程 / 线程间通信及资源争抢问题

#### 缺点
- 无法有效利用 CPU 多核资源
- 极易达到性能瓶颈

#### 适用场景
适用于客户端数量较少，且处理速度较快的场景  
（处理较慢或活跃连接较多时，串行处理会导致响应延迟）

---

### 单 Reactor 多线程

#### 工作流程
1. Reactor 线程通过 I/O 多路复用模型进行客户端请求监控  
2. 触发事件后，进行事件处理  
   - 如果是新建连接请求，则获取新建连接，并添加至多路复用模型进行事件监控  
   - 如果是数据通信请求，则接收数据后分发给 Worker 线程池进行业务处理  
   - 工作线程处理完毕后，将响应交给 Reactor 线程进行数据响应  

#### 优点
- 充分利用 CPU 多核资源

#### 缺点
- 多线程间的数据共享访问控制复杂  
- 单 Reactor 线程承担所有事件监听和响应  
- 高并发场景下容易成为性能瓶颈

---

### 多 Reactor 多线程

#### 工作流程
1. 主 Reactor 处理新连接请求事件，有新连接到来则分发到子 Reactor 中监控  
2. 子 Reactor 进行客户端通信监控，有事件触发则接收数据并分发给 Worker 线程池  
3. Worker 线程池分配独立线程进行具体业务处理  
   - 工作线程处理完毕后，将响应交给子 Reactor 线程进行数据响应  

#### 优点
- 充分利用 CPU 多核资源  
- 主从 Reactor 各司其职  

---

## One Thread One Loop 主从 Reactor 模型

One Thread One Loop 的思想是：  
**把所有操作都放到一个线程中进行，一个线程对应一个事件循环。**

当前实现中：
- 不提供业务层 Worker 线程池的默认实现
- 只实现主从 Reactor
- 是否引入 Worker 线程池由组件库使用者自行决定

---

## 功能模块划分

### 模块总览

系统主要划分为两大模块：

- **Server 模块（高性能网络服务器）**
- **协议模块**

---

## Server 模块 —— 高性能服务器核心组件

### Buffer 模块

**Buffer 是 Reactor 中的字节缓冲区，负责管理内存、读写指针和空间复用，不关心协议与业务逻辑。**

#### 设计背景

在非阻塞 I/O 模型下：

- `recv()` 可能一次只读取到部分数据，甚至暂时无数据可读  
- `send()` 可能一次只能发送部分数据  

因此，需要一个用户态缓冲区来：

- 暂存从内核读取的数据  
- 缓存尚未完全发送的数据  

#### 核心职责

##### 1. 读写指针管理

Buffer 使用两个指针维护数据状态：

- `_reader_idx`：当前读指针  
- `_writer_idx`：当前写指针  

##### 2. 非阻塞场景下的数据暂存

提供接口：

- `Write()` / `WriteString()`：向缓冲区写入数据  
- `Read()` / `ReadAsString()`：从缓冲区读取并消费数据  
- `GetLine()`：按行读取数据（以 `'\n'` 为分隔）  

支持：

- 多次 `recv()` → 一次业务处理  
- 一次业务生成 → 多次 `send()`  

##### 3. 空间复用与自动扩容

- 优先复用头部已读空间（通过前移可读数据）  
- 空间不足时采用 **倍增扩容策略**  
- 使用 `memmove` 保证重叠内存拷贝的安全性  

---

### Socket 模块

**Socket 是对 TCP 系统调用的最小封装，用于将内核 I/O 行为翻译为上层稳定、可预期的返回语义。**

#### 设计背景

原始系统调用存在大量底层细节：

- `recv()` 返回值需结合 `errno` 判断真实含义  
- `send()` 可能部分发送，甚至触发 `SIGPIPE`  
- 非阻塞 `accept()` 会频繁失败  

Socket 模块的目标是：

> **屏蔽系统调用细节，为上层提供清晰、统一的接口语义**

#### 核心职责

##### 1. TCP 套接字生命周期管理

- 创建 socket  
- bind / listen  
- accept 新连接  
- connect 发起连接  
- close 关闭套接字  

##### 2. 非阻塞 I/O 语义封装

- `EAGAIN`：当前不可读 / 不可写（非阻塞正常情况）  
- `EINTR`：系统调用被信号打断  
- `recv == 0`：对端关闭连接（FIN）  

##### 3. 统一返回值约定

- `Recv()`  
  - `> 0`：成功读取字节数  
  - `= 0`：对端关闭或当前无数据  
  - `< 0`：系统错误  

- `Send()`  
  - `> 0`：成功发送字节数（可能为部分发送）  
  - `= 0`：当前不可写  
  - `< 0`：发送错误  

---

### Channel 模块

**Channel 是对“一个文件描述符（fd）及其关心的 I/O 事件”的抽象封装。**

> 一句话总结：  
> **Channel = fd + 事件类型 + 事件回调**

在 Reactor 模型中：

- Poller 负责 **监控哪些 fd 发生了事件**
- Channel 负责 **事件发生后执行什么逻辑**

#### Channel 的职责

- 保存 fd 关注的事件（EPOLLIN / EPOLLOUT 等）
- 保存实际发生的就绪事件（revents）
- 维护事件对应的回调函数
- 在事件就绪时按优先级触发回调

#### 核心成员说明

| 成员 | 说明 |
|----|----|
| `_fd` | 被监控的文件描述符 |
| `_loop` | 所属 EventLoop |
| `_events` | 关注的事件集合 |
| `_revents` | 实际发生的事件 |
| `_read_cb` | 可读事件回调 |
| `_write_cb` | 可写事件回调 |
| `_error_cb` | 错误事件回调 |
| `_close_cb` | 关闭回调 |
| `_event_cb` | 任意事件统一回调 |

#### 事件管理接口

```cpp
EnableRead();    // 监听读事件
EnableWrite();   // 监听写事件
DisableRead();   // 取消读事件
DisableWrite();  // 取消写事件
DisableAll();    // 取消所有事件
```
### EventLoop 模块
EventLoop 模块负责 **事件监控、就绪事件处理和任务调度**。  
核心职责包括：

- 使用 Poller 监听注册的 Channel 的事件
- 处理就绪事件，调用 Channel 对应回调
- 提供线程安全的任务队列机制
- 提供定时器接口（通过 TimerWheel）

---

### TimerWheel 模块
TimerWheel 是基于 **时间轮思想** 的定时器模块，用于高效管理大量定时任务。  

- 每个时间轮槽位存储多个任务（shared_ptr 管理生命周期）
- 每次 tick 推进清理当前槽位
- 支持任务刷新、取消、添加
- 结合 EventLoop 实现线程安全操作

---

### Connection 模块
Connection 模块封装单个 TCP 连接，负责管理：

- 套接字 I/O
- Channel 事件回调
- 输入/输出缓冲区
- 协议上下文
- 连接状态与定时销毁管理

提供接口包括：

- `Send()`, `ShutDown()`, `EnableInactiveRealse()`
- 设置业务回调：`SetConnectCallBack()`、`SetMsgCallBack()`、`SetCloseCallBack()`、`SetAnyCallBack()`
- 协议升级：`UpGrade()`

---

### Acceptor 模块
Acceptor 模块负责 **监听端口并接收新连接**，并将新连接交给上层处理。  

核心职责：

- 创建监听套接字
- 启动读事件监控
- 触发事件时获取新连接，并回调上层逻辑

接口：

- `Listen()`：启动监听
- `SetAcceptCallBack()`：设置新连接回调

---

### LoopThread 模块
LoopThread 将 EventLoop 与线程整合，实现 **One Thread One Loop** 思路。  

- 每个线程管理一个 EventLoop 实例
- 提供线程安全的获取 EventLoop 接口
- 启动线程后自动运行 EventLoop

---

### LoopThreadPool 模块
LoopThreadPool 管理多个 LoopThread，用于从 Reactor 模型的 **主从线程分配**。  

核心职责：

- 创建和管理从属 LoopThread（工作线程）
- 提供轮转或其他策略的 EventLoop 分配
- 支持 0 个线程（单 Reactor 模型）

用途：

- 主 Reactor 获取新连接，将连接分发给从 Reactor 的 EventLoop 进行事件监控和处理
- 支持多 Reactor 多线程场景，充分利用 CPU 多核资源

---

### TcpServer 模块
TcpServer 是完整的 **高性能 TCP 服务器** 封装，实现主从 Reactor 模型。  

功能：

- 创建主 Reactor（Acceptor + EventLoop）
- 初始化从属 Reactor（LoopThreadPool）
- 提供连接回调接口（新连接、消息、关闭等）
- 对 Connection 进行统一管理
- 支持协议升级、定时销毁非活跃连接

核心流程：

1. 启动主 Reactor，监听端口
2. 接收新连接，通过 LoopThreadPool 分发给从 Reactor
3. 每个从 Reactor 管理 Connection 的读写事件
4. Connection 通过 EventLoop 执行任务、定时器、业务回调