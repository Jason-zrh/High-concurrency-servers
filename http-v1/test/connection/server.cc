#include "../../source/server.hpp"

// 用于管理新的连接
std::unordered_map<uint64_t, ConnectionPtr> _conns;
u_int64_t connect_id = 0;

void OnMessage(const ConnectionPtr &conn, Buffer *buf)
{
    std::string msg = buf->ReadAsString(buf->ReadAbleSize());
    DBG_LOG("%s", msg.c_str());

    std::string str = "Hello Tiktok!";
    conn->Send(str.c_str(), str.size());
}

void OnConnected(const ConnectionPtr &conn)
{
    DBG_LOG("New Connection: %p", conn.get());
}

void ConnectionDestory(const ConnectionPtr &conn)
{
    _conns.erase(conn->GetId());
}

// 设置监听服务器的读回调，实际上就是获取链接
void Acceptor(EventLoop *loop, Channel *lis_channel)
{
    int newfd = accept(lis_channel->GetFd(), nullptr, nullptr);
    if (newfd < 0)
        return;

    // 给获取上来的通信套接字创建channel进行管理
    connect_id++;
    ConnectionPtr conn(new Connection(loop, connect_id, newfd));
    conn->SetServerCloseCallBack(std::bind(ConnectionDestory, std::placeholders::_1));
    conn->SetConnectCallBack(std::bind(OnConnected, std::placeholders::_1));
    conn->SetMsgCallBack(std::bind(OnMessage, std::placeholders::_1, std::placeholders::_2));

    // 启动非活跃销毁
    conn->EnableInactiveRealse(10);
    conn->Established();

    _conns.insert(std::make_pair(connect_id, conn));
}

int main()
{
    srand(time(NULL));
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