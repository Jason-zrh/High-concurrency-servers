// Linux下简单的定时器系统
#include <sys/timerfd.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <ctime>
#include <cstdio>

using namespace std;


int main()
{
    // clockid: CLOCK_REALTIME-系统实时时间，如果修改了系统时间就会出问题；
    //          CLOCK_MONOTONIC-从开机到现在的时间是一种相对时间；
    // flags: 0-默认阻塞属性       
    
    int timerfd = timerfd_create(CLOCK_MONOTONIC, 0); // 默认阻塞操作
    if(timerfd < 0)
    {
        perror("timerfd_create error");
        return -1;
    }

    struct itimerspec itime;
    
    itime.it_value.tv_sec = 3;  // 此处为设置超时时间3s
    itime.it_value.tv_nsec = 0; // 防止纳秒变成随机值设为0

    itime.it_interval.tv_sec = 3; // 第一次超时后，每次超时的间隔时间
    itime.it_interval.tv_nsec = 0;

    timerfd_settime(timerfd, 0, &itime, nullptr);


    while(1)
    {
        uint64_t exp;
        int ret = read(timerfd, &exp, sizeof(time));
    }
    return 0;
}