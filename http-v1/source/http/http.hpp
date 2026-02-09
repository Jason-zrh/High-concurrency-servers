#include "../server.hpp"
#include <fstream>
#include <regex> // 正则表达式

#include <sys/stat.h>

// ====================================================================================================
//                                               Util工具模块
// ====================================================================================================
class Util
{
public:
    // 字符串分割 - 返回子串的数量
    static size_t Split(const std::string &src, const std::string &sep, std::vector<std::string> *array)
    {
        int offset = 0; // 偏移量
        while (offset < src.size())
        {
            size_t pos = src.find(sep, offset); // 从偏移量后面开始查找字符
            if (pos == std::string::npos)
            {
                if (pos == src.size())
                    break;
                // 没有找到特定字符
                array->push_back(src.substr(offset));
                return array->size();
            }
            // 当前子串为空
            if (pos == offset)
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
    static bool ReadFile(const std::string &filename, std::string *buf)
    {
        std::ifstream ifs(filename, std::ios::binary);
        if (ifs.is_open() == false)
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
        if (ifs.good() == false)
        {
            ERR_LOG("Read %s failed", filename.c_str());
            ifs.close();
            return false;
        }
        ifs.close();
        return true;
    }

    // 写文件
    static bool WriteFile(const std::string &filename, const std::string &buf)
    {
        // 丢弃原有内容，直接写入新内容
        std::ofstream ofs(filename, std::ios::binary | std::ios::trunc);
        if (ofs.is_open() == false)
        {
            ERR_LOG("Open File failed");
            return false;
        }
        ofs.write(buf.c_str(), buf.size());
        if (ofs.good() == false)
        {
            ERR_LOG("Writr %s failed", filename.c_str());
            ofs.close();
            return false;
        }
        ofs.close();
        return true;
    }

    // 编码url - 避免url中资源路径与查询字符串中的特殊字符与http请求中特殊字符产生歧义
    // 不编码的字符: RFC3986 . - _ ~ 字母，数字属于绝对不编码数据
    static std::string UrlEncode(const std::string &url, bool convert_space_to_plus)
    {
        std::string res;
        for (auto &c : url)
        {
            if (c == ',' || c == '-' || c == '_' || c == '~' || isalpha(c) || isdigit(c))
            {
                res += c;
                continue;
            }
            if (c == ' ' && convert_space_to_plus)
            {
                res += '+';
                continue;
            }
            // 剩余字符都需要转换为%HH格式
            char tmp[4] = {0};
            snprintf(tmp, 4, "%%%02X", c);
            res += tmp;
        }
        return res;
    }

    static char HexToDecimal(char c)
    {
        if (isdigit(c))
            return c - '0';
        else if (islower(c))
            return c - 'a' + 10;
        else if (isupper(c))
            return c - 'A' + 10;
        else
            return -1;
    }

    // 解码url
    static std::string UrlDecode(const std::string &url, bool convert_plus_to_space)
    {
        std::string res;
        // 遇到百分号，则将紧跟其后的两个数字转化为字符
        for (int i = 0; i < url.size(); i++)
        {
            if (url[i] == '+' && convert_plus_to_space)
            {
                res += ' ';
                continue;
            }
            if (url[i] == '%' && i + 2 < url.size())
            {
                char ch1 = HexToDecimal(url[i + 1]);
                char ch2 = HexToDecimal(url[i + 2]);
                char ch = (ch1 << 4) + ch2;
                res += ch;
                i += 2;
                continue;
            }
        }
        return res;
    }

    // 响应状态码描述信息获取
    static std::string StatuDescribe(int statu)
    {
        std::unordered_map<int, std::string> statuMap =
            {
                {100, "Continue"},
                {101, "Switching Protocol"},
                {102, "Processing"},
                {103, "Early Hints"},
                {200, "OK"},
                {201, "Created"},
                {202, "Accepted"},
                {203, "Non-Authoritative Information"},
                {204, "No Content"},
                {205, "Reset Content"},
                {206, "Partial Content"},
                {207, "Multi-Status"},
                {208, "Already Reported"},
                {226, "IM Used"},
                {300, "Multiple Choice"},
                {301, "Moved Permanently"},
                {302, "Found"},
                {303, "See Other"},
                {304, "Not Modified"},
                {305, "Use Proxy"},
                {306, "unused"},
                {307, "Temporary Redirect"},
                {308, "Permanent Redirect"},
                {400, "Bad Request"},
                {401, "Unauthorized"},
                {402, "Payment Required"},
                {403, "Forbidden"},
                {404, "Not Found"},
                {405, "Method Not Allowed"},
                {406, "Not Acceptable"},
                {407, "Proxy Authentication Required"},
                {408, "Request Timeout"},
                {409, "Conflict"},
                {410, "Gone"},
                {411, "Length Required"},
                {412, "Precondition Failed"},
                {413, "Payload Too Large"},
                {414, "URI Too Long"},
                {415, "Unsupported Media Type"},
                {416, "Range Not Satisfiable"},
                {417, "Expectation Failed"},
                {418, "I'm a teapot"},
                {421, "Misdirected Request"},
                {422, "Unprocessable Entity"},
                {423, "Locked"},
                {424, "Failed Dependency"},
                {425, "Too Early"},
                {426, "Upgrade Required"},
                {428, "Precondition Required"},
                {429, "Too Many Requests"},
                {431, "Request Header Fields Too Large"},
                {451, "Unavailable For Legal Reasons"},
                {501, "Not Implemented"},
                {502, "Bad Gateway"},
                {503, "Service Unavailable"},
                {504, "Gateway Timeout"},
                {505, "HTTP Version Not Supported"},
                {506, "Variant Also Negotiates"},
                {507, "Insufficient Storage"},
                {508, "Loop Detected"},
                {510, "Not Extended"},
                {511, "Network Authentication Required"}};

        auto it = statuMap.find(statu);
        if (it != statuMap.end())
            return it->second;
        else
            return "Unknow";
    }

    // 根据文件后缀名获取mine
    static std::string ExMime(const std::string &filename)
    {
        std::unordered_map<std::string, std::string> suffixMap =
            {
                {".aac", "audio/aac"},
                {".abw", "application/x-abiword"},
                {".arc", "application/x-freearc"},
                {".avi", "video/x-msvideo"},
                {".azw", "application/vnd.amazon.ebook"},
                {".bin", "application/octet-stream"},
                {".bmp", "image/bmp"},
                {".bz", "application/x-bzip"},
                {".bz2", "application/x-bzip2"},
                {".csh", "application/x-csh"},
                {".css", "text/css"},
                {".csv", "text/csv"},
                {".doc", "application/msword"},
                {".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
                {".eot", "application/vnd.ms-fontobject"},
                {".epub", "application/epub+zip"},
                {".gif", "image/gif"},
                {".htm", "text/html"},
                {".html", "text/html"},
                {".ico", "image/vnd.microsoft.icon"},
                {".ics", "text/calendar"},
                {".jar", "application/java-archive"},
                {".jpeg", "image/jpeg"},
                {".jpg", "image/jpeg"},
                {".js", "text/javascript"},
                {".json", "application/json"},
                {".jsonld", "application/ld+json"},
                {".mid", "audio/midi"},
                {".midi", "audio/x-midi"},
                {".mjs", "text/javascript"},
                {".mp3", "audio/mpeg"},
                {".mpeg", "video/mpeg"},
                {".mpkg", "application/vnd.apple.installer+xml"},
                {".odp", "application/vnd.oasis.opendocument.presentation"},
                {".ods", "application/vnd.oasis.opendocument.spreadsheet"},
                {".odt", "application/vnd.oasis.opendocument.text"},
                {".oga", "audio/ogg"},
                {".ogv", "video/ogg"},
                {".ogx", "application/ogg"},
                {".otf", "font/otf"},
                {".png", "image/png"},
                {".pdf", "application/pdf"},
                {".ppt", "application/vnd.ms-powerpoint"},
                {".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
                {".rar", "application/x-rar-compressed"},
                {".rtf", "application/rtf"},
                {".sh", "application/x-sh"},
                {".svg", "image/svg+xml"},
                {".swf", "application/x-shockwave-flash"},
                {".tar", "application/x-tar"},
                {".tif", "image/tiff"},
                {".tiff", "image/tiff"},
                {".ttf", "font/ttf"},
                {".txt", "text/plain"},
                {".vsd", "application/vnd.visio"},
                {".wav", "audio/wav"},
                {".weba", "audio/webm"},
                {".webm", "video/webm"},
                {".webp", "image/webp"},
                {".woff", "font/woff"},
                {".woff2", "font/woff2"},
                {".xhtml", "application/xhtml+xml"},
                {".xls", "application/vnd.ms-excel"},
                {".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
                {".xml", "application/xml"},
                {".xul", "application/vnd.mozilla.xul+xml"},
                {".zip", "application/zip"},
                {".3gp", "video/3gpp"},
                {".3g2", "video/3gpp2"},
                {".7z", "application/x-7z-compressed"}};

        // 先获取文件扩展名，再获取mime
        size_t pos = filename.find_last_of('.');
        if (pos == std::string::npos)
            return "application/octet-stream";

        std::string exp = filename.substr(pos);
        auto it = suffixMap.find(exp);
        if (it == suffixMap.end())
            return "application/octet-stream";

        return it->second;
    }

    // 判断文件是否是一个目录
    static bool IsDirection(const std::string &filename)
    {
        struct stat st;
        int ret = stat(filename.c_str(), &st);
        if (ret < 0)
            return false;
        return S_ISDIR(st.st_mode);
    }

    // 判断是否是普通文件
    static bool IsRegular(const std::string &filename)
    {
        struct stat st;
        int ret = stat(filename.c_str(), &st);
        if (ret < 0)
            return false;
        return S_ISREG(st.st_mode);
    }

    // 请求资源是否有效
    static bool IsValid(const std::string &path)
    {
        // 按照/进行目录分割，计算有多少子目录,深度不能小于0
        int level = 0;
        std::vector<std::string> subdir;
        Split(path, "/", &subdir);

        for (auto &dir : subdir)
        {
            if (dir == "..")
            {
                level--;
                if (level < 0)
                    return false;
                continue;
            }

            level++;
        }
        return true;
    }
};

// ====================================================================================================
//                                             HttpRequest模块
// ====================================================================================================