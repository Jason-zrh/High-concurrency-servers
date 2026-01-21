// bind使用方法
#include <iostream>
#include <string>
#include <functional>
#include <vector>
#include <unistd.h>
using namespace std;
using Task = function<void()>;

void print(const string& str)
{
    cout << str << endl;
}

int main()
{
    vector<Task> tasks;
    tasks.push_back(bind(print, "你好👋"));
    tasks.push_back(bind(print, "拜拜👋"));
    tasks.push_back(bind(print, "字节跳动!"));
    tasks.push_back(bind(print, "我踏马来辣!"));

    for(auto& f : tasks)
    {
        f();
        sleep(1);
    }
    return 0;
}