#include "../../source/server.hpp"

// 用于管理新的连接
std::unordered_map<uint64_t, ConnectionPtr> _conns;
uint64_t connect_id = 0;

// 主线程
EventLoop Base_loop;

// 从线程
std::vector<LoopThread> threads(2);
int thread_select = 0;

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
void NewConnection(int newfd)
{

    connect_id++;
    thread_select = (thread_select + 1) % 2;
    ConnectionPtr conn(new Connection(threads[thread_select].GetLoop(), connect_id, newfd));

    conn->SetServerCloseCallBack(ConnectionDestory);
    conn->SetConnectCallBack(OnConnected);
    conn->SetMsgCallBack(OnMessage);

    conn->EnableInactiveRealse(10);
    conn->Established();

    _conns.emplace(connect_id, conn);
}

int main()
{
    Socket sock;
    Acceptor acceptor(&Base_loop, 8080);
    acceptor.SetAcceptCallBack(std::bind(NewConnection, std::placeholders::_1));
    acceptor.Listen();

    // ★ EventLoop 自己内部是 while(looping)
    Base_loop.Start();

    sock.Close();
    return 0;
}