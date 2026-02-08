#include <condition_variable>
#include <unordered_map>
#include <functional>
#include <algorithm>
#include <iostream>
#include <typeinfo>
#include <cassert>
#include <cstring>
#include <vector>
#include <memory>
#include <string>
#include <thread>
#include <mutex>

#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>

// ====================================================================================================
//                                               日志宏模块
// ====================================================================================================

#define INF 0
#define DBG 1
#define ERR 2
#define DEFAULT_LOG_LEVEL INF
#define LOG(level, format, ...)                                                                                           \
    {                                                                                                                     \
        if (level >= DEFAULT_LOG_LEVEL)                                                                                   \
        {                                                                                                                 \
            time_t t = time(NULL);                                                                                        \
            struct tm *m = localtime(&t);                                                                                 \
            char ts[32] = {0};                                                                                            \
            strftime(ts, 31, "%H:%M:%S", m);                                                                              \
            fprintf(stdout, "[%p %s %s:%d] " format "\n", (void *)pthread_self(), ts, __FILE__, __LINE__, ##__VA_ARGS__); \
        }                                                                                                                 \
    }
#define INF_LOG(format, ...) LOG(INF, format, ##__VA_ARGS__);
#define DBG_LOG(format, ...) LOG(DBG, format, ##__VA_ARGS__);
#define ERR_LOG(format, ...) LOG(ERR, format, ##__VA_ARGS__);

// ====================================================================================================
//                                              Buffer模块
// ====================================================================================================

#define DEFAULT_BUFFER_SIZE 1024
class Buffer
{
public:
    Buffer()
        : _buffer(DEFAULT_BUFFER_SIZE)
        , _reader_idx(0)
        , _writer_idx(0)
    { }

    // 当前写指针位置
    char *WritePos()
    {
        return Begin() + _writer_idx;
    }

    // 当前读指针位置
    char *ReadPos()
    {
        return Begin() + _reader_idx;
    }

    // 当前可读数据大小
    uint64_t ReadAbleSize()
    {
        return _writer_idx - _reader_idx;
    }

    // 向后移动读指针（消费数据）
    void MoveReadOffset(uint64_t len)
    {
        assert(len <= ReadAbleSize());
        _reader_idx += len;
    }

    // 向后移动写指针（写入完成后调用）
    void MoveWriteOffset(uint64_t len)
    {
        // 写入空间应已由 EnsureWriteSpace 保证
        assert(len <= TailIdleSize());
        _writer_idx += len;
    }

    // 从缓冲区读取 len 字节到外部缓冲区
    void Read(void *buf, uint64_t len)
    {
        assert(len <= ReadAbleSize());
        std::copy(ReadPos(), ReadPos() + len, static_cast<char *>(buf));
        MoveReadOffset(len);
    }

    // 读取指定长度并以 string 形式返回
    std::string ReadAsString(uint64_t len)
    {
        assert(len <= ReadAbleSize());
        std::string str;
        str.resize(len);
        Read(&str[0], len);
        return str;
    }

    // 写入任意二进制数据
    void Write(const void *data, uint64_t len)
    {
        // 允许空写
        if (len == 0)
            return;
        // 非空写入必须保证数据指针有效
        assert(data != nullptr);
        EnsureWriteSpace(len);
        const char *d = static_cast<const char *>(data);
        std::copy(d, d + len, WritePos());
        MoveWriteOffset(len);
    }

    // 写入字符串内容（不包含结尾 '\0'）
    void WriteString(const std::string &data)
    {
        Write(data.c_str(), data.size());
    }

    // 将另一个 Buffer 的可读数据复制到当前 Buffer
    // 不影响源 Buffer 状态
    void WriteBuffer(Buffer &data)
    {
        Write(data.ReadPos(), data.ReadAbleSize());
    }

    // 将另一个 Buffer 的可读数据写入并消费
    void WriteBufferAndConsume(Buffer &data)
    {
        uint64_t len = data.ReadAbleSize();
        Write(data.ReadPos(), len);
        data.MoveReadOffset(len);
    }

    // [NOTE] FindCRLF 实际查找的是 '\n'，并非严格的 "\r\n"
    // 教学代码中通常简化为按行（line-based）处理
    char *FindCRLF()
    {
        void *res = memchr(ReadPos(), '\n', ReadAbleSize());
        return static_cast<char *>(res);
    }

    // 读取一行数据（包含 '\n'）
    std::string GetLine()
    {
        char *pos = FindCRLF();
        if (pos == nullptr)
            return "";
        return ReadAsString(pos - ReadPos() + 1);
    }

    // 清空缓冲区（逻辑清空，不释放内存）
    void Clear()
    {
        _reader_idx = 0;
        _writer_idx = 0;
    }

    // 确保至少有 len 字节可写空间
    // 优先复用头部空间，其次进行扩容
    void EnsureWriteSpace(uint64_t len)
    {
        // 尾部空间足够，直接写
        if (len <= TailIdleSize())
            return;

        // 通过前移可读数据复用空间
        if (len <= TailIdleSize() + HeadIdleSize())
        {
            uint64_t readable = ReadAbleSize();
            std::memmove(Begin(), ReadPos(), readable);
            /*
             * 这里使用memmove而不使用memcpy或copy
             * “memmove 和 memcpy 的区别在于是否支持内存重叠。在我的 Buffer 实现中，
             * 需要在同一块缓冲区内把可读数据整体前移复用空间，这属于典型的重叠拷贝场景，
             * 所以必须使用 memmove，否则行为是未定义的。”
             */
            _reader_idx = 0;
            _writer_idx = readable;
        }
        else
        {
            // 扩容：采用倍增策略，减少频繁 realloc
            uint64_t new_size = _buffer.size();
            uint64_t need_size = _writer_idx + len;

            while (new_size < need_size)
            {
                new_size *= 2;
            }
            _buffer.resize(new_size);
        }
    }

    ~Buffer()
    { }

private:
    // 返回底层缓冲区起始地址
    char *Begin()
    {
        return _buffer.data();
    }

    // 尾部剩余可写空间大小
    uint64_t TailIdleSize() const
    {
        return _buffer.size() - _writer_idx;
    }

    // 头部已读但尚未复用的空间大小
    uint64_t HeadIdleSize() const
    {
        return _reader_idx;
    }
private:
    std::vector<char> _buffer; // 实际存储空间
    uint64_t _reader_idx;      // 读指针
    uint64_t _writer_idx;      // 写指针
};

// ====================================================================================================
//                                              Socket模块
// ====================================================================================================

#define MAX_LISTEN 20000

// Socket：对 TCP socket 的最小、正确、非阻塞封装
// 职责：
//   1. 封装系统调用（socket / bind / listen / accept / recv / send）
//   2. 正确处理 errno 语义（EAGAIN / EINTR / peer closed）
//   3. 不涉及任何业务逻辑、不做 Buffer 拼包

class Socket
{
public:
    // 构造一个无效 socket
    Socket()
        : _sockfd(-1)
    { }

    // 用已有 fd 构造（常用于 accept 之后）
    Socket(int fd)
        : _sockfd(fd)
    { }

    // 创建 TCP 套接字
    bool CreateSocket()
    {
        // AF_INET      : IPv4
        // SOCK_STREAM  : TCP
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            ERR_LOG("Creat Socket ERR");
            return false;
        }
        _sockfd = fd;
        return true;
    }

    // 绑定 IP + 端口
    bool Bind(uint16_t port, const std::string &ip)
    {
        sockaddr_in server;
        server.sin_family = AF_INET;
        server.sin_port = htons(port); // 主机序 -> 网络序
        server.sin_addr.s_addr = inet_addr(ip.c_str());

        socklen_t len = sizeof(server);
        int ret = bind(_sockfd, (const sockaddr *)&server, len);
        if (ret < 0)
        {
            ERR_LOG("Bind ERR");
            return false;
        }
        return true;
    }

    // 监听套接字，进入 LISTEN 状态
    bool Listen(int backlog = MAX_LISTEN)
    {
        // backlog 是内核对半连接队列 / 全连接队列的容量提示
        int ret = listen(_sockfd, backlog);
        if (ret < 0)
        {
            ERR_LOG("Listen ERR");
            return false;
        }
        return true;
    }

    // 接受新连接
    // 返回值：
    //   >=0 : 新连接 fd
    //   -1  : 当前无可 accept 的连接（EAGAIN / EINTR），或系统错误
    // [FIX-1] EINTR 应重试 accept，而不是直接返回
    int Accept()
    {
        while (true)
        {
            int fd = accept(_sockfd, nullptr, nullptr);
            if (fd >= 0)
                return fd;

            if (errno == EINTR)
                continue; // 被信号打断，重试

            // 非阻塞情况下这里需要返回了，不然会陷入无限自循环
            if (errno == EAGAIN)
                return -1; // 当前无连接（非阻塞正常情况）

            ERR_LOG("Accept ERR");
            return -1;
        }
    }

    // 客户端主动发起连接
    bool Connect(uint16_t port, const std::string &ip)
    {
        sockaddr_in server;
        server.sin_family = AF_INET;
        server.sin_port = htons(port);
        server.sin_addr.s_addr = inet_addr(ip.c_str());

        socklen_t len = sizeof(server);
        int ret = connect(_sockfd, (const sockaddr *)&server, len);
        // EINPROGRESS状态码代表: 握手已经发起了，但还没收到对方的 ACK。我先去忙别的了，你自己盯着点进度。
        // 用于非阻塞轮询
        if (ret < 0 && errno != EINPROGRESS)
        {
            ERR_LOG("Connect ERR");
            return false;
        }
        return true;
    }

    // 接收数据
    // 返回值语义（与 muduo 对齐）：
    //   >0 : 实际读取的字节数
    //    0 : 对端关闭连接
    //   -1 : 发生系统错误，或当前不可读（EAGAIN / EINTR）
    // [FIX-2] 明确区分“对端关闭”和“当前不可读”
    ssize_t Recv(void *buf, size_t len, int flag = 0)
    {
        ssize_t ret = recv(_sockfd, buf, len, flag);
        if (ret < 0)
        {
            // 当前无数据（非阻塞正常）
            if (errno == EINTR || errno == EAGAIN)
                return -1; 

            ERR_LOG("Recv ERR");
            return -1;
        }

        if (ret == 0)
        {
            // [NOTE] 只有 ret==0 才是对端关闭
            INF_LOG("Peer Closed");
            return 0;
        }
        return ret;
    }

    // 非阻塞接收（通过 MSG_DONTWAIT）
    ssize_t NonBlockRecv(void *buf, size_t len)
    {
        // MSG_DONTWAIT非阻塞
        return Recv(buf, len, MSG_DONTWAIT);
    }

    // 发送数据
    // 返回值：
    //   >0 : 实际发送字节数（可能小于 len，属于正常情况）
    //    0 : 当前不可写（EAGAIN / EINTR）
    //   -1 : 发送错误
    ssize_t Send(void *buf, size_t len, int flag = 0)
    {
        // 防止SIGPIPE
        flag |= MSG_NOSIGNAL;
        ssize_t ret = send(_sockfd, buf, len, flag);
        if (ret < 0)
        {
            if (errno == EAGAIN || errno == EINTR)
                return 0;

            ERR_LOG("Send ERR");
            return -1;
        }
        return ret;
    }

    // 非阻塞发送
    ssize_t NonBlockSend(void *buf, size_t len)
    {
        return Send(buf, len, MSG_DONTWAIT);
    }

    // 关闭套接字
    void Close()
    {
        if (_sockfd != -1)
        {
            close(_sockfd);
            _sockfd = -1;
        }
    }

    // 创建服务器监听 socket
    // 顺序：
    // socket -> nonblock -> reuse addr -> bind -> listen
    bool CreateServer(uint16_t port, const std::string &ip = "0.0.0.0", bool isBlock = true)
    {
        if (!CreateSocket())
            return false;

        if (!isBlock)
            SetNonBlock(); // 非阻塞是 Reactor 的前提

        ReuseAddress(); // 支持服务器快速重启

        if (!Bind(port, ip))
            return false;
        if (!Listen())
            return false;

        return true;
    }

    // 创建客户端 socket
    bool CreateClient(uint16_t port, const std::string &ip)
    {
        if (!CreateSocket())
            return false;
        if (!Connect(port, ip))
            return false;
        return true;
    }

    // 开启地址 / 端口复用
    void ReuseAddress()
    {
        int opt = 1;
        setsockopt(_sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(int));
        setsockopt(_sockfd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(int));
    }

    // 设置 socket 为非阻塞（文件状态标志）
    void SetNonBlock()
    {
        int fl = fcntl(_sockfd, F_GETFL, 0);
        if (fl < 0)
        {
            ERR_LOG("F_GETFL ERR");
            return;
        }
        fcntl(_sockfd, F_SETFL, fl | O_NONBLOCK);
    }

    int GetFd() const
    {
        return _sockfd;
    }

    // RAII：对象析构时关闭 fd
    ~Socket()
    {
        if (_sockfd != -1)
            Close();
    }

private:
    int _sockfd; // 套接字文件描述符
};

