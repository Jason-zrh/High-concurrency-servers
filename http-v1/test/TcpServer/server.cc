#include "../../source/server.hpp"


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
}

int main()
{
    TcpServer server(8080);
    server.SetThreadCount(2);
    server.EnableInactiveRealse(10);
    server.SetMsgCallBack(OnMessage);
    server.SetCloseCallBack(ConnectionDestory);
    server.SetConnectCallBack(OnConnected);
    server.Start();
}