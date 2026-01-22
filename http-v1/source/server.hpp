#include <iostream>
#include <vector>
#include <sys/types.h>
#include <cassert>
#include <algorithm>
#include <string>
#include <cstring>

#define DEFAULT_BUFFER_SIZE 1024

// 基于双指针的可增长缓冲区
// 读区间：[reader_idx, writer_idx)
class Buffer
{
public:
    Buffer()
        : _buffer(DEFAULT_BUFFER_SIZE)
        , _reader_idx(0)
        , _writer_idx(0)
    {
    }

    // 返回底层缓冲区起始地址
    char *Begin()
    {
        return _buffer.data();
    }

    // 当前写指针位置
    char *WritePos()
    {
        return Begin() + _writer_idx;
    }

    // 当前读指针位置
    char *ReadPos()
    {
        return Begin() + _reader_idx;
    }

    // 尾部剩余可写空间大小
    uint64_t TailIdleSize()
    {
        return _buffer.size() - _writer_idx;
    }

    // 头部已读但尚未复用的空间大小
    uint64_t HeadIdleSize()
    {
        return _reader_idx;
    }

    // 当前可读数据大小
    uint64_t ReadAbleSize()
    {
        return _writer_idx - _reader_idx;
    }

    // 向后移动读指针（消费数据）
    void MoveReadOffset(uint64_t len)
    {
        _reader_idx += len;
    }

    // 向后移动写指针（写入完成后调用）
    void MoveWriteOffset(uint64_t len)
    {
        // 写入空间应已由 EnsureWriteSpace 保证
        assert(len <= TailIdleSize());
        _writer_idx += len;
    }

    // 确保至少有 len 字节可写空间
    // 优先复用头部空间，其次进行扩容
    void EnsureWriteSpace(uint64_t len)
    {
        // 尾部空间足够，直接写
        if (len <= TailIdleSize())
            return;

        // 通过前移可读数据复用空间
        if (len <= TailIdleSize() + HeadIdleSize())
        {
            uint64_t readable = ReadAbleSize();
            std::memmove(Begin(), ReadPos(), readable);
            /* 这里使用memmove而不使用memcpy或copy
             * “memmove 和 memcpy 的区别在于是否支持内存重叠。在我的 Buffer 实现中，
             * 需要在同一块缓冲区内把可读数据整体前移复用空间，这属于典型的重叠拷贝场景，
             * 所以必须使用 memmove，否则行为是未定义的。”
             */
            _reader_idx = 0;
            _writer_idx = readable;
        }
        else
        {
            // 扩容：采用倍增策略，减少频繁 realloc
            uint64_t new_size = _buffer.size();
            uint64_t need_size = _writer_idx + len;

            while (new_size < need_size)
            {
                new_size *= 2;
            }

            _buffer.resize(new_size);
        }
    }

    // 写入任意二进制数据
    void Write(const void *data, uint64_t len)
    {
        // 允许空写
        if (len == 0)
            return;

        // 非空写入必须保证数据指针有效
        assert(data != nullptr);

        EnsureWriteSpace(len);

        const char *d = static_cast<const char *>(data);
        std::copy(d, d + len, WritePos());

        MoveWriteOffset(len);
    }

    // 从缓冲区读取 len 字节到外部缓冲区
    void Read(void *buf, uint64_t len)
    {
        assert(len <= ReadAbleSize());
        std::copy(ReadPos(), ReadPos() + len, static_cast<char *>(buf));
        MoveReadOffset(len);
    }

    // 写入字符串内容（不包含结尾 '\0'）
    void WriteString(const std::string &data)
    {
        Write(data.c_str(), data.size());
    }

    // 将另一个 Buffer 的可读数据复制到当前 Buffer
    // 不影响源 Buffer 状态
    void WriteBuffer(Buffer &data)
    {
        Write(data.ReadPos(), data.ReadAbleSize());
    }

    // 将另一个 Buffer 的可读数据写入并消费
    void WriteBufferAndConsume(Buffer &data)
    {
        uint64_t len = data.ReadAbleSize();
        Write(data.ReadPos(), len);
        data.MoveReadOffset(len);
    }

    // 读取指定长度并以 string 形式返回
    std::string ReadAsString(uint64_t len)
    {
        assert(len <= ReadAbleSize());
        std::string str;
        str.resize(len);
        Read(&str[0], len);
        return str;
    }

    // 查找当前可读区中的 '\n'
    char *FindCRLF()
    {
        void *res = memchr(ReadPos(), '\n', ReadAbleSize());
        return static_cast<char *>(res);
    }

    // 读取一行数据（包含 '\n'）
    std::string GetLine()
    {
        char *pos = FindCRLF();
        if (pos == nullptr)
            return "";
        return ReadAsString(pos - ReadPos() + 1);
    }

    // 清空缓冲区（逻辑清空，不释放内存）
    void Clear()
    {
        _reader_idx = 0;
        _writer_idx = 0;
    }

    ~Buffer() {}

private:
    std::vector<char> _buffer; // 实际存储空间
    uint64_t _reader_idx;      // 读指针
    uint64_t _writer_idx;      // 写指针
};