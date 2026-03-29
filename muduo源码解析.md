做muduo源码阅读的原因来自于字节后端一面被面试官狠狠拷打，才发现自己对muduo的真实实现根本不了解，只是跟着老师写了一遍框架，但不知其所以然，秉持着面试挂也要挂的有价值，特地在第二天开始补读源码，这也让我学到了一点，学东西不能只学表面，要深挖原理，方可体会其中奥秘


# muduo源码阅读解析
## Buffer模块
Buffer模块主要充当发送和接收的缓冲区，所以缓冲区设计的好坏会直接决定了收发数据性能的高低
muduo的buffer设计非常巧妙，它尽可能的减少了数据的复制、拷贝和拼接次数，并且采用readv的方式减少了syscall，大大节约了读写的性能开销


> Buffer内部空间分割示意图
``` cpp
// +-------------------+------------------+------------------+
// | prependable bytes |  readable bytes  |  writable bytes  |
// |                   |     (CONTENT)    |                  |
// +-------------------+------------------+------------------+
// |                   |                  |                  |
// 0      <=      readerIndex   <=   writerIndex    <=     size
```


> Buffer的成员变量
``` cpp 
// 数组连续空间，避免链表降低缓存命中率
std::vector<char> buffer_;
// 两个指针将一个buffer分成三部分·
size_t readerIndex_;
size_t writerIndex_;
// 用于在缓冲区中查找\r\n的
static const char kCRLF[];
```
注意到，Buffer的读写指针在最开始的时候并不是指在整个数组的最开始，而是预留了一段空间，这一段预留的空间是为了在未来支持不移动body的数据的情况下在头部添加协议头部字段，减少了复制拷贝的性能消耗


> 缓冲区收缩机制
``` cpp
void shrink(size_t reserve)
{
    // FIXME: use vector::shrink_to_fit() in C++ 11 if possible.
    Buffer other;
    other.ensureWritableBytes(readableBytes() + reserve);
    other.append(toStringPiece());
    swap(other);
}
```
当经过网络传输峰值大包后，由于内存扩容机制会把内存扩大到很大，但后续空间使用率又不高，在这种情况下，muduo会新建一块恰好够存原数据的新缓冲区，再将两个缓冲区做置换，这样就可以把不使用的大空间回收，减少资源浪费


> 缓冲区扩容机制
``` cpp
void makeSpace(size_t len)
{
    if (writableBytes() + prependableBytes() < len + kCheapPrepend)
    {
        // FIXME: move readable data
        buffer_.resize(writerIndex_ + len);
    }
    else
    {
        // move readable data to the front, make space inside buffer
        assert(kCheapPrepend < readerIndex_);
        size_t readable = readableBytes();
        std::copy(begin() + readerIndex_,
                begin() + writerIndex_,
                begin() + kCheapPrepend);
        readerIndex_ = kCheapPrepend;
        writerIndex_ = readerIndex_ + readable;
        assert(readable == readableBytes());
    }
}
```
在扩容之前，会先计算原有空间足不足够，如果足够则移动数据到起始位置，将剩余空间用于新数据写入，如果空间不足则尝试扩容


> readv，一次从fd中读取数据到多个缓冲区的核心机制
``` cpp
ssize_t Buffer::readFd(int fd, int* savedErrno)
{
    // 第二套缓冲区(栈的临时空间)
    char extrabuf[65536];
    // 缓冲区列表
    struct iovec vec[2];
    // 计算buffer可接收的数据
    const size_t writable = writableBytes();
    vec[0].iov_base = begin()+writerIndex_;
    vec[0].iov_len = writable;
    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof extrabuf;
    // when there is enough space in this buffer, don't read into extrabuf.
    // when extrabuf is used, we read 128k-1 bytes at most.
    // 当缓冲区可写空间小于额外缓冲区就使用俩，如果大于则只使用buffer接收数据
    const int iovcnt = (writable < sizeof extrabuf) ? 2 : 1;
    const ssize_t n = sockets::readv(fd, vec, iovcnt);
    if (n < 0)
    {
        *savedErrno = errno;
    }
    else if (implicit_cast<size_t>(n) <= writable)
    {
        writerIndex_ += n;
    }
    else
    {
        // 将多个缓冲区的数据合并到一起
        writerIndex_ = buffer_.size();
        append(extrabuf, n - writable);
    }
    return n;
}
```
这段代码会根据buffer可写剩余空间灵活选择fd读入缓冲区的数量，有效减少了系统调用次数从而减少系统调用开销
iovec是缓冲区列表，readv的参数分别为fd, 缓冲区列表，缓冲区个数


