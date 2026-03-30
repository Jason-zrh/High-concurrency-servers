做muduo源码阅读的原因来自于字节后端一面被面试官狠狠拷打，才发现自己对muduo的真实实现根本不了解，只是跟着老师写了一遍框架，但不知其所以然，秉持着面试挂也要挂的有价值，特地在第二天开始补读源码，学东西不能只学表面，要深挖原理，方可体会其中奥秘


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
- setTcpNoDelay: TCP的Nagle算法是当一端频繁发送小包给对端的时候，nagle会尝试把小包积压，当合成一个大包的时候再一起发送给对端，但是nagle算法会因为延时ACK导致产生较高的网络延迟，一般为了保证较低延迟，会关闭这个算法，但是因此会导致小包发送频率升高(协议头占比较高)，占用大量网络带宽，所以在muduo的实现中，在Connetion中采用了不同的发送策略来兼顾性能和延迟降低
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
class Poller : noncopyable
{
public:
    typedef std::vector<Channel*> ChannelList;
    // 一个poller与一个eventloop绑定
    Poller(EventLoop* loop);
    virtual ~Poller();

    // 纯虚函数必须由子类完成重写
    // 开始监控关心事件
    virtual Timestamp poll(int timeoutMs, ChannelList* activeChannels) = 0;
    // 更新事件
    virtual void updateChannel(Channel* channel) = 0;
    // 移除关心事件
    virtual void removeChannel(Channel* channel) = 0;
    // 判断channel是否注册到关心列表中
    virtual bool hasChannel(Channel* channel) const;
    // 工厂入口(根据环境来选择使用poll还是epoll)
    Poller* Poller::newDefaultPoller(EventLoop* loop)
    {
        if (::getenv("MUDUO_USE_POLL"))
            return new PollPoller(loop);
        else
            return new EPollPoller(loop);
    }
    void assertInLoopThread() const
    {
        ownerLoop_->assertInLoopThread();
    }
protected:
    typedef std::map<int, Channel*> ChannelMap;
    ChannelMap channels_;
private:
    EventLoop* ownerLoop_;
};
```
poller是一个基类，用于被epoll或poll继承，用于策略模式的实现，它将关键函数设置为纯虚函数，要求必须派生类实现


> EpollPoller关键函数和成员(epoll策略)
``` cpp
// 开始监控
Timestamp poll(int timeoutMs, ChannelList* activeChannels) override;
// 更新channel关心的事件
void updateChannel(Channel* channel) override;
// 移除channel
void removeChannel(Channel* channel) override;
// 将内核的数据提到用户态
void fillActiveChannels(int numEvents, ChannelList* activeChannels) const;

static const int kInitEventListSize = 16;
// update主要与内核交互
void update(int operation, Channel* channel);
typedef std::vector<struct epoll_event> EventList;
int epollfd_;
EventList events_;
```
EpollPoller继承Poller，必须重写poller中的纯虚函数，同时它还兼顾了更新poller关心的事件和回填活跃channel的功能，最巧妙的是它可以通过O(1)的时间复杂度找到就绪的channel，而不需要再通过map回表查询fd对应channel，epoll_event中存的是就绪事件和用户设置的数据，在后续epoll_ctl的时候muduo把channel的ptr设置进来了，在后面可以直接O(1)找回


> 关键函数的实现
``` cpp
// 首先关注poll的返回值是一个时间戳，用于给上层eventloop返回事件就绪事件，在事件就绪后统一返回，既保证了事件时间的统一性又减少了系统调用的开销
Timestamp EPollPoller::poll(int timeoutMs, ChannelList* activeChannels)
{
    LOG_TRACE << "fd total count " << channels_.size();
    int numEvents = ::epoll_wait(epollfd_,
                        &*events_.begin(),
                        static_cast<int>(events_.size()),
                        timeoutMs);
    int savedErrno = errno;
    Timestamp now(Timestamp::now());
    if (numEvents > 0)
    {
        LOG_TRACE << numEvents << " events happened";
        fillActiveChannels(numEvents, activeChannels);
        // 如果返回事件填满了整个就绪事件列表，就给列表扩容，防止产生事件截断
        if (implicit_cast<size_t>(numEvents) == events_.size())
            events_.resize(events_.size()*2);
    }
    else if (numEvents == 0)
        LOG_TRACE << "nothing happened";
    else
    {
        if (savedErrno != EINTR)
        {
            errno = savedErrno;
            LOG_SYSERR << "EPollPoller::poll()";
        }
    }
    return now;
}

// 这个函数主要是用于给上层eventloop返回就绪的channel列表，通过ch->handlevent的方式来执行对应回调
void EPollPoller::fillActiveChannels(int numEvents, ChannelList* activeChannels) const
{
    assert(implicit_cast<size_t>(numEvents) <= events_.size());
    for (int i = 0; i < numEvents; ++i)
    {
        // 这里采用O(1)的事件复杂度找到就绪的channel，主要是因为在update的时候将ptr设为channel了
        Channel* channel = static_cast<Channel*>(events_[i].data.ptr);
        // 给这个channel设置就绪事件
        channel->set_revents(events_[i].events);
        // 将这个channel添加到就绪channel列表中
        activeChannels->push_back(channel);
    }
}