// ====================================================================================================
//                                                  Channel模块
// ====================================================================================================

// Channel模块是对一个描述符需要进行的IO事件管理的模块
// 实现对描述符可读，可写，错误...事件的管理操作，
// 以及Poller模块对描述符进行IO事件监控就绪后，根据不同的事件，回调不同的处理函数功能。

// 事件的回调函数
class EventLoop;
class Poller;
using EventCallBack = std::function<void()>;

class Channel
{
public:
    // 创建一个channel类
    Channel(EventLoop *loop, int fd)
        : _loop(loop)
        , _fd(fd)
        , _events(0)
        , _revents(0)
        , _tied(false)
    { }
    ~Channel()
    { }
    // 更新
    void Update();
    // 移除监控(从epoll的红黑树上删除掉)
    void Remove();
    // 设置就绪事件--(在后面Poller中调用)
    void SetRevents(uint32_t revents)
    {
        _revents = revents;
    }
    // 获得fd
    int GetFd()
    {
        return _fd;
    }
    // 获得该文件描述符要设置的监控事件
    uint32_t GetEvent()
    {
        return _events;
    }
    // 设置回调函数
    void SetReadCallBack(const EventCallBack &cb)
    {
        _read_cb = cb;
    }
    void SetWriteCallBack(const EventCallBack &cb)
    {
        _write_cb = cb;
    }
    void SetErrorCallBack(const EventCallBack &cb)
    {
        _error_cb = cb;
    }
    void SetCloseCallBack(const EventCallBack &cb)
    {
        _close_cb = cb;
    }
    void SetEventCallBack(const EventCallBack &cb)
    {
        _event_cb = cb;
    }
    // 绑定对象生命周期，避免回调期间对象已析构
    void Tie(const std::shared_ptr<void> &obj)
    {
        _tie = obj;
        _tied = true;
    }
    // 是否监控了读事件
    bool ReadAble()
    {
        return (_events & EPOLLIN);
    }
    // 是否监控了写事件
    bool WriteAble()
    {
        return (_events & EPOLLOUT);
    }
    // 启动读事件监控
    void EnableRead()
    {
        _events |= EPOLLIN;
        Update();
    }
    // 启动写事件监控
    void EnableWrite()
    {
        _events |= EPOLLOUT;
        Update();
    }
    // 解除读事件监控
    void DisableRead()
    {
        _events &= ~EPOLLIN;
        Update();
    }
    // 解除写事件监控
    void DisableWrite()
    {
        _events &= ~EPOLLOUT;
        Update();
    }
    // 关闭所有事件监控
    void DisableAll()
    {
        _events = 0;
        Update();
    }

