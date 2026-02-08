#include "../server.hpp"
#include "fstream"

// ====================================================================================================
//                                               Util工具模块
// ====================================================================================================
class Util
{
public:
    // 字符串分割 - 返回子串的数量
    size_t Split(const std::string &src, const std::string &sep, std::vector<std::string> *array)
    {
        int offset = 0; // 偏移量
        while (offset < src.size())
        {
            size_t pos = src.find(sep, offset); // 从偏移量后面开始查找字符
            if (pos == std::string::npos)
            { 
                if(pos == src.size())
                    break;
                // 没有找到特定字符
                array->push_back(src.substr(offset));
                return array->size();
            }
            // 当前子串为空
            if(pos == offset)
            {   
                offset = pos + sep.size();
                continue;
            }
           
            array->push_back(src.substr(offset, pos - offset));
            offset = pos + sep.size();
        }
        return array->size();
    }

    // 读取文件所有内容, 将读取的内容放到一个buffer中
    static bool ReadFile(const std::string& filename, std::string* buf)
    {
        std::ifstream ifs(filename, std::ios::binary);
        if(ifs.is_open() == false)
        {
            ERR_LOG("Open File failed");
            return false;
        }
        // 读取所有数据
        size_t fsize = 0;
        ifs.seekg(0, ifs.end);
        fsize = ifs.tellg();
        ifs.seekg(0, ifs.beg);

        buf->resize(fsize);
        ifs.read(&((*buf)[0]), fsize);
        if(ifs.good() == false)
        {
            ERR_LOG("Read %s failed", filename.c_str());
            ifs.close();
            return false;
        }
        ifs.close();
        return true;
    }

    // 写文件
    static bool WriteFile()
    {
    }

    // 编码url
    static bool UrlEncode()
    {
    }

    // 解码url
    static bool UrlDecode()
    {
    }

    // 响应状态码描述信息获取
    static std::string StatuDescribe()
    {
    }

    // 根据文件后缀名获取mine
    static std::string ExMime()
    {
    }

    // 判断文件是否是一个目录
    static bool IsDirection()
    {
    }

    // 判断是否是普通文件
    static bool IsRegular()
    {
    }

    // 请求资源是否有效
    static bool IsValid()
    {
    }
};