#include "http.hpp"

#define WWWROOT "./wwwroot"

// 一个简单的动态接口（可选，用于对比静态资源）
void Hello(const HttpRequest &req, HttpResponse *rsp)
{
    std::string body = "Hello HTTP Server\n";
    body += "Method: " + req._method + "\n";
    body += "Path: " + req._path + "\n";
    rsp->SetContent(body, "text/plain");
}

int main()
{
    // 1️⃣ 创建 HTTP 服务器，监听 8080 端口
    HttpServer server(8080);

    // 2️⃣ 设置 IO 线程数（性能测试关键）
    server.SetThreadCount(4);

    // 3️⃣ 设置静态资源根目录
    server.SetBaseDir(WWWROOT);

    // 4️⃣ 注册一个简单的动态接口（非必须）
    server.Get("/hello", Hello);

    // 5️⃣ 启动服务器（内部调用 TcpServer::Start）
    server.Listen();

    return 0;
}