    // 解决触发事件
    void HandleEvent()
    {
        std::shared_ptr<void> guard;
        if (_tied)
        {
            guard = _tie.lock();
            if (!guard)
                return;
        }

        if (_event_cb)
            _event_cb();

        bool error = false;
        bool closed = false;

        if (_revents & EPOLLERR)
            error = true;

        if (_revents & EPOLLHUP)
            closed = true;

        // 可读事件
        if (_revents & (EPOLLIN | EPOLLPRI))
        {
            if (_read_cb)
                _read_cb();
        }

        // 可写事件
        if (_revents & EPOLLOUT)
        {
            if (_write_cb)
                _write_cb();
        }

        // 错误 / 关闭最后处理
        if (error)
        {
            if (_error_cb)
                _error_cb();
        }
        else if (closed)
        {
            if (_close_cb)
                _close_cb();
        }
    }

private:
    int _fd;
    EventLoop *_loop;
    uint32_t _events;  // 需要监控的事件
    uint32_t _revents; // 实际就绪的事件

    // 事件就绪后的回调函数
    EventCallBack _read_cb;  // 可写
    EventCallBack _write_cb; // 可读
    EventCallBack _error_cb; // 错误产生
    EventCallBack _close_cb; // 连接关闭
    EventCallBack _event_cb; // 任意一个事件触发
    std::weak_ptr<void> _tie;
    bool _tied;
};

// ====================================================================================================
//                                       Poller模块(EventLoop子模块)
// ====================================================================================================

#define MAX_EPOLLEREVENTS 1024
class Poller
{
public:
    // 创建epoll描述符
    Poller()
    {
        _epfd = epoll_create1(EPOLL_CLOEXEC);
        if (_epfd < 0)
        {
            ERR_LOG("Epoll Create ERR");
            abort();
        }
    }

    // 更新一个事件的监控事件或着将一个事件添加到监控中
    void UpdateEvent(Channel *channel)
    {
        int fd = channel->GetFd();
        uint32_t events = channel->GetEvent();

        bool exist = IsExist(channel);

        // epoll不允许内核监控一个文件描述符但是不关心事件
        if (events == 0)
        {
            if (exist)
            {
                _channels.erase(fd);
                Update(channel, EPOLL_CTL_DEL);
            }
            return;
        }

        if (exist)
            Update(channel, EPOLL_CTL_MOD);
        else
        {
            _channels[fd] = channel;
            Update(channel, EPOLL_CTL_ADD);
        }
    }

    // 移除监控
    void RemoveEvent(Channel *channel)
    {
        auto it = _channels.find(channel->GetFd());
        if (it == _channels.end())
            return;

        _channels.erase(channel->GetFd());
        Update(channel, EPOLL_CTL_DEL);
    }

    // 开始监控，返回就绪链接
    void Poll(std::vector<Channel *> *active)
    {
        // int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);
        int nfds = epoll_wait(_epfd, _evs, MAX_EPOLLEREVENTS, -1); // 设置为永久阻塞
        if (nfds < 0)
        {
            if (errno == EINTR)
                return;
            ERR_LOG("Epoll wait ERR");
            abort();
        }

        // nfds就是就绪的事件数量
        // 内核在 events 数组中，写入了 返回值个 epoll_event
        // events[0] ~ events[n-1] 是有效的
        // 每一个 epoll_event 对应一个 就绪的 fd / Channel
        for (int i = 0; i < nfds; i++)
        {
            auto it = _channels.find(_evs[i].data.fd);
            if (it == _channels.end())
                continue; // 忽略已被移除的 fd
            it->second->SetRevents(_evs[i].events);
            active->push_back(it->second);
        }
    }

    ~Poller()
    {
        close(_epfd);
    }

private:
    // [FIX-1] EPOLL_CTL_DEL 时，event 参数必须为 nullptr
    void Update(Channel *channel, int op)
    {
        int fd = channel->GetFd();
        if (op == EPOLL_CTL_DEL)
        {
            if (epoll_ctl(_epfd, EPOLL_CTL_DEL, fd, nullptr) < 0)
            {
                ERR_LOG("Epoll_ctl DEL error: %s", strerror(errno));
            }
            return;
        }

        epoll_event ev;
        ev.events = channel->GetEvent();
        ev.data.fd = fd;

        if (epoll_ctl(_epfd, op, fd, &ev) < 0)
        {
            ERR_LOG("Epoll_ctl ADD/MOD error: %s", strerror(errno));
        }
    }

    // 判断一个channel是否已经添加了事件的监控(是否已经管理)
    bool IsExist(Channel *channel)
    {
        auto it = _channels.find(channel->GetFd());
        if (it == _channels.end())
            return false;
        return true;
    }

private:
    int _epfd;
    struct epoll_event _evs[MAX_EPOLLEREVENTS];
    std::unordered_map<int, Channel *> _channels;
};

// ====================================================================================================
//                                           TimerWheel模块
// ====================================================================================================

class EventLoop;

/*
    时间轮思想
    利用定时器，我们可以看一下每间隔几秒就检查一下链接状况，将不活跃的链接直接断开
    但是如果有成千上万的链接，那每一次都要遍历一次消耗是巨大的，所以我们提出了时间轮的思想

    维护一个数组和一个指针tick，每一秒tick向后移动一次，走到哪里就代表哪里任务应该被执行了
    如果同一时间有多个需要被同时执行的任务则使用下拉数组完成


    如果要时间到了以后自动执行任务，可以将该任务放到一个类的析构函数中，当时间到了以后对象被销毁析构函数自动被执行
    同时如果这个链接在规定的秒数中有活跃操作，则应该刷新它的销毁时间，所以这里可以用shared_ptr,如果
    这个链接在第10s时产生通信则在(10 + 30s)的地方再创建一个shared_ptr对象,这样可以使它内部的引用计数+1
*/

