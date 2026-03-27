// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//

#include "muduo/net/Buffer.h"

#include "muduo/net/SocketsOps.h"

#include <errno.h>
#include <sys/uio.h>

using namespace muduo;
using namespace muduo::net;

const char Buffer::kCRLF[] = "\r\n";

const size_t Buffer::kCheapPrepend;
const size_t Buffer::kInitialSize;


ssize_t Buffer::readFd(int fd, int* savedErrno)
{
  // saved an ioctl()/FIONREAD call to tell how much to read
  char extrabuf[65536];
  // muduo采用了readv的策略，从内核缓冲区中将数据一次拿到
  // 如果这个buffer中可写空间还很大，就直接讲数据写入buffer
  // 如果buffer空间不多，先把缓冲区写满，再使用在栈上开辟的临时空间extrabuf
  // 最后将临时缓冲区的buffer与buffer合并
  struct iovec vec[2];

  // struct iovec {
  //   void  *iov_base;  // 缓冲区起始地址
  //   size_t iov_len;   // 缓冲区长度
  // };

  const size_t writable = writableBytes();
  // 标记写入地址和可写入大小
  vec[0].iov_base = begin()+writerIndex_;
  vec[0].iov_len = writable;
  vec[1].iov_base = extrabuf;
  vec[1].iov_len = sizeof extrabuf;
  // when there is enough space in this buffer, don't read into extrabuf.
  // when extrabuf is used, we read 128k-1 bytes at most.
  // 如果buffer中空间小于栈的空间，就有两个缓冲区，如果大于则只使用buffer就可以了
  const int iovcnt = (writable < sizeof extrabuf) ? 2 : 1;
  // 一次readv可以将数据从内核读到多个内核缓冲区中
  // 参数是fd，使用的缓冲区列表，缓冲区个数，一次系统调用就可以读到多个数据
  const ssize_t n = sockets::readv(fd, vec, iovcnt);
  // 读产生错误
  if (n < 0)
  {
    *savedErrno = errno;
  }
  // 直接全部在buffer中存
  else if (implicit_cast<size_t>(n) <= writable)
  {
    writerIndex_ += n;
  }
  // 分别在两个缓冲区中存
  else
  {
    // 说明buffer已经写满，更新写指针位置
    writerIndex_ = buffer_.size();
    // 将两个缓冲区合并
    append(extrabuf, n - writable);
  }
  // if (n == writable + sizeof extrabuf)
  // {
  //   goto line_30;
  // }
  return n;
}