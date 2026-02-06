#include "../server.hpp"

class EchoServer {
    private:
        TcpServer _server;
    private:
        void OnConnected(const ConnectionPtr &conn) {
            DBG_LOG("NEW CONNECTION:%p", conn.get());
        }
        void OnClosed(const ConnectionPtr &conn) {
            DBG_LOG("CLOSE CONNECTION:%p", conn.get());
        }
        void OnMessage(const ConnectionPtr &conn, Buffer *buf) {
            conn->Send(buf->ReadPos(), buf->ReadAbleSize());
            buf->MoveReadOffset(buf->ReadAbleSize());
            conn->ShutDown();
        }
    public:
        EchoServer(int port):_server(port) {
            _server.SetThreadCount(2);
            _server.EnableInactiveRealse(10);
            _server.SetCloseCallBack(std::bind(&EchoServer::OnClosed, this, std::placeholders::_1));
            _server.SetConnectCallBack(std::bind(&EchoServer::OnConnected, this, std::placeholders::_1));
            _server.SetMsgCallBack(std::bind(&EchoServer::OnMessage, this, std::placeholders::_1, std::placeholders::_2));
        }
        void Start() { _server.Start(); }
};