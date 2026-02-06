#include "echo.hpp"

int main()
{
    EchoServer server(8080);
    server.Start();
    return 0;
}