// #include "../server.hpp"

// class EchoServer {
//     private:
//         TcpServer _server;
//     private:
//         void OnConnected(const ConnectionPtr &conn) {
//             DBG_LOG("NEW CONNECTION:%p", conn.get());
//         }
//         void OnClosed(const ConnectionPtr &conn) {
//             DBG_LOG("CLOSE CONNECTION:%p", conn.get());
//         }
//         void OnMessage(const ConnectionPtr &conn, Buffer *buf) {
//             conn->Send(buf->ReadPos(), buf->ReadAbleSize());
//             buf->MoveReadOffset(buf->ReadAbleSize());
//             conn->ShutDown();
//         }
//     public:
//         EchoServer(int port):_server(port) {
//             _server.SetThreadCount(4);
//             _server.EnableInactiveRealse(10);
//             _server.SetCloseCallBack(std::bind(&EchoServer::OnClosed, this, std::placeholders::_1));
//             _server.SetConnectCallBack(std::bind(&EchoServer::OnConnected, this, std::placeholders::_1));
//             _server.SetMsgCallBack(std::bind(&EchoServer::OnMessage, this, std::placeholders::_1, std::placeholders::_2));
//         }
//         void Start() { _server.Start(); }
// };

// #include "../server.hpp"

// class EchoServer
// {
// private:
//     TcpServer _server;

// private:
//     void OnConnected(const ConnectionPtr &conn)
//     {
//         DBG_LOG("NEW CONNECTION:%p", conn.get());
//     }

//     void OnClosed(const ConnectionPtr &conn)
//     {
//         DBG_LOG("CLOSE CONNECTION:%p", conn.get());
//     }

//     void OnMessage(const ConnectionPtr &conn, Buffer *buf)
//     {
//         // 1️⃣ 丢弃请求内容（你现在不解析 HTTP）
//         buf->MoveReadOffset(buf->ReadAbleSize());

//         // 2️⃣ 构造最小 HTTP/1.1 响应
//         const char *body = "Hello";
//         std::string resp =
//             "HTTP/1.1 200 OK\r\n"
//             "Content-Length: 5\r\n"
//             "Connection: close\r\n"
//             "\r\n"
//             "Hello";

//         // 3️⃣ 发送完整响应
//         conn->Send(resp.c_str(), resp.size());

//         // 4️⃣ 关键：不要立刻 close
//         //    等对端读完，或者让底层 write 完成后再关
//         conn->ShutDown();
//     }

// public:
//     EchoServer(int port)
//         : _server(port)
//     {
//         _server.SetThreadCount(4);
//         _server.EnableInactiveRealse(10);
//         _server.SetCloseCallBack(
//             std::bind(&EchoServer::OnClosed, this, std::placeholders::_1));
//         _server.SetConnectCallBack(
//             std::bind(&EchoServer::OnConnected, this, std::placeholders::_1));
//         _server.SetMsgCallBack(
//             std::bind(&EchoServer::OnMessage, this,
//                       std::placeholders::_1, std::placeholders::_2));
//     }

//     void Start() { _server.Start(); }
// };


#include "../server.hpp"

class EchoServer {
private:
    TcpServer _server;

private:
    void OnConnected(const ConnectionPtr &conn) {
        DBG_LOG("NEW CONNECTION: %p", conn.get());
    }

    void OnClosed(const ConnectionPtr &conn) {
        DBG_LOG("CLOSE CONNECTION: %p", conn.get());
    }

    void OnMessage(const ConnectionPtr &conn, Buffer *buf) {
        // 1️⃣ 读取完整请求并丢弃
        buf->MoveReadOffset(buf->ReadAbleSize());

        // 2️⃣ 检查请求中是否包含 Connection: keep-alive
        //    这里我们简单用 buf->ReadPos() 查找字符串即可
        //    如果是生产环境，应该用 parser
        bool keep_alive = false;
        std::string request(buf->ReadPos(), buf->ReadAbleSize());
        if (request.find("Connection: keep-alive") != std::string::npos ||
            request.find("connection: keep-alive") != std::string::npos) {
            keep_alive = true;
        }

        // 3️⃣ 构造最小 HTTP/1.1 响应
        const char* body = "Hello";
        std::string resp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 5\r\n"
            + std::string("Connection: ") + (keep_alive ? "keep-alive" : "close") + "\r\n"
            "\r\n"
            "Hello";

        // 4️⃣ 发送响应
        conn->Send(resp.c_str(), resp.size());

        // 5️⃣ 决定是否关闭连接
        if (!keep_alive) {
            conn->ShutDown();  // 仅关闭不复用连接
        }
        // 如果 keep_alive = true，则继续监听，可复用连接
    }

public:
    EchoServer(int port)
        : _server(port) {
        _server.SetThreadCount(4);
        _server.EnableInactiveRealse(10);
        _server.SetCloseCallBack(
            std::bind(&EchoServer::OnClosed, this, std::placeholders::_1));
        _server.SetConnectCallBack(
            std::bind(&EchoServer::OnConnected, this, std::placeholders::_1));
        _server.SetMsgCallBack(
            std::bind(&EchoServer::OnMessage, this,
                      std::placeholders::_1, std::placeholders::_2));
    }

    void Start() { _server.Start(); }
};



