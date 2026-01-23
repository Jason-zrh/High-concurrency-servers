#pragma once
#include <iostream>
#include <time.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

// 设置日志等级
#define SIZE      1024
#define Info      0    // 通知
#define Debug     1    // 调试
#define Warning   2    // 警告 
#define Error     3    // 报错 
#define Fatal     4    // 致命 

// 设置打印方式
#define Screen    0    // 屏幕打印 
#define Onefile   1    // 在一个文件中打印 
#define Classfile 2    // 在屏幕上打印 

// 存储日志的文件名字
#define FILENAME "log.txt"
using namespace std;

// 日志类
class Log
{
public:
    Log()
    {
        // 默认是屏幕打印
        _showmethod = Screen;
        // 如果改成保存到文件中的默认路径
        _path = "./log/";
    }

    // 改变日志打印方式
    void Enable(int method)
    {
        _showmethod = method;
    }

    // 将日志等级转化成字符串
    string levelToString(int level)
    {
        switch(level)
        {
            case Info:
                return "Info";
            case Debug:
                return "Debug";
            case Warning:
                return "Warning";
            case Error:
                return "Error";
            case Fatal:
                return "Fatal";
            default:
                return "NONE";
        }
    }


    void printlog(int leval, string logtext)
    {
        switch (_showmethod)
        {
            case Screen:
                // 直接在屏幕上打印
                cout << logtext << endl;
                break;
            case Onefile:
                // 在一个文件中写所有日志
                printOnefile(FILENAME, logtext + "\n");
                break;
            case Classfile:
                // 将日志分类
                printClassfile(leval, logtext + "\n");
                break;
            default:
                break;
        }

    }

    void printOnefile(const string& pathname, const string& logtext)
    {
        string _pathname = _path + pathname;
        int fd = open(_pathname.c_str(), O_APPEND|O_CREAT|O_WRONLY, 0666);
        if(fd < 0) return;
        // 向打开的文件中写日志
        write(fd, logtext.c_str(), logtext.size());
        // 写后关闭fd
        close(fd);
    }

    void printClassfile(int leval, const string& logtext)
    {
        string filename = FILENAME;
        filename += ".";
        filename += levelToString(leval);
        printOnefile(filename, logtext);
    }

    // 重载()运算符，做到直接打印日志
    void operator()(int level, const char* format, ...)
    {
        // 日志的格式: 默认部分 + 自定义部分
        // 默认部分: 等级 + 时间
        char leftmessage[SIZE];
        time_t t = time(nullptr);
        struct tm* ctime = localtime(&t);
        snprintf(leftmessage, sizeof(leftmessage), "[%s][%d-%d-%d %d:%d:%d]:",levelToString(level).c_str(), ctime->tm_year + 1900, ctime->tm_mon + 1, ctime->tm_mday, ctime->tm_hour, ctime->tm_min, ctime->tm_sec);

        // 自定义部分:
        char rightmessage[SIZE];
        va_list s;
        va_start(s, format);
        vsnprintf(rightmessage, sizeof(rightmessage), format, s);
        va_end(s);

        // 日志合到一起
        char logtext[SIZE * 2];
        snprintf(logtext, sizeof(logtext), "%s %s", leftmessage, rightmessage);
        printlog(level, logtext);
    }

    ~Log()
    { }
private:
    int _showmethod;
    string _path;
};
