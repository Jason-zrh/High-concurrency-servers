// #include "http.hpp"

// #define WWWROOT "./wwwroot/"

// std::string RequestStr(const HttpRequest &req) {
//     std::stringstream ss;
//     ss << req._method << " " << req._path << " " << req._version << "\r\n";
//     for (auto &it : req._params) {
//         ss << it.first << ": " << it.second << "\r\n";
//     }
//     for (auto &it : req._headers) {
//         ss << it.first << ": " << it.second << "\r\n";
//     }
//     ss << "\r\n";
//     ss << req._body;
//     return ss.str();
// }
// void Hello(const HttpRequest &req, HttpResponse *rsp) 
// {
//     rsp->SetContent(RequestStr(req), "text/plain");
// }
// void Login(const HttpRequest &req, HttpResponse *rsp) 
// {
//     rsp->SetContent(RequestStr(req), "text/plain");
// }
// void PutFile(const HttpRequest &req, HttpResponse *rsp) 
// {
//     std::string pathname = WWWROOT + req._path;
//     Util::WriteFile(pathname, req._body);
// }
// void DelFile(const HttpRequest &req, HttpResponse *rsp) 
// {
//     rsp->SetContent(RequestStr(req), "text/plain");
// }
// int main()
// {
//     HttpServer server(8080);
//     server.SetThreadCount(4);
//     server.SetBaseDir(WWWROOT);//设置静态资源根目录，告诉服务器有静态资源请求到来，需要到哪里去找资源文件
//     server.Get("/hello", Hello);
//     server.Post("/login", Login);
//     server.Put("/1234.txt", PutFile);
//     server.Delete("/1234.txt", DelFile);
//     server.Listen();
//     return 0;
// }


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