// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is a public header file, it must only include public header files.

#ifndef MUDUO_NET_BUFFER_H
#define MUDUO_NET_BUFFER_H

#include "muduo/base/copyable.h"
#include "muduo/base/StringPiece.h"
#include "muduo/base/Types.h"

#include "muduo/net/Endian.h"

#include <algorithm>
#include <vector>

#include <assert.h>
#include <string.h>
// #include <unistd.h>  // ssize_t

namespace muduo
{
  namespace net
  {

    /// A buffer class modeled after org.jboss.netty.buffer.ChannelBuffer
    ///
    /// @code
    /// +-------------------+------------------+------------------+
    /// | prependable bytes |  readable bytes  |  writable bytes  |
    /// |                   |     (CONTENT)    |                  |
    /// +-------------------+------------------+------------------+
    /// |                   |                  |                  |
    /// 0      <=      readerIndex   <=   writerIndex    <=     size
    /// @endcode
    class Buffer : public muduo::copyable
    {
    public:
      // 给协议头预留出来的空间
      static const size_t kCheapPrepend = 8;
      // 默认空间大小
      static const size_t kInitialSize = 1024;
      // 初始化空间
      explicit Buffer(size_t initialSize = kInitialSize)
          : buffer_(kCheapPrepend + initialSize),
            readerIndex_(kCheapPrepend),
            writerIndex_(kCheapPrepend)
      {
        assert(readableBytes() == 0);
        assert(writableBytes() == initialSize);
        assert(prependableBytes() == kCheapPrepend);
      }

      // implicit copy-ctor, move-ctor, dtor and assignment are fine
      // NOTE: implicit move-ctor is added in g++ 4.6

      // 交换两个buffer(预测可能是用一个空buffer来换另一个满buffer?)
      void swap(Buffer &rhs)
      {
        buffer_.swap(rhs.buffer_);
        std::swap(readerIndex_, rhs.readerIndex_);
        std::swap(writerIndex_, rhs.writerIndex_);
      }

      // 计算可读空间大小
      size_t readableBytes() const
      {
        return writerIndex_ - readerIndex_;
      }

      // 计算可写空间大小
      size_t writableBytes() const
      {
        return buffer_.size() - writerIndex_;
      }

      // 计算读指针前面的空间(这里不止有预留的协议空间，还有已经被消费的空间，在后续扩容逻辑中需要使用)
      size_t prependableBytes() const
      {
        return readerIndex_;
      }

      // 可读点的迭代器起点
      const char *peek() const
      {
        return begin() + readerIndex_;
      }

      // 从读的起点查找/r/n
      const char *findCRLF() const
      {
        // FIXME: replace with memmem()?
        const char *crlf = std::search(peek(), beginWrite(), kCRLF, kCRLF + 2);
        return crlf == beginWrite() ? NULL : crlf;
      }

      // 传入一个起点，从这个起点查找/r/n
      const char *findCRLF(const char *start) const
      {
        // 保证查找位置在已有数据中
        assert(peek() <= start);
        assert(start <= beginWrite());
        // FIXME: replace with memmem()?
        const char *crlf = std::search(start, beginWrite(), kCRLF, kCRLF + 2);
        return crlf == beginWrite() ? NULL : crlf;
      }

      // 全文搜索\n
      const char *findEOL() const
      {
        const void *eol = memchr(peek(), '\n', readableBytes());
        return static_cast<const char *>(eol);
      }

      // 传入起点到结束查找\n
      const char *findEOL(const char *start) const
      {
        assert(peek() <= start);
        assert(start <= beginWrite());
        const void *eol = memchr(start, '\n', beginWrite() - start);
        return static_cast<const char *>(eol);
      }

      // retrieve returns void, to prevent
      // string str(retrieve(readableBytes()), readableBytes());
      // the evaluation of two functions are unspecified
      // 只移动read_idx来消费数据？
      void retrieve(size_t len)
      {
        assert(len <= readableBytes());
        // 长度小于可读
        if (len < readableBytes())
          readerIndex_ += len;
        // 等于可读直接全部取出(可能是重置?)
        else
          retrieveAll();
      }

