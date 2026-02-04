#include "../../source/server.hpp"

// 用于管理新的连接
std::unordered_map<uint64_t, ConnectionPtr> _conns;
uint64_t connect_id = 0;

void OnMessage(const ConnectionPtr &conn, Buffer *buf)
{
    std::string msg = buf->ReadAsString(buf->ReadAbleSize());
    DBG_LOG("Recv: %s", msg.c_str());

    std::string str = "Hello Tiktok!";
    conn->Send(str.c_str(), str.size());
}

void OnConnected(const ConnectionPtr &conn)
{
    DBG_LOG("New Connection id=%lu fd=%d", conn->GetId(), conn->GetFd());
}

void ConnectionDestory(const ConnectionPtr &conn)
{
    DBG_LOG("Connection Destroy id=%lu", conn->GetId());
    _conns.erase(conn->GetId());
}

// 接收新连接
void Acceptor(EventLoop *loop, Channel *lis_channel)
{
    while (true) // ★ 一次性 accept 干净（ET/LT 都安全）
    {
        int newfd = accept(lis_channel->GetFd(), nullptr, nullptr);
        if (newfd < 0)
        {
            if (errno == EAGAIN || errno == EINTR)
                break;
            ERR_LOG("accept error");
            break;
        }

        connect_id++;
        ConnectionPtr conn(new Connection(loop, connect_id, newfd));

        conn->SetServerCloseCallBack(ConnectionDestory);
        conn->SetConnectCallBack(OnConnected);
        conn->SetMsgCallBack(OnMessage);

        conn->EnableInactiveRealse(10);
        conn->Established();

        _conns.emplace(connect_id, conn);
    }
}

int main()
{
    Socket sock;
    EventLoop loop;

    bool ret = sock.CreateServer(8080, "0.0.0.0", false);
    assert(ret);

    Channel channel(&loop, sock.GetFd());
    channel.SetReadCallBack(std::bind(Acceptor, &loop, &channel));
    channel.EnableRead();

    // ★ EventLoop 自己内部是 while(looping)
    loop.Start();

    sock.Close();
    return 0;
}