void EPollPoller::updateChannel(Channel* channel)
{
    Poller::assertInLoopThread();
    // epoll设置了三个状态，分别是add, new, deleted
    // 这个状态通过index保存在channel中，在update的时候通过检测状态来判断epoll_ctl的行为
    const int index = channel->index();
    LOG_TRACE << "fd = " << channel->fd()<< " events = " << channel->events() << " index = " << index;
    // 新添加或者被添加过被删除
    if (index == kNew || index == kDeleted)
    {
        // a new one, add with EPOLL_CTL_ADD
        int fd = channel->fd();
        // 先判断channel在map中存不存在
        if (index == kNew)
        {
            assert(channels_.find(fd) == channels_.end());
            channels_[fd] = channel;
        }
        else // index == kDeleted
        {
            assert(channels_.find(fd) != channels_.end());
            assert(channels_[fd] == channel);
        }
        // 然后将index状态修改，update它channel的关心事件
        channel->set_index(kAdded);
        update(EPOLL_CTL_ADD, channel);
    }
    else
    {
        // update existing one with EPOLL_CTL_MOD/DEL
        int fd = channel->fd();
        (void)fd;
        assert(channels_.find(fd) != channels_.end());
        assert(channels_[fd] == channel);
        assert(index == kAdded);
        // 如果这个channel没有关心的事件，就直接给他从内核关心列表中删除，epoll不允许关注一个没有关注事件的fd
        if (channel->isNoneEvent())
        {
            // 这里语义跟remove不同，remove是在map和epoll中删除，这里del是暂时不关心事件但是fd还在map中，后续可以随时关心
            update(EPOLL_CTL_DEL, channel);
            channel->set_index(kDeleted);
        }
        else    
        // 修改fd的关心事件
            update(EPOLL_CTL_MOD, channel);
    }
}

void EPollPoller::removeChannel(Channel* channel)
{
    Poller::assertInLoopThread();
    int fd = channel->fd();
    LOG_TRACE << "fd = " << fd;
    assert(channels_.find(fd) != channels_.end());
    assert(channels_[fd] == channel);
    assert(channel->isNoneEvent());
    int index = channel->index();
    assert(index == kAdded || index == kDeleted);
    // 在这里彻底取消对channel的关心，既从map中移除又从epoll中移除
    size_t n = channels_.erase(fd);
    (void)n;
    assert(n == 1);
    //
    if (index == kAdded)
    {
        update(EPOLL_CTL_DEL, channel);
    }
    channel->set_index(kNew);
}

void EPollPoller::update(int operation, Channel* channel)
{
    struct epoll_event event;
    memZero(&event, sizeof event);
    // 设置channel的关心事件
    event.events = channel->events();
    // 在这里events的data中存的是channel的指针，方便后续一次O(1)查找
    event.data.ptr = channel;
    int fd = channel->fd();
    LOG_TRACE << "epoll_ctl op = " << operationToString(operation)
    << " fd = " << fd << " event = { " << channel->eventsToString() << " }";
    if (::epoll_ctl(epollfd_, operation, fd, &event) < 0)
    {
        if (operation == EPOLL_CTL_DEL)
        {
            LOG_SYSERR << "epoll_ctl op =" << operationToString(operation) << " fd =" << fd;
        }
        else
        {
            LOG_SYSFATAL << "epoll_ctl op =" << operationToString(operation) << " fd =" << fd;
        }
    }
}
```
在这个模块中值得看的设计点:
1. epoll_ctl的过程中，将channel设置到epoll_event的data中，在后续事件就绪后可以直接获取到channel填写到就绪channel列表
2. 用channel的index和状态机(new, add, deleted)，deleted代表曾经添加到关心列表中，如果暂时不关心事件可以先取消，后续可以再通过map找到channel添加关心事件，与remove的语义不同(完全删除channel)


## EventLoop模块
EventLoop是一个核心模块，它管理了一个线程被分配的所有channel，并且里面整合了Poller模块进行对channel事件的关心，它的作用可以总结为三个: 1.对事件进行监控(Poller) 2.就绪事件处理(Poller的fillActiveChannel) 3.执行任务(执行Channel的回调)


> 成员变量
``` cpp
// 保存对应这个eventloop的线程id，one thread one loop，reactor与线程绑定
const pid_t threadId_;


// 是否退出loop
std::atomic<bool> quit_; 
// 判断是否在loop中
bool looping_; /* atomic */
// 是否在处理回调
bool eventHandling_; /* atomic */
// 是否在执行任务队列
bool callingPendingFunctors_; /* atomic */
int64_t iteration_;


// epoll返回时间，用来校准定时器 / 事件时间戳
Timestamp pollReturnTime_;
// 时间轮，用来执行定时任务，销毁超时链接的
std::unique_ptr<TimerQueue> timerQueue_;


// 用来跨线程唤醒线程，本质是linux的eventfd
int wakeupFd_;
std::unique_ptr<Channel> wakeupChannel_;
// 管理上下文?
boost::any context_;


// Poller模块整合
std::unique_ptr<Poller> poller_;
typedef std::vector<Channel*> ChannelList;
// epoll_wait返回的就绪fd列表
ChannelList activeChannels_;
// 正在处理的channel
Channel* currentActiveChannel_;


// 自己封装的锁(没有使用unique_lock为了轻量化)
mutable MutexLock mutex_;
// 任务队列(被锁保护，因为可能同时有多个线程向任务队列投放任务)
std::vector<Functor> pendingFunctors_ GUARDED_BY(mutex_);
```
eventloop的成员很多，其中大部分是为了防止某些意外发生而设置的标志位，而核心中的核心:
- threadId_             用于记录eventloop所属线程，用于跨线程投放任务
- wakeupFd_             就是linux的eventfd，用来跨线程唤醒
- poller_               Poller模块，用于事件的关心，Reactor的核心
- activeChannels_       就绪事件channel列表，用于channel对就绪的事件调用它的回调
- mutex_                muduo自行设计的锁，用于处理多线程向同一任务队列投放任务的场景
- pendingFunctors_      任务队列，当跨线程投放任务的时候会把任务投递到对应的eventloop的任务队列中


> 核心成员函数
``` cpp
// 主要循环函数，每次loop都获取就绪的事件并执行对应回调
void EventLoop::loop()
{
    assert(!looping_);
    assertInLoopThread();
    looping_ = true;
    quit_ = false; // FIXME: what if someone calls quit() before loop() ?
    LOG_TRACE << "EventLoop " << this << " start looping";
    while (!quit_)
    {
        // 获取就绪channel
        activeChannels_.clear();
        pollReturnTime_ = poller_->poll(kPollTimeMs, &activeChannels_);
        // TODO sort channel by priority
        eventHandling_ = true;
        for (Channel *channel : activeChannels_)
        {
            // 执行对应回调
            currentActiveChannel_ = channel;
            currentActiveChannel_->handleEvent(pollReturnTime_);
        }
        currentActiveChannel_ = NULL;
        eventHandling_ = false;
        // 执行任务队列中的任务
        doPendingFunctors();
    }
    LOG_TRACE << "EventLoop " << this << " stop looping";
    looping_ = false;
}
// 如果是跨线程quit，先把对应线程wakeup一下
void EventLoop::quit()
{
    quit_ = true;
    if (!isInLoopThread())
    {
        wakeup();
    }
}

