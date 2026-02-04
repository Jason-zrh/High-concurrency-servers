#include "../../source/server.hpp"

int main()
{
    Socket sock;
    bool ret = sock.CreateClient(8080, "127.0.0.1");
    assert(ret);

    for (int i = 0; i < 5; i++)
    {
        char* msg = "Hello muduo";
        sock.Send(msg, sizeof(msg));   // ★ 不能用 sizeof

        char recvbuf[1024] = {0};
        int n = sock.Recv(recvbuf, sizeof(recvbuf) - 1);
        if (n > 0)
        {
            recvbuf[n] = 0;
            std::cout << "Server reply: " << recvbuf << std::endl;
        }

        sleep(1);
    }

    sock.Close();   // ★ 主动关闭，触发服务器 close 回调
    return 0;
}
