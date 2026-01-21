// 正则表达式的使用
#include <iostream>
#include <regex>
#include <string>

using namespace std;

int main()
{
    string str = "/bytedance/1027";
    regex e("/bytedance/(\\d+)"); // 正则表达式
    smatch matches;              // 存储匹配结果的

    bool ret = regex_match(str, matches, e);
    if(ret == false)
        return -1;
    
    for(auto& it : matches) 
    {
        cout << it << endl;
    }
    return 0;
}