void EventLoop::runInLoop(Functor cb)
{
    // 如果任务属于该线程直接执行
    if (isInLoopThread())
        cb();
    else
    // 投放到对应线程任务队列中
        queueInLoop(std::move(cb));
}

void EventLoop::queueInLoop(Functor cb)
{
    {
        MutexLockGuard lock(mutex_);
        // 这里使用了move右值引用，减少了一次拷贝，提升性能
        pendingFunctors_.push_back(std::move(cb));
    }

    if (!isInLoopThread() || callingPendingFunctors_)
        wakeup();
}

void EventLoop::updateChannel(Channel *channel)
{
    assert(channel->ownerLoop() == this);
    assertInLoopThread();
    poller_->updateChannel(channel);
}

void EventLoop::removeChannel(Channel *channel)
{
    assert(channel->ownerLoop() == this);
    assertInLoopThread();
    if (eventHandling_)
    {
        assert(currentActiveChannel_ == channel ||
                std::find(activeChannels_.begin(), activeChannels_.end(), channel) == activeChannels_.end());
    }
    poller_->removeChannel(channel);
}

bool EventLoop::hasChannel(Channel *channel)
{
    assert(channel->ownerLoop() == this);
    assertInLoopThread();
    return poller_->hasChannel(channel);
}

void EventLoop::abortNotInLoopThread()
{
  LOG_FATAL << "EventLoop::abortNotInLoopThread - EventLoop " << this
            << " was created in threadId_ = " << threadId_
            << ", current thread id = " << CurrentThread::tid();
}

// 向wakeupfd中写一个值
void EventLoop::wakeup()
{
    uint64_t one = 1;
    ssize_t n = sockets::write(wakeupFd_, &one, sizeof one);
    if (n != sizeof one)
    {
    LOG_ERROR << "EventLoop::wakeup() writes " << n << " bytes instead of 8";
    }
}

// 处理wakeupfd
void EventLoop::handleRead()
{
    uint64_t one = 1;
    ssize_t n = sockets::read(wakeupFd_, &one, sizeof one);
    if (n != sizeof one)
    {
    LOG_ERROR << "EventLoop::handleRead() reads " << n << " bytes instead of 8";
    }
}

void EventLoop::doPendingFunctors()
{
    std::vector<Functor> functors;
    callingPendingFunctors_ = true;

    {
        MutexLockGuard lock(mutex_);
        functors.swap(pendingFunctors_);
    }

    for (const Functor &functor : functors)
        functor();

    callingPendingFunctors_ = false;
}
```
EventLoop中，需要主要关注的函数是loop，runInLoop，queueInLoop，doPendingFunctors
- loop函数会先执行就绪事件的回调，执行结束后再调用doPendingFunctors执行别的线程投放到任务队列中的任务
- runInLoop函数则是为了保证对应任务在对应loop中执行，如果一个任务属于该线程，则直接执行，否则调用queueInLoop将任务投到对应线程中，在queueInLoop传参的时候，使用move移动语义(?)
- queueInLoop函数设计考虑也比较完全，在投递任务前应先加锁，保证线程安全，并在两种情况下会给wakeup对应线程
1. 任务线程与当前线程不符，唤醒对应线程，执行任务
2. 用户单独调用的queueInLoop，如果这个线程已经处于callingPendingFunctors_，说明该线程的对应任务队列已经清空，如果只向该队列中添加任务而不唤醒，可能会导致任务被延迟执行或者阻塞，实际上callingPendingFunctors_ 解决的是“同线程任务重入问题”
- doPendingFunctors采用的是用一个空的任务队列交换已有的任务队列，这样一来避免了在执行任务的过程中一直持有锁，导致其他线程无法向任务队列中投放任务，减少了并发量


> runInLoop中的move语义 + 右值引用
``` cpp
void EventLoop::runInLoop(Functor cb)
{
    // 如果任务属于该线程直接执行
    if (isInLoopThread())
        cb();
    else
    // 投放到对应线程任务队列中
        queueInLoop(std::move(cb)); // move语义
}
```
在runInLoop中，如果要跨线程投放任务要将任务添加到对应线程中，如果不使用move且该任务函数实现较大且在高并发的情况下被频繁调用，那么这个函数就会被疯狂拷贝，产生大量性能消耗，所以这里使用move直接将资源转移到vector中而不是再构造一个

整个过程中，move的作用是将资源标记为可作为右值引用``static_cast<T&&> value``，真正实现资源转移的是该资源的移动构造函数，它的参数就是右值引用的资源，在muduo的使用中，cb是一个function对象，在C++的std实现中，function自带了移动构造函数，这样一来就可以避免函数的深拷贝而是直接转移资源，在转移后原持有该资源的会变为空，资源放到新的所属对象中，下面是fuction移动构造的源码
``` cpp
function(function&& __x) noexcept
    : _Function_base(), _M_invoker(__x._M_invoker)
{
    if (static_cast<bool>(__x))
    {
        _M_functor = __x._M_functor;
        _M_manager = __x._M_manager;
        __x._M_manager = nullptr;
        __x._M_invoker = nullptr;
    }
}
```
相当于通过移动构造将原来的函数的数据和管理方法全部转移到新的function对象(插入到任务队列中新的对象)中，可以节约复制开销


## EventLoopThread模块
在一个独立线程中创建并运行一个 EventLoop，并把这个loop暴露给外部使用，由于loop需要保存所属线程id，所以loop需要在线程中创建


> 核心实现
``` cpp
class EventLoopThread : noncopyable
{
 public:
    typedef std::function<void(EventLoop*)> ThreadInitCallback;