## Socket模块
在muduo的实现中，socket模块的作用是只负责管理已有 fd + 提供常用操作封装，并没有像我自己实现的那样将创建设置等一系列操作都放在一起，muduo把底层调用封装在SocketsOps中，这里将两个模块合起来统称为Socket模块来讲解


> socket的主要成员和主要成员函数
``` cpp
// 对应socket的bind, listen, accept操作，而主体实现放在SocketsOpts中，这里只是上层调用
void bindAddress(const InetAddress& localaddr);
void listen();
int accept(InetAddress* peeraddr);

// 开启 TCP 保活机制（心跳探测）当一个链接长时间没有反应的时候会自动close
void setKeepAlive(bool on);
// 半关闭，相当于TCP四次挥手的一半，一端向另一端发送FIN，这段只读不写
void shutdownWrite();
// 选择Tcp的Nagle算法是否开启
void setTcpNoDelay(bool on);
// 开启地址复用
void setReuseAddr(bool on);
// 开启端口复用
void setReusePort(bool on);

// 唯一一个成员变量就是sockfd
const int sockfd_;
```
- setKeepAlive: 是TCP保活机制，如果上层是http协议开启长连接，就要调用它来保证长连接的可靠性
- shutdownWrite: 这个函数保证的Tcp的优雅关闭，该段向对方发送FIN标识该端不再发送数据并在对端接受缓冲区插入EOF，直到对端将想发送的数据发送完毕后才会发送FIN，最后四次挥手完成后再调用close可以保证不丢失数据
- setTcpNoDelay: TCP的Nagle算法是当一端频繁发送小包给对端的时候，nagle会尝试把小包积压，当合成一个大包的时候再一起发送给对端，但是nagle算法会因为延时ACK导致产生较高的网络延迟，一般为了保证较低延迟，会关闭这个算法，但是因此会导致小包发送频率升高(协议头占比较高)，占用大量网络带宽，所以在muduo的实现中，采用了writev
``` text
用户 write()
    ↓
Buffer（应用层缓冲）
    ↓
一次性 writev()
    ↓
TCP 发送
```
上述流程既可以保证延迟较低，也可以保证减少频繁发送小包导致的网络堵塞和带宽消耗


> SocketsOpts的主要成员函数
``` cpp
int createNonblockingOrDie(sa_family_t family);
int  connect(int sockfd, const struct sockaddr* addr);
void bindOrDie(int sockfd, const struct sockaddr* addr);
void listenOrDie(int sockfd);
int  accept(int sockfd, struct sockaddr_in6* addr);
ssize_t read(int sockfd, void *buf, size_t count);
ssize_t readv(int sockfd, const struct iovec *iov, int iovcnt);
ssize_t write(int sockfd, const void *buf, size_t count);
void close(int sockfd);
void shutdownWrite(int sockfd);
```
SocketsOpts主要是对底层的封装，这样可以做到:
1. 遵循单一职责原则
2. 将系统调用统一管理
3. 提高代码复用性
4. 支持不同来源的fd（如 accept）


## Channel模块
Channel在整个项目中的作用是管理fd关心的事件和设置回调函数，epoll负责检查事件是否就绪，而channel就负责事件就绪后的回调


