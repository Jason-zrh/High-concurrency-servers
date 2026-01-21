#include <iostream>
#include <regex>
#include <string>


int main()
{
    // HTTP请求格式: GET /bytedance/login?user=jason&pass=20051027 HTTP/1.1\r\n
    std::string str = "GET /bytedance/login HTTP/1.1\r\n";
    std::regex e("(GET|POST|HEAD|PUT|DELETE) ([^?]*)(?:\\?(.*))? (HTTP/1\\.[01])(?:\n|\r\n)?");
    /*
     * 正则表达式
     * (GET|POST|HEAD|PUT|DELETE) 中的‘｜’是或者的意思
     * ([^?]*) 捕捉除了'?'以外的字符1个或多个
     * (HTTP/1\\.[01]) [01]是指0或1
     * (.*) 捕获任意字符除了'\r\n'1个或多个
     * (?:\n|\r\n)? '?:'代表匹配但是不捕捉, 最后的'?'代表前面的一句表达式是可选的
    */ 
    std::smatch matches;
    bool ret = std::regex_match(str, matches, e);
    if(ret == false)
        return -1;
    for(auto& it : matches)
    {
        std::cout << it << std::endl;
    }
    return 0;
}