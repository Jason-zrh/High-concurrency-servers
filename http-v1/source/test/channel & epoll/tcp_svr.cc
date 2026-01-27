#include "server.hpp"

void HandleClose(Channel* channel)
{
    std::cout << "Close: " << channel->GetFd() << std::endl;
    channel->Remove();// 移除监控
    delete channel;
}

void HandleRead(Channel* channel)
{
    int fd = channel->GetFd();
    char buf[1024] = {0};
    int ret = recv(fd, buf, sizeof(buf), 0);
    if(ret <= 0)
        return HandleClose(channel);
    
    channel->EnableWrite(); // 开启对可写事件的监控
    std::cout << recv << std::endl;
}

void HandleWrite(Channel* channel)
{
    char buf[1024] = "To ByteDance !!!";
    int ret = send(channel->GetFd(), buf, sizeof(buf), 0);
    if(ret <= 0)
        return HandleClose(channel);
    
    // 无数据可写，关闭对写事件的监控
    channel->DisableWrite();
}

void HandleError(Channel* channel)
{
    return HandleClose(channel);
}

void HandleEvent(Channel* channel)
{
    std::cout << "Get a msg !" << std::endl;
}

// 设置监听服务器的读回调，实际上就是获取链接
void Acceptor(Poller* poller, Channel* lis_channel)
{
    int newfd = accept(lis_channel->GetFd(), nullptr, nullptr);
    if(newfd < 0) { return; }

    // 给获取上来的通信套接字创建channel进行管理
    Channel* channel = new Channel(poller, newfd);
    channel->SetReadCallBack(std::bind(HandleRead, channel));
    channel->SetWriteCallBack(std::bind(HandleWrite, channel));
    channel->SetErrorCallBack(std::bind(HandleError, channel));
    channel->SetCloseCallBack(std::bind(HandleClose, channel));
    channel->SetEventCallBack(std::bind(HandleEvent, channel));
    channel->EnableRead();
}

int main()
{
    // TCP服务器
    Socket sock;
    Poller epoll;
    // Channel管理文件描述符和poller
    Channel channel(&epoll, sock.GetFd());
    // 设置回调函数
    channel.SetReadCallBack(std::bind(Acceptor, &epoll, &channel));
    channel.EnableRead(); // 开始关注该文件描述符的读事件，读事件就绪->获取到新链接了
    // 构建监听服务器
    bool ret = sock.CreateServer(8080); // 不是进行通信的fd(是在饭店门口揽客的)
    while (1)
    {
       std::vector<Channel* > actives;
       epoll.Poll(&actives);
       for(auto& e : actives)
        e->HandleEvent();
    }
    sock.Close();
    return 0;
}