> 成员变量
``` cpp
    EventLoop* loop_;     // channel所绑定的loop，channel只知道关心的事件，真正调用poller的还是loop
                          // Channel 自己 ≠ 能操作 epoll
                          // 真正操作 epoll 的是 Poller
                          // 而 Poller 又归属于 EventLoop

    const int fd_;        // 管理的文件fd
    int events_;          // channel管理的fd关心的事件列表
    int revents_;         // fd就绪事件列表

    int index_;           // 用于标识这个fd在epoll的状态，EpollPoller中可见下面定义
                          // const int kNew = -1;   (从未加入过epoll)
                          // const int kAdded = 1;  (在epoll中)
                          // const int kDeleted = 2;(加入过但是删除了)
                          // 这个用于epoll_ctl的时候选择 MOD/ADD/DEL

    std::weak_ptr<void> tie_; 
    // 绑定上层对象生命周期，防止回调的时候上层连接已经被释放
    // TcpConnection 被 close()
    // Channel 还在 epoll 里
    // epoll 触发 -> 回调访问野指针

    bool tied_;           // 标记是否绑定生命周期
    bool eventHandling_;  // 标记是否在处理事件
    bool addedToLoop_;    // 是否注册到Poller中
    bool logHup_;         // 是否打印日志

    ReadEventCallback readCallback_; // 各种回调函数
    EventCallback writeCallback_;
    EventCallback closeCallback_;
    EventCallback errorCallback_;
```
muduo为了回调执行的安全性，在设计channel的时候加了很多状态表示，如tied，eventhandling... 他们可以保证在执行回调的时候上层连接未被释放，防止产生UAF(Use After Free)的情况发生，确保了整个流程的安全


> 核心成员函数
``` cpp
void handleEvent(Timestamp receiveTime)
{
    std::shared_ptr<void> guard;
    if (tied_)
    {
        guard = tie_.lock();
        if (guard)
            handleEventWithGuard(receiveTime);
    }
    else
        handleEventWithGuard(receiveTime);
}

void Channel::handleEventWithGuard(Timestamp receiveTime)
{
    eventHandling_ = true;
    LOG_TRACE << reventsToString();
    // 在处理挂断事件的时候，要同时判断是否有可读事件，防止还没读就挂断，导致数据丢失
    if ((revents_ & POLLHUP) && !(revents_ & POLLIN))
    {
        if (logHup_)
            LOG_WARN << "fd = " << fd_ << " Channel::handle_event() POLLHUP";
        if (closeCallback_) 
            closeCallback_();
    }

    if (revents_ & POLLNVAL)
        LOG_WARN << "fd = " << fd_ << " Channel::handle_event() POLLNVAL";
    // 如果产生错误或者没有val的时候，会调用错误回调
    if (revents_ & (POLLERR | POLLNVAL))
        if (errorCallback_) errorCallback_();

    if (revents_ & (POLLIN | POLLPRI | POLLRDHUP))
        if (readCallback_) readCallback_(receiveTime);

    if (revents_ & POLLOUT)
        if (writeCallback_) writeCallback_();

    eventHandling_ = false;
}
void enableReading() { events_ |= kReadEvent; update(); }
void disableReading() { events_ &= ~kReadEvent; update(); }
void enableWriting() { events_ |= kWriteEvent; update(); }
void disableWriting() { events_ &= ~kWriteEvent; update(); }
void disableAll() { events_ = kNoneEvent; update(); }
bool isWriting() const { return events_ & kWriteEvent; }
bool isReading() const { return events_ & kReadEvent; }
```
在channel的设计中，最主要的就是handlevent，它涉及事件就绪后对应回调的分发执行，它采用了Tie()来保证执行回调的安全，eventHandling_标识是否在处理事件，防止在处理事件的时候上层的连接被释放


> 一次完整的IO事件
``` text
1. epoll_wait 返回
2. Poller 填充 channel->revents_
3. EventLoop 调用：
      channel->handleEvent()
4. handleEvent():
      -> tie 保活
      -> handleEventWithGuard()
5. 分发回调：
      readCallback()
      writeCallback()
```


## Poller模块
在我的demo实现中，poller是直接封装了epoll，而muduo是一个通用网络框架，为了平台和系统的兼容性，它采用了策略模式和工厂模式，策略模式主要用来把一组可替换的算法封装起来，通过统一接口，在运行时动态选择具体实现，它的整个作用是使用对象，工厂模式则是给上层一个创建对象的统一接口，使用者不需要关心实例化的是什么，只需要调用工厂就可以获得一个Poller对象，在muduo中，给上层了epoll和poll两个选择来作为poller的底层策略(只有linux有epoll，在连接数较少的情况下，poll的性能不比epoll差很多)，整个策略模式是通过继承和多态实现的


> Poller(基类)
``` cpp

```





# 对比Muduo源码个人实现设计不足的点
## Buffer模块
- muduo源码在buffer空间头部预留了协议头的空间，允许在后续直接回填协议头，不必重新分配一整块新内存，减少了协议头和包体重新拼接复制移动产生的消耗，后续可以通过prepend()将协议头添加到头部空间