    EventLoopThread(const ThreadInitCallback& cb = ThreadInitCallback(),
                const string& name = string());
    ~EventLoopThread();

    EventLoop* EventLoopThread::startLoop()
    {
        // 防止重复启动线程
        assert(!thread_.started());
        thread_.start();

        EventLoop* loop = NULL;
        {
            MutexLockGuard lock(mutex_);
            // 等待线程入口函数的loop创建，防止返回空loop
            while (loop_ == NULL)
            {
                cond_.wait();
            }
            loop = loop_;
        }
        return loop;
    }

private:
    // 线程入口函数
    void EventLoopThread::threadFunc()
    {
        EventLoop loop;
        if (callback_)
            callback_(&loop);
        {
            MutexLockGuard lock(mutex_);
            loop_ = &loop;
            cond_.notify();
        }
        // 这里阻塞运行，循环启动
        loop.loop();
        // 走到这里说明循环停止了，将loop置空
        MutexLockGuard lock(mutex_);
        loop_ = NULL;
    }


    EventLoop* loop_ GUARDED_BY(mutex_);
    bool exiting_;
    Thread thread_;
    MutexLock mutex_;
    Condition cond_ GUARDED_BY(mutex_);
    ThreadInitCallback callback_; 
};
```


## EventLoopThreadPool模块
管理多个 IO 线程（EventLoop），并提供负载均衡策略分发连接


> 核心实现
``` cpp
class EventLoopThreadPool : noncopyable
{
public:
    typedef std::function<void(EventLoop*)> ThreadInitCallback;

    EventLoopThreadPool(EventLoop* baseLoop, const string& nameArg);
    ~EventLoopThreadPool();
    void setThreadNum(int numThreads) { numThreads_ = numThreads; }
    void start(const ThreadInitCallback& cb = ThreadInitCallback());

    EventLoop* getNextLoop();
    /// with the same hash code, it will always return the same EventLoop
    EventLoop* getLoopForHash(size_t hashCode);

    std::vector<EventLoop*> getAllLoops();

    bool started() const
    { return started_; }

    const string& name() const
    { return name_; }

private:

    EventLoop* baseLoop_;
    string name_;
    bool started_;
    int numThreads_;
    int next_;
    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    std::vector<EventLoop*> loops_;
};


void EventLoopThreadPool::start(const ThreadInitCallback& cb)
{
    assert(!started_);
    baseLoop_->assertInLoopThread();
    started_ = true;
    for (int i = 0; i < numThreads_; ++i)
    {
        char buf[name_.size() + 32];
        snprintf(buf, sizeof buf, "%s%d", name_.c_str(), i);
        // 根据数量创建线程池
        EventLoopThread* t = new EventLoopThread(cb, buf);
        threads_.push_back(std::unique_ptr<EventLoopThread>(t));
        // 保存对应loop
        loops_.push_back(t->startLoop());
    }
    // 如果只有一个核心线程，任务则都交给核心线程处理
    if (numThreads_ == 0 && cb)
    {
        cb(baseLoop_);
    }
}

EventLoop* EventLoopThreadPool::getNextLoop()
{
    baseLoop_->assertInLoopThread();
    assert(started_);
    EventLoop* loop = baseLoop_;

    if (!loops_.empty())
    {
        loop = loops_[next_];
        ++next_;
        if (implicit_cast<size_t>(next_) >= loops_.size())
        {
            next_ = 0;
        }
    }
    return loop;
}

EventLoop* EventLoopThreadPool::getLoopForHash(size_t hashCode)
{
    baseLoop_->assertInLoopThread();
    EventLoop* loop = baseLoop_;

    if (!loops_.empty())
    {
        loop = loops_[hashCode % loops_.size()];
    }
    return loop;
}

std::vector<EventLoop*> EventLoopThreadPool::getAllLoops()
{
    baseLoop_->assertInLoopThread();
    assert(started_);
    if (loops_.empty())
    {
        return std::vector<EventLoop*>(1, baseLoop_);
    }
    else
    {
        return loops_;
    }
}
```
EventLoopThreadPool模块中，muduo为loop分配提供两种分配方式，一种是简单分配的RR轮转均匀分布，另一种是稳定的哈希映射
RR轮转适用于通用IO场景，有较强的负载均衡能力，但是稳定性较差(同一个用户两次登陆可能分配到不同线程上，需要跨线程同步)
哈希映射适用于有“会话/用户”的服务，有较好的稳定性，但是可能会导致负载偏移，可能会出现某一个loop很繁忙同时另一个loop很空的情况


## TcpConnection模块(与demo中Connetion模块类似)
TcpConnection = 一个 TCP 连接在用户态的抽象（Reactor + IO缓冲 + 生命周期管理），每个connection应该在accept到连接的时候用socketfd创建，上层用户不应该可以直接创建connetion对象


> 核心成员变量
``` cpp
EventLoop* loop_;      // 该连接所属loop
const string name_;    // 连接的唯一标识，一般127.0.0.1:8888#1

StateE state_;         // 连接的生命周期状态机
                       // enum StateE { 
                       //     kDisconnected, 连接完全关闭
                       //     kConnecting,   正在建立连接
                       //     kConnected,    建立连接完成
                       //     kDisconnecting 正在关闭连接
                       // };