      // 移动read_idx直到end
      void retrieveUntil(const char *end)
      {
        assert(peek() <= end);
        assert(end <= beginWrite());
        retrieve(end - peek());
      }

      void retrieveInt64()
      {
        retrieve(sizeof(int64_t));
      }

      void retrieveInt32()
      {
        retrieve(sizeof(int32_t));
      }

      void retrieveInt16()
      {
        retrieve(sizeof(int16_t));
      }

      void retrieveInt8()
      {
        retrieve(sizeof(int8_t));
      }

      // 直接重置读写指针，从头开始覆盖写，而不是删除数据
      void retrieveAll()
      {
        readerIndex_ = kCheapPrepend;
        writerIndex_ = kCheapPrepend;
      }

      // 把所有的数据都以string格式读出来
      string retrieveAllAsString()
      {
        return retrieveAsString(readableBytes());
      }

      // 读长度为len的数据以string格式
      string retrieveAsString(size_t len)
      {
        assert(len <= readableBytes());
        string result(peek(), len);
        retrieve(len);
        return result;
      }

      // StringPiece只保存指向内容的指针，和内容的大小，不拷贝内容
      // 将所有缓冲区中的数据形成一个string_view
      StringPiece toStringPiece() const
      {
        return StringPiece(peek(), static_cast<int>(readableBytes()));
      }

      // 用于buffer收缩
      void append(const StringPiece &str)
      {
        append(str.data(), str.size());
      }

      void append(const char * /*restrict*/ data, size_t len)
      {
        // 先确保写空间足够
        ensureWritableBytes(len);
        // 将数据一次性拷贝
        std::copy(data, data + len, beginWrite());
        // 生产数据，写指针后移
        hasWritten(len);
      }

      void append(const void * /*restrict*/ data, size_t len)
      {
        append(static_cast<const char *>(data), len);
      }

      // 保证写空间足够，如果不够则调用扩容逻辑
      void ensureWritableBytes(size_t len)
      {
        if (writableBytes() < len)
        {
          makeSpace(len);
        }
        assert(writableBytes() >= len);
      }

      // 写位置
      char *beginWrite()
      {
        return begin() + writerIndex_;
      }

      const char *beginWrite() const
      {
        return begin() + writerIndex_;
      }

      // 生产数据，向后移动写指针
      void hasWritten(size_t len)
      {
        assert(len <= writableBytes());
        writerIndex_ += len;
      }

      // 不写了?(和意为)
      void unwrite(size_t len)
      {
        assert(len <= readableBytes());
        writerIndex_ -= len;
      }

      // 添加网络主机序大小的数据
      ///
      /// Append int64_t using network endian
      ///
      void appendInt64(int64_t x)
      {
        int64_t be64 = sockets::hostToNetwork64(x);
        append(&be64, sizeof be64);
      }

      ///
      /// Append int32_t using network endian
      ///
      void appendInt32(int32_t x)
      {
        int32_t be32 = sockets::hostToNetwork32(x);
        append(&be32, sizeof be32);
      }

      void appendInt16(int16_t x)
      {
        int16_t be16 = sockets::hostToNetwork16(x);
        append(&be16, sizeof be16);
      }

      void appendInt8(int8_t x)
      {
        append(&x, sizeof x);
      }

      ///
      /// Read int64_t from network endian
      ///
      /// Require: buf->readableBytes() >= sizeof(int32_t)
      int64_t readInt64()
      {
        int64_t result = peekInt64();
        retrieveInt64();
        return result;
      }

      ///
      /// Read int32_t from network endian
      ///
      /// Require: buf->readableBytes() >= sizeof(int32_t)
      int32_t readInt32()
      {
        int32_t result = peekInt32();
        retrieveInt32();
        return result;
      }

      int16_t readInt16()
      {
        int16_t result = peekInt16();
        retrieveInt16();
        return result;
      }

      int8_t readInt8()
      {
        int8_t result = peekInt8();
        retrieveInt8();
        return result;
      }

      ///
      /// Peek int64_t from network endian
      ///
      /// Require: buf->readableBytes() >= sizeof(int64_t)
      int64_t peekInt64() const
      {
        assert(readableBytes() >= sizeof(int64_t));
        int64_t be64 = 0;
        ::memcpy(&be64, peek(), sizeof be64);
        return sockets::networkToHost64(be64);
      }