// ===================================
//            定时任务对象
// ===================================

// 定时任务真正要执行的回调函数类型
using TaskFunc = std::function<void()>;

// 定时任务释放时调用的回调（用于从时间轮中移除索引）
using RealseFunc = std::function<void()>;

/*
 * TimerTask 表示一个“定时任务实体”
 * - 生命周期由 shared_ptr 管理
 * - 当最后一个 shared_ptr 被释放时触发析构
 * - 析构中根据是否被取消决定是否执行任务回调
 */


class TimerTask
{
public:
    /*
     * @id       : 定时任务唯一标识
     * @timeout  : 超时时间（秒）
     * @cb       : 定时任务到期后要执行的回调函数
    */
    TimerTask(uint64_t id, uint32_t timeout, const TaskFunc &cb)
        : _id(id),
          _timeout(timeout),
          _task_cb(cb),
          _isCancel(false) // 默认任务未被取消
    { }

    /*
     * 析构函数：
     * - 时间轮中保存该任务的 shared_ptr 被释放时触发
     * - 如果任务未被取消，则执行任务回调
     * - 无论是否取消，都会调用释放回调清理时间轮中的索引
     */
    ~TimerTask()
    {
        // 如果被取消了就不执行任务回调
        if (!_isCancel)
            _task_cb();

        // 通知时间轮移除该任务对应的 weak_ptr 索引
        if (_release_cb)
            _release_cb();
    }

    /*
     * 设置释放回调
     * 该回调由 TimerWheel 提供，用于在任务销毁时
     * 从 _timers 哈希表中移除对应条目
     */
    void SetRealse(const RealseFunc &cb)
    {
        _release_cb = cb;
    }

    // 返回任务的超时时间（用于刷新任务）
    uint32_t DelayTime()
    {
        return _timeout;
    }

    /*
     * 取消定时任务
     * - 并不会立刻删除任务
     * - 只是标记状态，在析构时不再执行任务回调
     */
    void Cancel()
    {
        _isCancel = true;
    }

private:
    uint64_t _id;           // 定时器任务唯一 ID
    uint32_t _timeout;      // 定时任务超时时间（秒）
    TaskFunc _task_cb;      // 定时任务到期要执行的回调
    RealseFunc _release_cb; // 释放回调：用于清理时间轮索引
    bool _isCancel;         // 是否被取消的标志位
};

// ===================================
//               时间轮
// ===================================

// shared_ptr：真正拥有 TimerTask 对象生命周期
using PtrTask = std::shared_ptr<TimerTask>;

// weak_ptr：仅用于索引，不参与生命周期管理
using WeakTask = std::weak_ptr<TimerTask>;

/*
 * TimerWheel：时间轮定时器
 *
 * 核心设计思想：
 * 1. 时间轮槽位（_wheel）使用 shared_ptr 管理任务生命周期
 * 2. _timers 使用 weak_ptr 保存任务索引，避免循环引用
 * 3. tick 每推进一次，就清理当前槽位，触发任务析构
 */
class TimerWheel
{
public:
    TimerWheel(EventLoop *loop)
        : _capacity(60) // 时间轮大小（60 秒一圈）
        , _tick(0) // 当前指针位置
        , _wheel(_capacity) // 初始化时间轮槽位
        , _loop(loop), _timerfd(CreateTimerFd()), _timer_channel(new Channel(_loop, _timerfd))
    {
        _timer_channel->SetReadCallBack(std::bind(&TimerWheel::Ontime, this));
        _timer_channel->EnableRead(); // 启动读事件监控，一旦触发读事件就会调用可读回调函数
    }

    /*
     * 添加定时任务
     * - 创建 TimerTask 对象（shared_ptr）
     * - 设置释放回调，用于任务析构时移除索引
     * - 将任务放入未来 timeout 秒对应的槽位
     * - 用 weak_ptr 保存任务索引
     */
    void TimerAddInLoop(uint64_t id, uint32_t timeout, const TaskFunc &cb)
    {
        // 创建定时任务对象（生命周期由时间轮槽位管理）
        PtrTask pt(new TimerTask(id, timeout, cb));

        // 设置任务析构时的回调，用于从 _timers 中移除
        pt->SetRealse(std::bind(&TimerWheel::RemoveTimer, this, id));

        // 将任务放入未来 timeout 秒对应的槽位
        _wheel[(_tick + timeout) % _capacity].push_back(pt);

        // 用 weak_ptr 保存任务索引（不影响生命周期）
        _timers[id] = WeakTask(pt);
    }

    /*
     * 延迟（刷新）定时任务
     * - 通过 weak_ptr 获取任务对象
     * - 如果任务仍然存在，将其重新放入未来的槽位
     */
    void TimerRefreshInLoop(uint64_t id)
    {
        auto it = _timers.find(id);
        if (it == _timers.end())
            return;

        // 通过 weak_ptr 安全地获取 shared_ptr
        PtrTask pt = it->second.lock();
        if (!pt)
            return;

        // 将任务重新加入未来 DelayTime 秒后的槽位
        _wheel[(_tick + pt->DelayTime()) % _capacity].push_back(pt);
    }

    /*
     * 取消定时任务
     * - 通过 weak_ptr 获取任务对象
     * - 设置取消标志位
     * - 任务仍会在到期时析构，但不会执行任务回调
     */
    void TimerCancelInLoop(uint64_t id)
    {
        auto it = _timers.find(id);
        if (it == _timers.end())
            return;

        PtrTask pt = it->second.lock();
        if (pt)
            pt->Cancel();
    }

    // 如果不想加锁就在一个线程中执行任务
    void TimerAdd(uint64_t id, uint32_t timeout, const TaskFunc &cb);
    void TimerRefresh(uint64_t id);
    void TimerCancel(uint64_t id);

    // 有线程安全问题，只能在组件内执行
    bool HasTimer(uint64_t id)
    {
        auto it = _timers.find(id);
        if (it == _timers.end())
            return false;
        return true;
    }

    /*
     * 时间轮推进（每秒调用一次）
     * - tick 前进一格
     * - 清空当前槽位
     * - 槽位中的 shared_ptr 被释放，触发 TimerTask 析构
     */
    void Run()
    {
        _tick = (_tick + 1) % _capacity;

        // 清空当前槽位，触发定时任务析构
        _wheel[_tick].clear();
    }

private:
    /*
     * 移除定时任务索引
     * - 由 TimerTask 析构时调用
     * - 清理 _timers 中对应的 weak_ptr
     */
    void RemoveTimer(uint64_t id)
    {
        auto it = _timers.find(id);
        if (it != _timers.end())
            _timers.erase(it);
    }