bool reading_;
// we don't expose those classes to client.
std::unique_ptr<Socket> socket_;    // socket模块，管理该连接fd
std::unique_ptr<Channel> channel_;  // channel模块，管理fd，回调绑定

const InetAddress localAddr_;       // 本端和对端ip地址
const InetAddress peerAddr_;

ConnectionCallback connectionCallback_;         // 连接事件回调，连接建立 / 断开时触发
MessageCallback messageCallback_;               // 消息回调，收到数据时调用
WriteCompleteCallback writeCompleteCallback_;   // 写完成回调，数据发送完毕
HighWaterMarkCallback highWaterMarkCallback_;   // 输出缓冲区过大
CloseCallback closeCallback_;                   // 通知server这个连接可以销毁了

size_t highWaterMark_;    // 输出缓冲区高水位回调
Buffer inputBuffer_;      // 输入输出缓冲区  
Buffer outputBuffer_;     // 输出缓冲区
boost::any context_;      // 协议上下文  
```


> 核心成员函数
``` cpp
// 构造函数
TcpConnection::TcpConnection(EventLoop* loop, const string& nameArg, int sockfd,
                const InetAddress& localAddr, const InetAddress& peerAddr)
    :loop_(CHECK_NOTNULL(loop))
    ,name_(nameArg)
    ,state_(kConnecting)  // 初始状态为开始建立连接
    ,reading_(true)
    ,socket_(new Socket(sockfd))
    ,channel_(new Channel(loop, sockfd))
    ,localAddr_(localAddr)
    ,peerAddr_(peerAddr)
    ,highWaterMark_(64*1024*1024)
{
    // 给channel绑定事件回调函数，这个回调函数根据上层业务来决定
    channel_->setReadCallback(std::bind(&TcpConnection::handleRead, this, _1));
    channel_->setWriteCallback(std::bind(&TcpConnection::handleWrite, this));
    channel_->setCloseCallback(std::bind(&TcpConnection::handleClose, this));
    channel_->setErrorCallback(std::bind(&TcpConnection::handleError, this));
    socket_->setKeepAlive(true);
}

void TcpConnection::connectEstablished()
{
    loop_->assertInLoopThread();
    // 状态检查，只有状态正确才继续往下走
    assert(state_ == kConnecting);
    setState(kConnected);
    // channel生命周期绑定，如果上层连接对象被析构了则不执行其回调
    channel_->tie(shared_from_this());
    // 设置读事件关心
    channel_->enableReading();
    connectionCallback_(shared_from_this());
}

void TcpConnection::connectDestroyed()
{
    loop_->assertInLoopThread();
    // 状态检查
    if (state_ == kConnected)
    {
        // 设置完全关闭状态
        setState(kDisconnected);
        // 取消channel所有关心事件
        channel_->disableAll();
        // 执行连接销毁回调，告诉server可以把连接销毁
        connectionCallback_(shared_from_this());
    }
    // 将channel从红黑树中移除
    channel_->remove();
}

// 处理读事件EPOLLIN
void TcpConnection::handleRead(Timestamp receiveTime)
{
    loop_->assertInLoopThread();
    int savedErrno = 0;
    // 先将数据从内核中读到接收缓冲区，底层采用readv一次系统调用
    ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);
    if (n > 0)
    {
        // 执行读回调处理接受到的数据
        messageCallback_(shared_from_this(), &inputBuffer_, receiveTime);
    }
    else if (n == 0)
    {
        // 没读到数据，进入关闭流程
        handleClose();
    }
    else
    {
        errno = savedErrno;
        LOG_SYSERR << "TcpConnection::handleRead";
        // 错误处理
        handleError();
    }
}

void TcpConnection::handleWrite()
{
    loop_->assertInLoopThread();
    // 先判断channel是否关心写事件了
    if (channel_->isWriting())
    {
        // 将数据从输出缓冲区写到内核
        ssize_t n = sockets::write(channel_->fd(),
                                    outputBuffer_.peek(),
                                    outputBuffer_.readableBytes());
        if (n > 0)
        {
            // 消费对应数量事件
            outputBuffer_.retrieve(n);
            // 如果没有可写数据了
            if (outputBuffer_.readableBytes() == 0)
            {
                // 取消对写事件的关心
                channel_->disableWriting();
                if (writeCompleteCallback_)
                {
                    loop_->queueInLoop(std::bind(writeCompleteCallback_, shared_from_this()));
                }
                // 如果连接处于半关闭状态，需要等数据发完再关闭，走到这里代表数据发完了，可以关闭
                if (state_ == kDisconnecting)
                {
                    shutdownInLoop();
                }
            }
        }
        else
        {
            LOG_SYSERR << "TcpConnection::handleWrite";
        }
    }
    else
    {
        LOG_TRACE << "Connection fd = " << channel_->fd()
                    << " is down, no more writing";
    }
}

void TcpConnection::handleClose()
{
    loop_->assertInLoopThread();
    LOG_TRACE << "fd = " << channel_->fd() << " state = " << stateToString();
    assert(state_ == kConnected || state_ == kDisconnecting);
    // we don't close fd, leave it to dtor, so we can find leaks easily.
    setState(kDisconnected);
    channel_->disableAll();
    // 保存shared_ptr，防止在函数调用中对象被析构
    TcpConnectionPtr guardThis(shared_from_this());
    connectionCallback_(guardThis);
    closeCallback_(guardThis);
}

void TcpConnection::handleError()
{
    int err = sockets::getSocketError(channel_->fd());
    LOG_ERROR << "TcpConnection::handleError [" << name_
            << "] - SO_ERROR = " << err << " " << strerror_tl(err);
}



void TcpConnection::send(const void* data, int len)
{
    send(StringPiece(static_cast<const char*>(data), len));
}