      ///
      /// Peek int32_t from network endian
      ///
      /// Require: buf->readableBytes() >= sizeof(int32_t)
      int32_t peekInt32() const
      {
        assert(readableBytes() >= sizeof(int32_t));
        int32_t be32 = 0;
        ::memcpy(&be32, peek(), sizeof be32);
        return sockets::networkToHost32(be32);
      }

      int16_t peekInt16() const
      {
        assert(readableBytes() >= sizeof(int16_t));
        int16_t be16 = 0;
        ::memcpy(&be16, peek(), sizeof be16);
        return sockets::networkToHost16(be16);
      }

      int8_t peekInt8() const
      {
        assert(readableBytes() >= sizeof(int8_t));
        int8_t x = *peek();
        return x;
      }

      ///
      /// Prepend int64_t using network endian
      ///
      void prependInt64(int64_t x)
      {
        int64_t be64 = sockets::hostToNetwork64(x);
        prepend(&be64, sizeof be64);
      }

      ///
      /// Prepend int32_t using network endian
      ///
      void prependInt32(int32_t x)
      {
        int32_t be32 = sockets::hostToNetwork32(x);
        prepend(&be32, sizeof be32);
      }

      void prependInt16(int16_t x)
      {
        int16_t be16 = sockets::hostToNetwork16(x);
        prepend(&be16, sizeof be16);
      }

      void prependInt8(int8_t x)
      {
        prepend(&x, sizeof x);
      }

      // 在头部预留空间添加协议头，优化性能?
      void prepend(const void * /*restrict*/ data, size_t len)
      {
        assert(len <= prependableBytes());
        readerIndex_ -= len;
        const char *d = static_cast<const char *>(data);
        std::copy(d, d + len, begin() + readerIndex_);
      }

      // 当muduo的buffer经过峰值大包长连接经历“大包峰值”后
      // muduo 可以主动收缩缓冲区大小，降低常驻内存（提升整体机器吞吐稳定性）
      // 我的缓冲区峰值后可能长期占用大块内存，连接数一上来会吃内存带宽和缓存命中。
      
      void shrink(size_t reserve)
      {
        // FIXME: use vector::shrink_to_fit() in C++ 11 if possible.
        // muduo使用一个新的缓冲区来把旧缓冲区进行swap
        Buffer other;
        other.ensureWritableBytes(readableBytes() + reserve);
        other.append(toStringPiece());
        swap(other);
      }

      // vector的空间大小
      size_t internalCapacity() const
      {
        return buffer_.capacity();
      }

      /// Read data directly into buffer.
      ///
      /// It may implement with readv(2)
      /// @return result of read(2), @c errno is saved
      ssize_t readFd(int fd, int *savedErrno);

    private:
      // 相当于自己的一个头部迭代器?
      char *begin()
      {
        return &*buffer_.begin();
      }

      const char *begin() const
      {
        return &*buffer_.begin();
      }

      // 这个是扩容逻辑？
      void makeSpace(size_t len)
      {
        // 计算写 + 前序空间加起来够不够新写入的长度 + 预留协议空间
        if (writableBytes() + prependableBytes() < len + kCheapPrepend)
        {
          // FIXME: move readable data
          // 不够的话就扩容
          buffer_.resize(writerIndex_ + len);
        }
        else
        {
          // move readable data to the front, make space inside buffer
          assert(kCheapPrepend < readerIndex_);
          // 计算
          size_t readable = readableBytes();
          // 将现有的空间copy到起始read_idx
          std::copy(begin() + readerIndex_,
                    begin() + writerIndex_,
                    begin() + kCheapPrepend);
          // 重新计算指针位置
          readerIndex_ = kCheapPrepend;
          writerIndex_ = readerIndex_ + readable;
          assert(readable == readableBytes());
        }
      }

    private:
      // 数组连续空间，避免链表降低缓存命中率
      std::vector<char> buffer_;
      // 两个指针将一个buffer分成三部分
      size_t readerIndex_;
      size_t writerIndex_;

      static const char kCRLF[];
    };

  } // namespace net
} // namespace muduo

#endif // MUDUO_NET_BUFFER_H