    static int CreateTimerFd()
    {
        int timerfd = timerfd_create(CLOCK_MONOTONIC, 0); // 默认阻塞操作
        if (timerfd < 0)
        {
            ERR_LOG("Create timerfd failed");
            abort();
        }

        struct itimerspec itime;

        itime.it_value.tv_sec = 1;  // 此处为设置超时时间3s
        itime.it_value.tv_nsec = 0; // 防止纳秒变成随机值设为0

        itime.it_interval.tv_sec = 1; // 第一次超时后，每次超时的间隔时间
        itime.it_interval.tv_nsec = 0;

        timerfd_settime(timerfd, 0, &itime, nullptr);
        return timerfd;
    }

    void ReadTimerFd()
    {
        uint64_t exp;
        int ret = read(_timerfd, &exp, sizeof(exp));
        if (ret < 0)
        {
            ERR_LOG("Read TimerFd failed");
            abort();
        }
        return;
    }

    void Ontime()
    {
        ReadTimerFd();
        Run();
    }

private:
    int _tick;     // 当前时间指针（秒针）
    int _capacity; // 时间轮容量

    // 时间轮槽位，每个槽位保存多个定时任务（shared_ptr）
    std::vector<std::vector<PtrTask>> _wheel;
    // 任务索引表：id -> weak_ptr，不参与生命周期管理
    std::unordered_map<uint64_t, WeakTask> _timers;

    int _timerfd;     // 定时器描述符
    EventLoop *_loop; // 需要对文件描述符进行事件监控
    std::unique_ptr<Channel> _timer_channel;
};

// ====================================================================================================
//                                              EventLoop模块
// ====================================================================================================

// 1.对事件进行监控 2.就绪事件处理 3.执行任务
class EventLoop
{
public:
    using Functor = std::function<void()>;
    EventLoop()
        : _thread_id(std::this_thread::get_id())
        , _eventfd(CreateEventFd())
        , _eventfd_channel(new Channel(this, _eventfd))
        , _timer_wheel(this)
    {
        _eventfd_channel->SetReadCallBack(std::bind(&EventLoop::ReadEventFd, this));
        _eventfd_channel->EnableRead(); // 启动对读事件的监控
    }

    // 启动eventloop
    void Start()
    {
        while (1)
        {
            std::vector<Channel *> actives;
            // 事件监控
            _poll.Poll(&actives);

            // 事件处理
            for (auto &ch : actives)
                ch->HandleEvent();

            // 执行任务(将任务队列中的任务全部执行一次)
            RunAllTasks();
        }
    }

    void RunInLoop(const Functor &cb)
    {
        // 如果这个任务执行与当前线程相同，直接执行
        if (IsInLoop())
            return cb();

        return QueueInLoop(cb);
    }

    void AssertInLoop()
    {
        assert(_thread_id == std::this_thread::get_id());
    }

    bool IsInLoop()
    {
        return (_thread_id == std::this_thread::get_id() ? true : false);
    }

    void QueueInLoop(const Functor &cb)
    {
        {
            std::unique_lock<std::mutex> _lock(_mtx);
            _tasks.push_back(cb);
        }
        // 唤醒有可能因为事件没有就绪而导致的阻塞 -> 给eventfd写一个数据，触发eventfd的可读事件就绪
        WakeUpEventFd();
    }

    void UpdateEvent(Channel *channel)
    {
        return _poll.UpdateEvent(channel);
    }

    void RemoveEvent(Channel *channel)
    {
        return _poll.RemoveEvent(channel);
    }

    // 添加定时器任务
    void TimerAdd(uint64_t id, uint32_t timeout, const TaskFunc &cb)
    {
        return _timer_wheel.TimerAdd(id, timeout, cb);
    }

    // 刷新定时器任务
    void TimerRefresh(uint64_t id)
    {
        return _timer_wheel.TimerRefresh(id);
    }

    // 取消定时器任务
    void TimerCancel(uint64_t id)
    {
        return _timer_wheel.TimerCancel(id);
    }

    // 检查定时器任务是否存在
    bool HasTimer(uint64_t id)
    {
        return _timer_wheel.HasTimer(id);
    }

private:
    static int CreateEventFd()
    {
        int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK); // 设置初始计数器为0， 禁止子进程复制，非阻塞
        if (efd < 0)
        {
            ERR_LOG("Eventfd ERR");
            abort();
        }
        return efd;
    }

    // ============ eventfd每次读写大小都是8字节 ============
    // 从eventfd中读取通知次数
    void ReadEventFd()
    {
        uint64_t res = 0;
        int ret = read(_eventfd, &res, sizeof(res));
        if (ret < 0)
        {
            if (errno == EINTR) // 被信号打断
                return;
            ERR_LOG("Read Eventfd Failed");
            abort();
        }
        return;
    }

    // 向eventfd写入一个值
    void WakeUpEventFd()
    {
        uint64_t val = 1;
        int ret = write(_eventfd, &val, sizeof(val));
        if (ret < 0)
        {
            if (errno == EINTR || errno == EAGAIN) // 被信号打断
                return;
            ERR_LOG("Read Eventfd Failed");
            abort();
        }
        return;
    }

    // 执行任务队列中的任务
    void RunAllTasks()
    {
        std::vector<Functor> functor;
        {
            std::unique_lock<std::mutex> _lock(_mtx); // 用花括号限定作用域，出了作用域锁会自动释放
            _tasks.swap(functor);                     // 清空现有的任务队列，并执行任务
        }
        for (auto &e : functor)
            e();
    }

private:
    std::thread::id _thread_id;                // 判断回调的任务在不在当前线程中，如果在当前线程就直接执行，如果不在就添加到任务队列中
    std::mutex _mtx;                           // 给任务队列加的锁
    Poller _poll;                              // 对事件进行监控
    int _eventfd;                              // 用于解决监控IO事件阻塞导致任务队列中的任务无法执行的错误
    std::unique_ptr<Channel> _eventfd_channel; // 管理enventfd
    std::vector<Functor> _tasks;               // 任务队列
    TimerWheel _timer_wheel;                   // 定时器模块
};

void Channel::Update()
{
    _loop->UpdateEvent(this);
}
void Channel::Remove()
{
    _loop->RemoveEvent(this);
}

void TimerWheel::TimerAdd(uint64_t id, uint32_t timeout, const TaskFunc &cb)
{
    _loop->RunInLoop(std::bind(&TimerWheel::TimerAddInLoop, this, id, timeout, cb));
}

void TimerWheel::TimerRefresh(uint64_t id)
{
    _loop->RunInLoop(std::bind(&TimerWheel::TimerRefreshInLoop, this, id));
}

void TimerWheel::TimerCancel(uint64_t id)
{
    _loop->RunInLoop(std::bind(&TimerWheel::TimerCancelInLoop, this, id));
}

// ====================================================================================================
//                                          Connection模块
// ====================================================================================================

// ==============================================
//                   Any类--存上下文的
// ==============================================

class Connection;
class TcpServer;
class Any
{
public:
    Any()
        : _content(nullptr)
    {
    }

    template <class T>
    Any(const T &val)
        : _content(new PlaceHolder<T>(val)) // !!!!! 在这里: PlaceHolder = Holder! !!!!!!!
    {
    }

    // 拷贝构造
    Any(const Any &other)
        : _content(other._content ? other._content->clone() : nullptr)
    {
    }