void TcpConnection::send(const StringPiece& message)
{
    if (state_ == kConnected)
    {
        if (loop_->isInLoopThread())
        {
            sendInLoop(message);
        }
        else
        {
            void (TcpConnection::*fp)(const StringPiece& message) = &TcpConnection::sendInLoop;
            loop_->runInLoop(std::bind(fp, this, message.as_string()));
        }
    }
}

// 将整个缓冲区发送
void TcpConnection::send(Buffer* buf)
{
    if (state_ == kConnected)
    {
        if (loop_->isInLoopThread())
        {
            sendInLoop(buf->peek(), buf->readableBytes());
            buf->retrieveAll();
        }
        else
        {
            void (TcpConnection::*fp)(const StringPiece& message) = &TcpConnection::sendInLoop;
            loop_->runInLoop(std::bind(fp, this, buf->retrieveAllAsString()));
        }
    }
}

// 整合逻辑最后所有流程归于下面的重载函数
void TcpConnection::sendInLoop(const StringPiece& message)
{
    sendInLoop(message.data(), message.size());
}

// 核心发送逻辑
void TcpConnection::sendInLoop(const void* data, size_t len)
{
    loop_->assertInLoopThread();
    ssize_t nwrote = 0;
    size_t remaining = len;
    bool faultError = false;
    // 先检查连接断没断开
    if (state_ == kDisconnected)
    {
        LOG_WARN << "disconnected, give up writing";
        return;
    }
    // 如果发送缓冲区中没有数据，尝试直接发送，避免拷贝到缓冲区再write
    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0)
    {
        nwrote = sockets::write(channel_->fd(), data, len);
        if (nwrote >= 0)
        {
            remaining = len - nwrote;
            if (remaining == 0 && writeCompleteCallback_)
            {
                loop_->queueInLoop(std::bind(writeCompleteCallback_, shared_from_this()));
            }
        }
        else // nwrote < 0
        {
            // 直接发送失败
            nwrote = 0;
            // EWOULDBLOCK: 发送缓冲区满
            if (errno != EWOULDBLOCK)
            {
                LOG_SYSERR << "TcpConnection::sendInLoop";
                // 对端关闭
                if (errno == EPIPE || errno == ECONNRESET) // FIXME: any others?
                {
                    faultError = true;
                }
            }
        }
    }
    // 数据可能在上面发送了，但是还有剩余
    assert(remaining <= len);
    if (!faultError && remaining > 0)
    {
        size_t oldLen = outputBuffer_.readableBytes();
        if (oldLen + remaining >= highWaterMark_
            && oldLen < highWaterMark_
            && highWaterMarkCallback_)
        {
            loop_->queueInLoop(std::bind(highWaterMarkCallback_, shared_from_this(), oldLen + remaining));
        }
        outputBuffer_.append(static_cast<const char*>(data)+nwrote, remaining);
        if (!channel_->isWriting())
        {
            channel_->enableWriting();
        }
    }
}
```
muduo通过巧妙设计，将所有的发送逻辑最后都归到```void TcpConnection::sendInLoop(const void* data, size_t len)```中统一发送，在这个函数中，如果发送缓冲区中没有数据，尝试直接调用write发送数据，如果缓冲区中有数据或者内核的发送缓冲区已满，则进入下层逻辑，如果旧数据 + 新数据(剩余数据)长度超过outbuffer的高水位，会进入发送限流防止写入速度 > 发送速度，outputBuffer无限增长，如果没超过则直接注册写事件，回调handlewrite进行数据发送


## Acceptor模块
muduo的架构是主从Reactor结构，Acceptor模块就是负责主Reactor接收新的连接，在muduo源码实现中，Acceptor并不会给连接创建TcpConnection对象，而是直接交由上层处理


> 成员变量和成员函数实现
``` cpp
class Acceptor : noncopyable
{
public:
    typedef std::function<void (int sockfd, const InetAddress&)> NewConnectionCallback;

    Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reuseport);
    ~Acceptor();

    void setNewConnectionCallback(const NewConnectionCallback& cb)
    { newConnectionCallback_ = cb; }

    void listen();

    bool listening() const { return listening_; }

    void Acceptor::listen()
    {
        loop_->assertInLoopThread();
        listening_ = true;
        acceptSocket_.listen();
        acceptChannel_.enableReading();
    }

    void Acceptor::handleRead()
    {
        loop_->assertInLoopThread();
        InetAddress peerAddr;
        //FIXME loop until no more
        int connfd = acceptSocket_.accept(&peerAddr);
        if (connfd >= 0)
        {
            if (newConnectionCallback_)
                newConnectionCallback_(connfd, peerAddr);
            else
                sockets::close(connfd);
        }
        else
        {
            LOG_SYSERR << "in Acceptor::handleRead";
            if (errno == EMFILE)
            {
                ::close(idleFd_);
                idleFd_ = ::accept(acceptSocket_.fd(), NULL, NULL);
                ::close(idleFd_);
                idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            }
        }
    }
 private:
    void handleRead();
    EventLoop* loop_;
    Socket acceptSocket_;
    Channel acceptChannel_;
    NewConnectionCallback newConnectionCallback_;
    bool listening_;         
    int idleFd_;
};
```
Acceptor整体实现非常简单，就是封装了一个监听fd和设置了一个读回调，在fd创建好监听读事件后，服务器就可以获取新连接了，连接就绪就会触发可读事件，并触发读回调将fd传给TcpServer进行Connection对象封装


## TcpServer模块
TcpServer = Acceptor（收连接） + EventLoopThreadPool（分发连接） + TcpConnection 管理（生命周期管理）通过创建一个TcpServer类(设置线程数，回调函数)就可以快速搭建一个可以进行业务处理的服务器


> 成员变量
``` cpp
EventLoop* loop_;       // baseLoop: 主Reactor用来接收连接，所有accept都在这里进行
const string ipPort_;   // 标识监听地址
const string name_;     // 服务名字(debug)
std::unique_ptr<Acceptor> acceptor_;               // 对监听fd的封装，当有新连接到来会调用newconnectionCallBack
std::shared_ptr<EventLoopThreadPool> threadPool_;  // 从Reactor线程池

