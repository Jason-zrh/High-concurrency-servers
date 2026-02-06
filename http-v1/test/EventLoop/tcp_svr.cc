#include "../../source/server.hpp"

// 设置回调函数
void HandleClose(Channel* channel)
{
    std::cout << "Close: " << channel->GetFd() << std::endl;
    channel->Remove();// 移除监控
    close(channel->GetFd());
}

void HandleRead(Channel* channel)
{
    int fd = channel->GetFd();
    char buf[1024] = {0};
    int ret = recv(fd, buf, sizeof(buf), 0);
    if(ret <= 0)
        return HandleClose(channel);
    
    // 服务器要开始向客户端返回信息，所以要开启对可写事件的监控
    std::cout << buf << std::endl;
    channel->EnableWrite(); 
}

void HandleWrite(Channel* channel)
{
    char buf[1024] = "To ByteDance !!!";
    // 修改处：只发送有效字符串长度，不发送后面的 \0 填充
    // 如果需要包含结尾的 \0，可以使用 strlen(buf) + 1，但在网络文本传输中通常不发 \0
    int len = strlen(buf); 
    int ret = send(channel->GetFd(), buf, len, 0); 
    
    if(ret <= 0)
        return HandleClose(channel);
    
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
void Acceptor(EventLoop* loop, Channel* lis_channel)
{
    int newfd = accept(lis_channel->GetFd(), nullptr, nullptr);
    if(newfd < 0) 
        return;

    // 给获取上来的通信套接字创建channel进行管理
    Channel* channel = new Channel(loop, newfd);
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
    EventLoop loop;
    // 构建监听服务器
    bool ret = sock.CreateServer(8080); // 不是进行通信的fd(是在饭店门口揽客的)
    // 管理链接的文件描述符
    Channel channel(&loop, sock.GetFd());
    // 设置回调函数
    channel.SetReadCallBack(std::bind(Acceptor, &loop, &channel));
    channel.EnableRead(); // 开始关注该文件描述符的读事件，读事件就绪->获取到新链接了

    while (1)
    {
        loop.Start();
    }

    sock.Close();
    return 0;
}