    ~Any()
    {
        delete _content;
    }

    Any &swap(Any &other)
    {
        std::swap(_content, other._content);
        return *this;
    }
    // 获取子类对象中保存数据的指针
    template <class T>
    T *Get()
    {
        assert(typeid(T) == _content->type());
        return &((PlaceHolder<T> *)_content)->_val;
    }

    // 赋值运算符重载
    template <class T>
    Any &operator=(const T &val)
    {
        // 创建临时对象，交换以后自动销毁
        Any(val).swap(*this);
        return *this;
    }

    Any &operator=(const Any &other)
    {
        Any(other).swap(*this);
        return *this;
    }

private:
    // 父类
    class Holder
    {
    public:
        virtual ~Holder() {}
        virtual const std::type_info &type() = 0; // 纯虚函数, 必须要在子类中实现
        virtual Holder *clone() = 0;
    };
    // 子类
    template <class T>
    class PlaceHolder : public Holder // 子类继承父类，那么子类就可以当作一个父类来使用
    {
    public:
        PlaceHolder(const T &val)
            : _val(val)
        {
        }

        virtual const std::type_info &type() // 获取子类对象保存类型
        {
            return typeid(T);
        }

        virtual Holder *clone() // 针对对象自身，克隆出一个新的子类对象
        {
            return new PlaceHolder(_val);
        }

    public:
        T _val;
    };
    Holder *_content;
};

typedef enum
{
    CONNECTED,    // 连接已经建立完成
    CONNECTING,   // 正在建立连接
    DISCONNECTED, // 已经断开连接
    DISCONNECTING // 正在断开连接
} ConnectState;

// 业务回调
using ConnectionPtr = std::shared_ptr<Connection>;
using ConnectedCallBack = std::function<void(const ConnectionPtr &)>;
using MessageCallBack = std::function<void(const ConnectionPtr &, Buffer *)>;
using ClosedCallBack = std::function<void(const ConnectionPtr &)>;
using AnyCallBack = std::function<void(const ConnectionPtr &)>;

class Connection : public std::enable_shared_from_this<Connection>
{
public:
    Connection(EventLoop *loop, uint64_t connect_id, int fd)
        : _loop(loop)
        , _connect_id(connect_id)
        , _sockfd(fd)
        , _channel(loop, fd)
        , _enable_inactive_release(false)
        , _state(CONNECTING), _socket(fd)
    {
        _channel.SetCloseCallBack(std::bind(&Connection::HandleClose, this));
        _channel.SetErrorCallBack(std::bind(&Connection::HandleError, this));
        _channel.SetReadCallBack(std::bind(&Connection::HandleRead, this));
        _channel.SetEventCallBack(std::bind(&Connection::HandleEvent, this));
        _channel.SetWriteCallBack(std::bind(&Connection::HandleWrite, this));
    }

    // 绑定自身生命周期到Channel，避免事件回调期间对象被析构
    void Tie(const ConnectionPtr &self)
    {
        _channel.Tie(self);
    }

    // 发送数据
    void Send(const char *data, size_t len)
    {
        // 这里的data可能是外部临时变量，该任务被压入任务队列，如果外部空间释放会导致空指针引用
        Buffer buf;
        buf.Write(data, len);
        auto self = shared_from_this();
        _loop->RunInLoop([self, buf]()
                         { self->SendInLoop(buf); });
    }

    // 提供给组件使用者的关闭接口 -- 并不实际关闭，需要判断有咩有数据待处理
    void ShutDown()
    {
        auto self = shared_from_this();
        _loop->RunInLoop([self]()
                         { self->ShutDownInLoop(); });
    }

    // 启动非活跃销毁，参数是多长时间无通信销毁
    void EnableInactiveRealse(int sec)
    {
        auto self = shared_from_this();
        _loop->RunInLoop([self, sec]()
                         { self->EnableInactiveRealseInLoop(sec); });
    }

    // 取消非活跃销毁
    void DisableInactiveRealse()
    {
        auto self = shared_from_this();
        _loop->RunInLoop([self]()
                         { self->DisableInactiveRealseInLoop(); });
    }

    // 切换协议，重置上下文和阶段性处理函数 -- 非线程安全的
    void UpGrade(const Any &context, const ConnectedCallBack &conn, const MessageCallBack &msg, const ClosedCallBack &close, const AnyCallBack &any)
    {
        _loop->AssertInLoop();
        auto self = shared_from_this();
        _loop->RunInLoop([self, context, conn, msg, close, any]()
                         { self->UpGradeInLoop(context, conn, msg, close, any); });
    }

    // 获取文件描述符
    int GetFd()
    {
        return _sockfd;
    }

    // 获取id
    uint64_t GetId()
    {
        return _connect_id;
    }

    void Established()
    {
        auto self = shared_from_this();
        _loop->RunInLoop([self]()
                         { self->EstablishedInLoop(); });
    }

    // 返回状态
    bool IsConnected()
    {
        return _state == CONNECTED;
    }

    // 设置上下文
    void SetContext(const Any &context)
    {
        _context = context;
    }

    // 返回获得上下文
    Any *GetContext()
    {
        return &_context;
    }

    void SetConnectCallBack(const ConnectedCallBack &cb)
    {
        _connect_cb = cb;
    }
    void SetMsgCallBack(const MessageCallBack &cb)
    {
        _msg_cb = cb;
    }
    void SetCloseCallBack(const ClosedCallBack &cb)
    {
        _close_cb = cb;
    }
    void SetAnyCallBack(const AnyCallBack &cb)
    {
        _any_cb = cb;
    }

    void SetServerCloseCallBack(const ClosedCallBack &cb)
    {
        _server_close_cb = cb;
    }

    ~Connection()
    {
        DBG_LOG("Realse Connection");
    }

private:
    // 保证线程安全啊bro
    void SendInLoop(Buffer buf)
    {
        // 不是真正发送数据，而是将数据放到发送缓冲区中
        if (_state == DISCONNECTED)
            return;

        _outBuffer.WriteBuffer(buf);

        if (_channel.WriteAble() == false)
            _channel.EnableWrite();
    }

    void ShutDownInLoop()
    {
        // 也不是实际释放连接操作, 需要判断还有没有数据待处理和发送
        _state = DISCONNECTING; // 设置待关闭状态
        if (_inBuffer.ReadAbleSize() > 0)
        {
            if (_msg_cb)
            {
                _msg_cb(shared_from_this(), &_inBuffer);
            }
        }
        if (_outBuffer.ReadAbleSize() > 0)
        {
            // 有数据待发送
            if (_channel.WriteAble() == false)
                _channel.EnableWrite();
        }

        if (_outBuffer.ReadAbleSize() == 0)
            ReleaseInLoop();
    }

    void ReleaseInLoop()
    {
        // 修改链接状态
        _state = DISCONNECTED;
        // 移除连接的事件监控
        _channel.Remove();
        // 关闭描述符
        _socket.Close();
        // 如果定时器中还有定时销毁任务则取消任务
        if (_loop->HasTimer(_connect_id))
            DisableInactiveRealseInLoop();

        if (_close_cb)
        {
            _close_cb(shared_from_this());
        }

        if (_server_close_cb)
        {
            _server_close_cb(shared_from_this());
        }
    }