ConnectionCallback connectionCallback_;            // 新连接到来的回调，一般绑定在acceptor的新连接回调
MessageCallback messageCallback_;                  // 收到消息的业务处理回调
WriteCompleteCallback writeCompleteCallback_;      // 写完成回调
ThreadInitCallback threadInitCallback_;            // 线程初始化回调 设置线程名 初始化 TLS 日志绑定
AtomicInt32 started_;
// always in loop thread
int nextConnId_;                                   // 给连接生成id

typedef std::map<string, TcpConnectionPtr> ConnectionMap;
ConnectionMap connections_;                        // 整个服务器上所有的连接
```


> 成员函数及其实现
``` cpp
// 一键搭建服务器
TcpServer::TcpServer(EventLoop* loop,
                     const InetAddress& listenAddr,
                     const string& nameArg,
                     Option option)
   :loop_(CHECK_NOTNULL(loop)),
    ipPort_(listenAddr.toIpPort()),
    name_(nameArg),
    acceptor_(new Acceptor(loop, listenAddr, option == kReusePort)),
    threadPool_(new EventLoopThreadPool(loop, name_)),
    connectionCallback_(defaultConnectionCallback),
    messageCallback_(defaultMessageCallback),
    nextConnId_(1)
{
  acceptor_->setNewConnectionCallback(
      std::bind(&TcpServer::newConnection, this, _1, _2));
}

void TcpServer::start()
{
    if (started_.getAndSet(1) == 0)
    {
        // 先启动从Reactor后再开启接收新连接
        threadPool_->start(threadInitCallback_);

        assert(!acceptor_->listening());
        // 启动获取新连接
        loop_->runInLoop(
            std::bind(&Acceptor::listen, get_pointer(acceptor_)));
    }
}

void TcpServer::newConnection(int sockfd, const InetAddress& peerAddr)
{
    loop_->assertInLoopThread();
    // 默认采用RR轮转的方式给新连接分配loop
    EventLoop* ioLoop = threadPool_->getNextLoop();


    char buf[64];
    snprintf(buf, sizeof buf, "-%s#%d", ipPort_.c_str(), nextConnId_);
    ++nextConnId_;
    string connName = name_ + buf;
    LOG_INFO << "TcpServer::newConnection [" << name_
            << "] - new connection [" << connName
            << "] from " << peerAddr.toIpPort();
    InetAddress localAddr(sockets::getLocalAddr(sockfd));
    // FIXME poll with zero timeout to double confirm the new connection
    // FIXME use make_shared if necessary

    TcpConnectionPtr conn(new TcpConnection(ioLoop,
                                            connName,
                                            sockfd,
                                            localAddr,
                                            peerAddr));
    // 在Server中保存连接
    connections_[connName] = conn;
    // 给连接设置回调函数
    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);
    conn->setCloseCallback(
        std::bind(&TcpServer::removeConnection, this, _1)); // FIXME: unsafe
    ioLoop->runInLoop(std::bind(&TcpConnection::connectEstablished, conn));
}

void TcpServer::removeConnection(const TcpConnectionPtr& conn)
{
    // FIXME: unsafe
    loop_->runInLoop(std::bind(&TcpServer::removeConnectionInLoop, this, conn));
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn)
{
    loop_->assertInLoopThread();
    LOG_INFO << "TcpServer::removeConnectionInLoop [" << name_
            << "] - connection " << conn->name();
    // 从map中擦除
    size_t n = connections_.erase(conn->name());
    (void)n;
    assert(n == 1);
    EventLoop* ioLoop = conn->getLoop();
    // 调用连接销毁
    ioLoop->queueInLoop(
        std::bind(&TcpConnection::connectDestroyed, conn));
}
```
可以发现，在muduo中很多映射的地方都使用了map -> 红黑树: O(logN)而不是unordered_map -> 哈希:O(1)，在connetion的管理中也是使用了map来做管理，主要是为了网络库稳定性的考量
1. unordered_map可以做到平均O(1)时间复杂度，但是如果遇到大量哈希冲突可能退化到O(N)，而红黑树是稳定的O(logN)不会产生退化
2. 在遍历哈希表的时候可能出现迭代器失效的情况(refresh)，而红黑树除了插入删除，不会使迭代器失效
3. 每个EventLoop实际上可能管理的连接数量并不多，map与哈希表的性能可以做到差不多
4. 哈希表扩容: 重新分配 bucket，所有元素重新hash，O(N)

总结来看:muduo 使用 map 而不是 unordered_map，是因为在网络库中比起平均性能，更重要的是性能的稳定性和可预测性。map 提供稳定的 O(logN) 复杂度，并且插入删除不会导致迭代器失效，而 unordered_map 在 rehash 时会导致迭代器失效并产生性能抖动，在高并发场景下是不安全的。


# muduo各个模块及函数调用示意图
## 模块关系总图
``` text
TcpServer
 ├── Acceptor
 │    └── Socket (listenfd)
 │    └── Channel (acceptChannel)
 │
 ├── EventLoop (baseLoop)
 │    ├── Poller
 │    └── Channel
 │
 ├── EventLoopThreadPool
 │    └── EventLoopThread
 │          └── EventLoop (subLoop)
 │
 └── connections (map<string, TcpConnection>)
        └── TcpConnection
              ├── Socket (connfd)
              ├── Channel
              ├── Buffer (inputBuffer)
              ├── Buffer (outputBuffer)
              └── EventLoop (所属 subLoop)
```
从此图来看整个网络库中最重要的当属两个模块: EventLoop和TcpConnection，其中eventloop负责每个监控每个fd关心的事件、事件回调处理和更新channel关心的事件；TcpConnection则负责管理一个连接的数据发送，接收... 


## 服务器启动调用链
``` text
main()
  ↓