- muduo有内存收缩机制，当buffer经过传输峰值后，如果长时间不再使用大块内存，muduo会有shrink()创建一个新的buffer并将旧buffer的元素copy到新buffer中，同时将内存收缩到正常大小，这样就不会存在长时间buffer持有大块空间内存，导致缓存命中率低，同时可以节约内存

- muduo的buffer在从内核缓冲区中(fd)中拿数据做了优化，使用了readv，使原本需要循环读取的内容通过一次系统调用就可以将数据读到多个缓冲区，参数为fd, iovec(缓冲区列表), n(缓冲区个数)，在muduo中，它在readfd中额外开辟了一块栈区空间(extrabuffer)大小为65535字节，当buffer剩余写空间不足的时候，会将剩余数据写到临时缓冲区中，最后将两个缓冲区合并，在此过程中避免了缓冲区重复开辟空间和数据的复制，节约了系统开销

- muduo优化了字符串传参，当内存收缩需要将数据拷贝到新缓冲区的时候，muduo采用了StringPiece(字符串视图)的方式，它保存了指向字符串的指针和字符串长度，在传参的时候不需要重新构造一次临时string对象，节约了一次潜在的额外拷贝

- muduo预留了网络字节序接口，简化了网络协议拼包和拆包过程，把他们变成了统一、可内联、少临时对象、少重复扫描的流程。如果没有预留接口，业务层需要先转换字节序，然后存到临时空间，再分别写入协议头和body，这样做可以减少临时对象和重复拷贝，减少系统开销


## channel模块
- muduo的channel绑定了上层TcpConnection的生命周期，防止上层对象被析构后下层还会调用回调函数导致悬空
> epoll_wait 返回事件
> → Channel::handleEvent()
> → 回调 TcpConnection::handleRead()
> 但此时：
> TcpConnection 已经被 close 并释放 ❌

- muduo将读回调单独取出并添加时间戳的参数，避免了后续计算超时时间需要再次获取，在高并发场景下仍会产生消耗，同时muduo没有任意事件回调，将刷新过期时间的任务合并到时间戳一直带下去判断即可，再次减少了回调函数所产生的消耗

- muduo在删除channel的时候也做了严格的检查，用ishandling标记是否在处理事件，防止下面这个情况
> handleEvent()
> → readCallback()
>   → TcpConnection::handleClose()
>     → Channel::remove()
同时也用addedToLoop_标记channel是否添加到loop，防止重复添加和remove未天际的channel

## EventLoop模块


## Poller模块
- muduo源码中的poller模块并没有直接封装epoll，而是将Poller中大部分函数变成虚函数，让上层来选择使用epoll或者poll，主要是因为epoll只能在linux平台下使用，为了系统兼容性可能别的平台要用poll，其次因为在连接数较少的情况下，poll 的性能并不差，而且实现更简单、行为更稳定

> 上述实际上是“策略模式 + 工厂模式”的组合，用工厂决定策略实例，用策略实现行为切换
> 策略模式: 把一组可替换的算法封装起来，通过统一接口，在运行时动态选择具体实现，它的整个作用是使用对象
> 抽象策略
> class Poller {
> public:
>    virtual Timestamp poll(...) = 0;
>    virtual void updateChannel(...) = 0;
> };
> 具体策略
> class EPollPoller : public Poller { ... };
> class PollPoller : public Poller { ... };
> 
> 而在创建对象时选择不同策略，使用的就是工厂模式
> 工厂模式: 为创建对象提供一个统一接口，隐藏具体类的实例化细节
> 在下述函数中采用的就是工厂模式来创建Poller对象
> Poller* Poller::newDefaultPoller(EventLoop* loop)
> {
>   if (::getenv("MUDUO_USE_POLL"))
>   {
>     return new PollPoller(loop);
>   }
>   else
>   {
>     return new EPollPoller(loop);
>   }
> }
>
> 这样设计有几个好处:
> 业务代码不知道是使用Epoll还是Poll，它只会得到一个抽象的Poller对象，实现了解耦
> 有较好的扩展性，在未来如果想用其他方式可以直接通过修改工厂
> 统一了创建接口，所有对象都统一管理

- 


