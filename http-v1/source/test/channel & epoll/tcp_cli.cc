#include "server.hpp"

int main()
{
    Socket sock;
    sock.CreateClient(8080, "111.229.73.240");

    while (1)
    {
        char buffer[1024] = "Hello muduo!";
        sock.Send(buffer, sizeof(buffer));

        char recv[1024] = {0};
        int n = sock.Recv(recv, sizeof(recv) - 1);

        recv[n] = 0;
        std::cout << recv << std::endl;
        sleep(1);
    }

    return 0;
}