TcpServer::TcpServer()
  ↓
Acceptor(loop, listenAddr)
  ↓
Socket::bind()
Socket::listen()
Channel::setReadCallback(handleRead)
  ↓
TcpServer::start()
  ↓
EventLoopThreadPool::start()
  ↓
EventLoopThread::startLoop()
  ↓
threadFunc()
  ↓
EventLoop loop;
loop.loop()
  ↓
Acceptor::listen()
  ↓
Channel::enableReading()
```
整个服务器的启动的入口就是TcpServer模块，TcpServer中封装了Acceptor模块，通过Acceptor接收到新连接后再分发给EventLoopThreadPool进行IO处理


## 新连接建立调用链
``` text
EventLoop::loop()
  ↓
Poller::poll()
  ↓
Channel::handleEvent()
  ↓
Acceptor::handleRead()
  ↓
accept()
  ↓
newConnectionCallback_(sockfd, addr)
  ↓
TcpServer::newConnection()
  ↓
  EventLoopThreadPool::getNextLoop()
  ↓
  new TcpConnection(loop, sockfd)
  ↓
    TcpConnection::TcpConnection()
      ↓
      Channel(fd)
      Channel::setReadCallback(handleRead)
      Channel::setWriteCallback(handleWrite)
      Channel::setCloseCallback(handleClose)

  ↓
  connections_[connName] = conn
  ↓
  TcpConnection::connectEstablished()
    ↓
    Channel::tie(shared_ptr)
    Channel::enableReading()
```


## 数据接收链(Read)
``` text
EventLoop::loop()
  ↓
Poller::poll()
  ↓
activeChannels
  ↓
Channel::handleEvent()
  ↓
TcpConnection::handleRead()
  ↓
Buffer::readFd()
  ↓
read(fd, buffer)
  ↓
append 到 inputBuffer
  ↓
messageCallback_(conn, buffer)
```


## 数据发送链(Write)
> 用户调用send
``` text
TcpConnection::send()
  ↓
  if (inLoopThread)
      sendInLoop()
  else
      loop->runInLoop(sendInLoop)
```

> 真正发送逻辑
``` text
TcpConnection::sendInLoop()
  ↓
  write(fd, data)
  ↓
  if (没发完)
      outputBuffer.append()
      Channel::enableWriting()
```

> epoll 触发写事件
``` text
EventLoop::loop()
  ↓
Channel::handleEvent()
  ↓
TcpConnection::handleWrite()
  ↓
write(fd, outputBuffer)
  ↓
Buffer::retrieve()
  ↓
if (写完)
    Channel::disableWriting()
```


## 连接关闭链
``` text
TcpConnection::handleClose()
  ↓
Channel::disableAll()
  ↓
closeCallback_(conn)
  ↓
TcpServer::removeConnection()
  ↓
EventLoop::runInLoop(removeConnectionInLoop)
  ↓
connections_.erase()
```


## 总体函数调用
``` text
main
 ↓
TcpServer
 ├── Acceptor
 │    └── Channel → EventLoop
 │
 ├── EventLoopThreadPool
 │    └── EventLoopThread → EventLoop(loop)
 │
 └── TcpConnection
      ├── Channel → EventLoop
      ├── Socket
      ├── Buffer(input/output)
      └── callbacks

========================

事件驱动核心：
EventLoop::loop
  ↓
Poller::poll
  ↓
Channel::handleEvent
  ↓
  ├── Acceptor::handleRead → newConnection
  └── TcpConnection::handleRead / handleWrite

========================

数据流：
read:
kernel → fd → Buffer → callback

write:
user → Buffer → fd → kernel
```


# 对比Muduo源码个人实现设计不足的点
## Buffer模块
- muduo源码在buffer空间头部预留了协议头的空间，允许在后续直接回填协议头，不必重新分配一整块新内存，减少了协议头和包体重新拼接复制移动产生的消耗，后续可以通过prepend()将协议头添加到头部空间

- muduo有内存收缩机制，当buffer经过传输峰值后，如果长时间不再使用大块内存，muduo会有shrink()创建一个新的buffer并将旧buffer的元素copy到新buffer中，同时将内存收缩到正常大小，这样就不会存在长时间buffer持有大块空间内存，导致缓存命中率低，同时可以节约内存

- muduo的buffer在从内核缓冲区中(fd)中拿数据做了优化，使用了readv，使原本需要循环读取的内容通过一次系统调用就可以将数据读到多个缓冲区，参数为fd, iovec(缓冲区列表), n(缓冲区个数)，在muduo中，它在readfd中额外开辟了一块栈区空间(extrabuffer)大小为65535字节，当buffer剩余写空间不足的时候，会将剩余数据写到临时缓冲区中，最后将两个缓冲区合并，在此过程中避免了缓冲区重复开辟空间和数据的复制，节约了系统开销

- muduo优化了字符串传参，当内存收缩需要将数据拷贝到新缓冲区的时候，muduo采用了StringPiece(字符串视图)的方式，它保存了指向字符串的指针和字符串长度，在传参的时候不需要重新构造一次临时string对象，节约了一次潜在的额外拷贝

- muduo预留了网络字节序接口，简化了网络协议拼包和拆包过程，把他们变成了统一、可内联、少临时对象、少重复扫描的流程。如果没有预留接口，业务层需要先转换字节序，然后存到临时空间，再分别写入协议头和body，这样做可以减少临时对象和重复拷贝，减少系统开销

## Socket模块

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
同时也用addedToLoop_标记channel是否添加到loop，防止重复添加和remove未添加的channel



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

## EventLoop模块

## EventLoopThread模块

## EventLoopThreadPool模块

## TcpConnection模块

## TcpServer模块

## Acceptor模块

## 整体架构和工程细节



