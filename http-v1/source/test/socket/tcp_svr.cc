#include "server.hpp"

int main()
{
    // TCP服务器
    Socket sock;
    // 一键构建服务器
    bool ret = sock.CreateServer(8080); // 不是进行通信的fd(是在饭店门口揽客的)
    while (1)
    {
        int newfd = sock.Accept(); // accept上来的才是真正用来通信的(饭店里一桌一桌的进行服务)
        if (newfd < 0)
            continue;

        Socket cli_sock(newfd);
        char recv[1024];
        int n = cli_sock.Recv(recv, sizeof(recv) - 1);
        if (n > 0)
            recv[n] = 0;

        std::cout << recv << std::endl;
        char send[1024] = "Server recv a msg!";
        cli_sock.Send(send, sizeof(send));
        cli_sock.Close();
    }
    sock.Close();
    return 0;
}