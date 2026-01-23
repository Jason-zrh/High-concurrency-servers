#include <iostream>
#include <vector>
#include <cassert>
#include <algorithm>
#include <string>
#include <cstring>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>   
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include "log.hpp"

extern Log lg;

// ================================================================
//                            Buffer模块
// ================================================================
#define DEFAULT_BUFFER_SIZE 1024

class Buffer
{
public:
    Buffer()
        : _buffer(DEFAULT_BUFFER_SIZE)
        , _reader_idx(0)
        , _writer_idx(0)
    { }

    // 返回底层缓冲区起始地址
    char *Begin()
    {
        return _buffer.data();
    }

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

    // 尾部剩余可写空间大小
    uint64_t TailIdleSize()
    {
        return _buffer.size() - _writer_idx;
    }

    // 头部已读但尚未复用的空间大小
    uint64_t HeadIdleSize()
    {
        return _reader_idx;
    }

    // 当前可读数据大小
    uint64_t ReadAbleSize()
    {
        return _writer_idx - _reader_idx;
    }

    // 向后移动读指针（消费数据）
    void MoveReadOffset(uint64_t len)
    {
        _reader_idx += len;
    }

    // 向后移动写指针（写入完成后调用）
    void MoveWriteOffset(uint64_t len)
    {
        // 写入空间应已由 EnsureWriteSpace 保证
        assert(len <= TailIdleSize());
        _writer_idx += len;
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
            /* 这里使用memmove而不使用memcpy或copy
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

    // 从缓冲区读取 len 字节到外部缓冲区
    void Read(void *buf, uint64_t len)
    {
        assert(len <= ReadAbleSize());
        std::copy(ReadPos(), ReadPos() + len, static_cast<char *>(buf));
        MoveReadOffset(len);
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

    // 读取指定长度并以 string 形式返回
    std::string ReadAsString(uint64_t len)
    {
        assert(len <= ReadAbleSize());
        std::string str;
        str.resize(len);
        Read(&str[0], len);
        return str;
    }

    // 查找当前可读区中的 '\n'
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

    ~Buffer() {}

private:
    std::vector<char> _buffer; // 实际存储空间
    uint64_t _reader_idx;      // 读指针
    uint64_t _writer_idx;      // 写指针
};


// ================================================================
//                            Socket模块
// ================================================================
#define MAX_LISTEN 1024

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
        if(fd < 0)
        {
            lg(Fatal, "Create socket error");
            return false;
        }
        _sockfd = fd;
        return true;
    }

    // 绑定 IP + 端口
    bool Bind(uint16_t port, const std::string& ip)
    {
        sockaddr_in server;
        server.sin_family = AF_INET;
        server.sin_port   = htons(port);              // 主机序 -> 网络序
        server.sin_addr.s_addr = inet_addr(ip.c_str());

        socklen_t len = sizeof(server);
        int ret = bind(_sockfd, (const sockaddr*)&server, len);
        if(ret < 0)
        {
            lg(Fatal, "Bind error");
            return false;
        }
        return true;
    }

    // 监听套接字，进入 LISTEN 状态
    bool Listen(int backlog = MAX_LISTEN)
    {
        // backlog 是内核对半连接队列 / 全连接队列的容量提示
        int ret = listen(_sockfd, backlog);
        if(ret < 0)
        {
            lg(Error, "Listen error");
            return false;
        }
        return true;
    }

    // 接受新连接
    // 返回值：
    //   >=0 : 新连接 fd
    //   -1  : 当前无可 accept 的连接（EAGAIN / EINTR），或系统错误
    int Accept()
    {
        // 非阻塞 listen fd 下，accept 可能频繁返回 EAGAIN
        int fd = accept(_sockfd, nullptr, nullptr);
        if(fd < 0)
        {
            // 非异常情况：当前无连接或被信号中断
            if(errno == EAGAIN || errno == EINTR)
                return -1;

            // 真正的系统错误
            lg(Error, "Accept error");
            return -1;
        }
        return fd;
    }

    // 客户端主动发起连接
    bool Connect(uint16_t port, const std::string& ip)
    {
        sockaddr_in server;
        server.sin_family = AF_INET;
        server.sin_port   = htons(port);
        server.sin_addr.s_addr = inet_addr(ip.c_str());

        socklen_t len = sizeof(server);
        int ret = connect(_sockfd, (const sockaddr*)&server, len);
        if(ret < 0)
        {
            lg(Error, "Connect error");
            return false;
        }
        return true;
    }

    // 接收数据
    // 返回值语义（与 muduo 对齐）：
    //   >0 : 实际读取的字节数
    //    0 : 对端关闭连接，或当前不可读（EAGAIN / EINTR）
    //   -1 : 发生系统错误
    ssize_t Recv(void* buf, size_t len, int flag = 0)
    {
        ssize_t ret = recv(_sockfd, buf, len, flag);
        if(ret < 0)
        {
            // 非阻塞下的正常情况
            if(errno == EINTR || errno == EAGAIN)
                return 0;

            lg(Error, "Recv error");
            return -1;
        }

        // ret == 0 表示对端发送 FIN，连接关闭
        if(ret == 0)
        {
            lg(Info, "Peer closed");
            return 0;
        }

        return ret;
    }

    // 非阻塞接收（通过 MSG_DONTWAIT）
    ssize_t NonBlockRecv(void* buf, size_t len)
    {
        return Recv(buf, len, MSG_DONTWAIT);
    }

    // 发送数据
    // 返回值：
    //   >0 : 实际发送字节数（可能小于 len，属于正常情况）
    //    0 : 当前不可写（EAGAIN / EINTR）
    //   -1 : 发送错误
    ssize_t Send(void* buf, size_t len, int flag = 0)
    {
        ssize_t ret = send(_sockfd, buf, len, flag);
        if(ret < 0)
        {
            if(errno == EAGAIN || errno == EINTR)
                return 0;

            lg(Error, "Send error");
            return -1;
        }
        return ret;
    }

    // 非阻塞发送
    ssize_t NonBlockSend(void* buf, size_t len)
    {
        return Send(buf, len, MSG_DONTWAIT);
    }

    // 关闭套接字
    bool Close()
    {
        int ret = close(_sockfd);
        if(ret < 0)
        {
            lg(Fatal, "Close error");
            return false;
        }
        return true;
    }

    // 创建服务器监听 socket
    // 顺序：
    //   socket -> nonblock -> reuse addr -> bind -> listen
    bool CreateServer(uint16_t port, const std::string& ip = "0.0.0.0")
    {
        if(!CreateSocket())
            return false;

        SetNonBlock();      // 非阻塞是 Reactor 的前提
        ReuseAddress();     // 支持服务器快速重启

        if(!Bind(port, ip))
            return false;
        if(!Listen())
            return false;

        return true;
    }

    // 创建客户端 socket
    bool CreateClient(uint16_t port, const std::string& ip)
    {
        if(!CreateSocket())
            return false;
        if(!Connect(port, ip))
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
        fcntl(_sockfd, F_SETFL, fl | O_NONBLOCK);
    }

    int GetFd() const
    {
        return _sockfd;
    }

    // RAII：对象析构时关闭 fd
    ~Socket()
    {
        if(_sockfd > 0)
            Close();
    }

private:
    int _sockfd;  // 套接字文件描述符
};


// ================================================================
//                            Channel模块
// ================================================================