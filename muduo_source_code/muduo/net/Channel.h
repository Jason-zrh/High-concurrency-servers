// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is an internal header file, you should not include this.

#ifndef MUDUO_NET_CHANNEL_H
#define MUDUO_NET_CHANNEL_H

#include "muduo/base/noncopyable.h"
#include "muduo/base/Timestamp.h"

#include <functional>
#include <memory>

namespace muduo
{
namespace net
{

class EventLoop;

///
/// A selectable I/O channel.
///
/// This class doesn't own the file descriptor.
/// The file descriptor could be a socket,
/// an eventfd, a timerfd, or a signalfd
class Channel : noncopyable
{
 public:
  // 其他事件回调
  typedef std::function<void()> EventCallback;
  // 读事件回调
  // 读事件需要“到达时刻”
  // 在 handleEventWithGuard() 触发读回调时，直接把 receiveTime 传下去。
  // 上层（如 TcpConnection）可以基于这个时间做延迟统计、超时控制、追踪 pipeline 抖动。
  typedef std::function<void(Timestamp)> ReadEventCallback;

  Channel(EventLoop* loop, int fd);
  ~Channel();

  // 处理就绪事件
  void handleEvent(Timestamp receiveTime);

  void setReadCallback(ReadEventCallback cb)
  { readCallback_ = std::move(cb); }
  void setWriteCallback(EventCallback cb)
  { writeCallback_ = std::move(cb); }
  void setCloseCallback(EventCallback cb)
  { closeCallback_ = std::move(cb); }
  void setErrorCallback(EventCallback cb)
  { errorCallback_ = std::move(cb); }

  /// Tie this channel to the owner object managed by shared_ptr,
  /// prevent the owner object being destroyed in handleEvent.
  // 将channel与上层生命周期绑定，防止在handleEvent中上层对象被销毁后续仍然收到回调
  void tie(const std::shared_ptr<void>&);

  int fd() const { return fd_; }
  // 返回该文件描述符关心的事件
  int events() const { return events_; }
  // 设置已就绪的关心事件
  void set_revents(int revt) { revents_ = revt; } // used by pollers
  // int revents() const { return revents_; }
  // 判断该fd有没有关心事件了
  bool isNoneEvent() const { return events_ == kNoneEvent; }
  // 设置关心事件
  void enableReading() { events_ |= kReadEvent; update(); }
  void disableReading() { events_ &= ~kReadEvent; update(); }
  void enableWriting() { events_ |= kWriteEvent; update(); }
  void disableWriting() { events_ &= ~kWriteEvent; update(); }
  void disableAll() { events_ = kNoneEvent; update(); }
  // 判断是否关心某些事件
  bool isWriting() const { return events_ & kWriteEvent; }
  bool isReading() const { return events_ & kReadEvent; }


  // ================== 分界线 ==================
  // for Poller
  int index() { return index_; } // 用来标记epoll中事件描述符的状态
  void set_index(int idx) { index_ = idx; } // NEW（未注册）ADDED（已注册）DELETED（已删除）

  // for debug
  // string reventsToString() const;
  // string eventsToString() const;

  void doNotLogHup() { logHup_ = false; }

  EventLoop* ownerLoop() { return loop_; }
  void remove();

 private:
  static string eventsToString(int fd, int ev);

  void update();
  void handleEventWithGuard(Timestamp receiveTime);

  static const int kNoneEvent; // 0
  static const int kReadEvent; // POLLIN | POLLPRI
  static const int kWriteEvent;// POLLOUT

  EventLoop* loop_;   // 该文件描述符绑定的loop
  const int  fd_;     // fd
  int        events_; // 关心的事件
  int        revents_; // it's the received event types of epoll or poll
  int        index_; // used by Poller. // 
  // 用于关闭日志打印
  bool       logHup_;

  // 下面这三个参数用来确保安全性，感觉有点类似于状态机？
  std::weak_ptr<void> tie_;
  bool tied_;           // 标记是否绑定上层生命周期对象
  bool eventHandling_;  // 标记是否在处理事件，防止在channel执行回调的时候被删除
  bool addedToLoop_;    // 标记是否被添加到循环中，防止重复注册和移除没注册的channel

  ReadEventCallback readCallback_;
  EventCallback writeCallback_;
  EventCallback closeCallback_;
  EventCallback errorCallback_;
};

}  // namespace net
}  // namespace muduo

#endif  // MUDUO_NET_CHANNEL_H