    // 连接获取后的状态下需要进行各种设置的状态: 设置事件回调，启动读监控
    void EstablishedInLoop()
    {
        assert(_state == CONNECTING);
        _state = CONNECTED;
        _channel.EnableRead();
        if (_connect_cb)
        {
            _connect_cb(shared_from_this());
        }
    }

    void EnableInactiveRealseInLoop(int sec)
    {
        _enable_inactive_release = true;
        // 添加定时销毁任务, 如果任务已经存在, 则刷新延迟即可, 如果不存在则新增
        if (_loop->HasTimer(_connect_id) == true)
            _loop->TimerRefresh(_connect_id);
        else
        {
            // 任务不存在，新增任务
            auto self = shared_from_this();
            _loop->TimerAdd(_connect_id, sec, [self]()
                            { self->ReleaseInLoop(); });
        }
    }

    void DisableInactiveRealseInLoop()
    {
        _enable_inactive_release = false;
        if (_loop->HasTimer(_connect_id) == true)
            _loop->TimerCancel(_connect_id);
    }

    void UpGradeInLoop(const Any &context, const ConnectedCallBack &conn, const MessageCallBack &msg, const ClosedCallBack &close, const AnyCallBack &any)
    {
        _context = context;
        _connect_cb = conn;
        _msg_cb = msg;
        _close_cb = close;
        _any_cb = any;
    }

    // Channel的五个回调函数
    void HandleRead()
    {
        // 读取Socket中的数据放到缓冲区
        char buffer[65536];
        bool peer_closed = false;

        while (true)
        {
            // 如果socket缓冲区中没有数据可能导致阻塞，所以这里用非阻塞读取
            ssize_t ret = _socket.NonBlockRecv(buffer, sizeof(buffer));
            if (ret > 0)
            {
                _inBuffer.Write(buffer, ret);
                continue;
            }

            if (ret == 0)
            {
                peer_closed = true;
                break;
            }

            if (errno == EAGAIN || errno == EINTR)
                break;

            // 发生错误
            HandleClose();
            return;
        }

        // 将数据放到输入缓冲区
        if (peer_closed)
        {
            HandleClose();
            return;
        }

        // 调用msg_cb进行业务处理
        if (_inBuffer.ReadAbleSize() > 0)
        {
            if (_msg_cb)
            {
                _msg_cb(shared_from_this(), &_inBuffer);
            }
        }
    }

    void HandleWrite()
    {

        // 触发写事件，socket可以发送数据
        ssize_t ret = _socket.NonBlockSend(_outBuffer.ReadPos(), _outBuffer.ReadAbleSize());
        if (ret < 0)
        {
            if (_inBuffer.ReadAbleSize() > 0)
            {
                if (_msg_cb)
                    _msg_cb(shared_from_this(), &_inBuffer);
            }
            ReleaseInLoop();
            return;
        }
        else if (ret == 0)
            return;

        _outBuffer.MoveReadOffset(ret);

        if (_outBuffer.ReadAbleSize() == 0)
        {
            _channel.DisableWrite();
            if (_state == DISCONNECTING)
            {
                ReleaseInLoop();
                return;
            }
        }
        return;
    }

    // 连接一旦挂断，套接字像个无能的丈夫，什么都干不了
    void HandleClose()
    {
        if (_inBuffer.ReadAbleSize() > 0)
        {
            if (_msg_cb)
                _msg_cb(shared_from_this(), &_inBuffer);
        }
        ReleaseInLoop();
    }

    void HandleError()
    {
        HandleClose();
    }

    void HandleEvent()
    {
        // 刷新连接的活跃度 - 延迟定时销毁任务
        if (_enable_inactive_release == true)
            _loop->TimerRefresh(_connect_id);

        // 调用组件使用者的事件回调
        if (_any_cb)
        {
            _any_cb(shared_from_this());
        }
    }

private:
    int _sockfd;                   // 关联文件描述符
    bool _enable_inactive_release; // 是否启动销毁非活跃链接
    uint64_t _connect_id;          // 连接唯一id -- 为了简化操作，可以同时作为定时器id
    ConnectState _state;           // 链接状态管理
    Socket _socket;                // 套接字操作管理
    Channel _channel;              // 连接事件管理
    Buffer _inBuffer;              // 输入缓冲区
    Buffer _outBuffer;             // 输出缓冲区
    Any _context;                  // 管理协议上下文
    EventLoop *_loop;              // 链接所关联的loop - 关联到线程

    // 业务处理
    ConnectedCallBack _connect_cb;
    MessageCallBack _msg_cb;
    ClosedCallBack _close_cb;
    AnyCallBack _any_cb;
    // 移除服务器内部的管理信息
    ClosedCallBack _server_close_cb;
};

// ====================================================================================================
//                                          Acceptor模块
// ====================================================================================================

// 用于管理监听套接字进行管理
// 1.创建一个监听套接字
// 2.启动读事件监控
// 3.事件触发后获取新链接
using AcceptCallBack = std::function<void(int)>;

class Acceptor
{
public:
    Acceptor(EventLoop *loop, uint16_t port)
        : _socket()
        , _loop(loop)
        , _channel(nullptr)
    {
        bool ret = _socket.CreateServer(port, "0.0.0.0", false);
        assert(ret == true);
        _channel = std::make_unique<Channel>(_loop, _socket.GetFd());
        _channel->SetReadCallBack(std::bind(&Acceptor::HandleRead, this));
    }

    void Listen()
    {
        _channel->EnableRead();
    }

    void SetAcceptCallBack(const AcceptCallBack &cb)
    {
        _accept_callback = cb;
    }

private:
    // 获取新连接，调用回调函数
    void HandleRead()
    {
        int newfd = _socket.Accept();
        if (newfd < 0)
            return;
        if (_accept_callback)
            _accept_callback(newfd);
    }

private:
    Socket _socket;                    // 用于创建监听套接字
    EventLoop *_loop;                  // 用于对监听套接字进行事件监控
    std::unique_ptr<Channel> _channel; // 管理监听套接字
    AcceptCallBack _accept_callback;
};

// ====================================================================================================
//                                          LoopThread模块
// ====================================================================================================

// 目标: 将eventloop模块与线程整合起来
// EventLoop 与线程是一一对应的，当eventloop构造的时候就会初始化线程id

// eventloop模块在实例化对象的时候必须要在线程内部, 因此必须要先创建线程，然后在线程的函数入口中去实例化eventloop对象

class LoopThread
{
public:
    // 创建线程，设定线程入口函数
    LoopThread()
        : _loop(nullptr)
        , _thread(std::thread(&LoopThread::ThreadEntry, this))
    { }

    EventLoop *GetLoop()
    {
        EventLoop *loop = nullptr;
        {
            std::unique_lock<std::mutex> lock(_mutex); // 加锁，loop为空就循环阻塞
            _cond.wait(lock, [&]()
                       { return _loop != nullptr; }); // 唤醒_cond上可能阻塞的线程
            loop = _loop;
        }
        return loop;
    }

private:
    // 实例化eventloop对象，启动eventloop模块
    void ThreadEntry()
    {
        EventLoop loop;
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _loop = &loop;

            // 唤醒等待的线程
            _cond.notify_all();
        }
        // 启动loop
        loop.Start();
    }

private:
    EventLoop *_loop;    // 要在线程内实例化
    std::thread _thread; // eventloop对应线程

    // 用于实现_loop获取的同步关系，避免loop创建了但是还没有实例化之前就被获取
    std::mutex _mutex;
    std::condition_variable _cond;
};

// ====================================================================================================
//                                        LoopThreadPool模块
// ====================================================================================================

// 对所有的loopthread进行管理和分配
// 功能:
// 1. 核心线程数量
// --- 注意事项 ---
// 在服务器中，主从Reactor模型是主线程只负责新连接获取，从属线程负责新连接的事件监控和处理
// 因此当前从属线程数量可能为0， 也就是单Reactor服务器，一个线程负责获取连接，也负责连接的处理

// 2. 对所有的线程进行管理，管理0个或多个LoopThread对象
// 3. 提供线程分配的功能, 主线程获得一个新连接，需要将新连接挂到从属线程上进行事件监控及处理
// 如果从属线程为0，则都由主线程处理，如果有多个线程则使用RR轮转思想进行线程分配(将对应的EventLoop获取到分配到Connection)

class LoopThreadPool
{
public:
    LoopThreadPool(EventLoop *loop)
        : _thread_count(0)
        , _thread_index(0)
        , _base_loop(loop)
    { }

    void SetThreadCount(int cnt)
    {
        _thread_count = cnt;
    }

    // 创建所有的从属线程
    void CreateThread()
    {
        if (_thread_count > 0)
        {
            _threads.resize(_thread_count);
            _loops.resize(_thread_count);
            for (int i = 0; i < _thread_count; i++)
            {
                _threads[i] = new LoopThread();
                _loops[i] = _threads[i]->GetLoop();
            }
        }
    }

    EventLoop *GetLoop()
    {
        // 轮转派发loop
        if (_thread_count == 0)
            return _base_loop;

        _thread_index = (_thread_index + 1) % _thread_count;
        return _loops[_thread_index];
    }

private:
    int _thread_count; // 从属线程的数量
    int _thread_index;
    EventLoop *_base_loop;              // 主EventLoop
    std::vector<LoopThread *> _threads; // 保存所有Loopthread对象
    std::vector<EventLoop *> _loops;
};

// ====================================================================================================
//                                        TcpServer模块
// ====================================================================================================

// 管理
// 1. Acceptor对象，创建监听套接字
// 2. EventLoop对象，baseLoop对象，实现对监听套接字的事件监控
// 3. std::unordered_map<int, ConnectionPtr> connections 管理新连接
// 4. LoopTreadPool 创建loop线程池，对新建连接进行事件监控及处理

// 功能
// 1. 设置从属线程池数量
// 2. 启动服务器
// 3. 设置业务回调函数(连接建立完成，消息，关闭，任意)
// 4. 是否启动非活跃超时销毁
// 5. 添加定时任务

// 定时任务
using Functor = std::function<void()>;

class TcpServer
{
public:
    TcpServer(uint16_t port)
        : _port(port)
        , _con_timer_id(0)
        , _enable_inactive_release(false)
        , _acceptor(&_baseloop, _port)
        , _pool(&_baseloop)
    {
        _acceptor.SetAcceptCallBack(std::bind(&TcpServer::NewConnection, this, std::placeholders::_1));
        _acceptor.Listen(); // 开始关心事件
    }

    // 启动服务器
    void Start()
    {
        _pool.CreateThread(); // 创建从属线程
        _baseloop.Start();    // 启动监听
    }

    void SetThreadCount(int cnt)
    {
        _pool.SetThreadCount(cnt);
    }

    void EnableInactiveRealse(int timeout)
    {
        _timeout = timeout;
        _enable_inactive_release = true;
    }

    // 设置延迟任务, 用于添加用户需要的定时任务
    void SetDelayTask(const Functor &func, int timeout)
    {
        _baseloop.RunInLoop(std::bind(&TcpServer::SetDelayTaskInLoop, this, func, timeout));
    }

    // 设置Connection回调函数
    void SetConnectCallBack(const ConnectedCallBack &cb)
    {
        _connect_cb = cb;
    }

    void SetMsgCallBack(const MessageCallBack &cb)
    {
        _msg_cb = cb;
    }

    void SetCloseCallBack(const ClosedCallBack &cb)
    {
        _close_cb = cb;
    }

    void SetAnyCallBack(const AnyCallBack &cb)
    {
        _any_cb = cb;
    }

    void SetServerCloseCallBack(const ClosedCallBack &cb)
    {
        _server_close_cb = cb;
    }

private:
    // 为新连接创建一个Connetion进行管理
    void NewConnection(int newfd)
    {
        _con_timer_id++;

        ConnectionPtr conn(new Connection(_pool.GetLoop(), _con_timer_id, newfd));
        conn->Tie(conn); // 绑定自身弱引用，避免并发释放阶段shared_from_this异常
        conn->SetCloseCallBack(_server_close_cb);
        conn->SetConnectCallBack(_connect_cb);
        conn->SetMsgCallBack(_msg_cb);
        conn->SetAnyCallBack(_any_cb);
        conn->SetServerCloseCallBack(std::bind(&TcpServer::RemoveConnection, this, std::placeholders::_1));

        // 启动非活跃连接销毁
        if (_enable_inactive_release)
            conn->EnableInactiveRealse(_timeout);

        conn->Established();
        _connnections.emplace(_con_timer_id, conn);
    }

    // 从管理connections中移除连接信息
    void RemoveConnection(const ConnectionPtr &conn)
    {
        _baseloop.RunInLoop(std::bind(&TcpServer::RemoveConnectionInLoop, this, conn));
    }

    void RemoveConnectionInLoop(const ConnectionPtr &conn)
    {
        int id = conn->GetId();
        auto it = _connnections.find(id);
        if (it != _connnections.end())
            _connnections.erase(id);
    }

    void SetDelayTaskInLoop(const Functor &func, int timeout)
    {
        _con_timer_id++;
        _baseloop.TimerAdd(_con_timer_id, timeout, func);
    }

private:
    uint16_t _port;
    uint64_t _con_timer_id;        // 自动增长的连接id
    int _timeout;                  // 非活跃连接的统计时间
    bool _enable_inactive_release; // 是否启动
    EventLoop _baseloop;           // 主线程
    Acceptor _acceptor;            // 监听套接字

    LoopThreadPool _pool;                                      // 从属loop线程池
    std::unordered_map<uint64_t, ConnectionPtr> _connnections; // 保存所有连接的Shared_ptr对象

    // 业务处理
    ConnectedCallBack _connect_cb;
    MessageCallBack _msg_cb;
    ClosedCallBack _close_cb;
    AnyCallBack _any_cb;
    // 移除服务器内部的管理信息
    ClosedCallBack _server_close_cb;
};