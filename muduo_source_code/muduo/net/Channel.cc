// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include "muduo/base/Logging.h"
#include "muduo/net/Channel.h"
#include "muduo/net/EventLoop.h"

#include <sstream>

#include <poll.h>

using namespace muduo;
using namespace muduo::net;

const int Channel::kNoneEvent = 0;
// 读写事件
const int Channel::kReadEvent = POLLIN | POLLPRI;
const int Channel::kWriteEvent = POLLOUT;

Channel::Channel(EventLoop* loop, int fd__)
  : loop_(loop),
    fd_(fd__),
    events_(0),
    revents_(0),
    index_(-1),
    logHup_(true),
    tied_(false),
    eventHandling_(false),
    addedToLoop_(false)
{
}

Channel::~Channel()
{
  assert(!eventHandling_);
  assert(!addedToLoop_);
  if (loop_->isInLoopThread())
  {
    assert(!loop_->hasChannel(this));
  }
}

void Channel::tie(const std::shared_ptr<void>& obj)
{
  tie_ = obj;
  tied_ = true;
}

void Channel::update()
{
  addedToLoop_ = true;
  loop_->updateChannel(this);
}

void Channel::remove()
{
  // 先断言是不是没有关心事件了，必须要没有关心事件才关闭
  assert(isNoneEvent());
  addedToLoop_ = false;
  loop_->removeChannel(this);
}

void Channel::handleEvent(Timestamp receiveTime)
{
  std::shared_ptr<void> guard;
  // 如果绑定了上层生命周期就要先判断
  if (tied_)
  {
    guard = tie_.lock();
    if (guard)
    {
      // 如果上层还存活就执行回调
      handleEventWithGuard(receiveTime);
    }
    // 不存活，不执行回调防止悬空
  }
  else
  {
    // 没绑定只直接执行了
    handleEventWithGuard(receiveTime);
  }
}

void Channel::handleEventWithGuard(Timestamp receiveTime)
{
  eventHandling_ = true;
  LOG_TRACE << reventsToString();
  // 挂断链接
  if ((revents_ & POLLHUP) && !(revents_ & POLLIN))
  {
    if (logHup_)
    {
      LOG_WARN << "fd = " << fd_ << " Channel::handle_event() POLLHUP";
    }
    if (closeCallback_) closeCallback_();
  }

  if (revents_ & POLLNVAL)
  {
    LOG_WARN << "fd = " << fd_ << " Channel::handle_event() POLLNVAL";
  }
  // 错误
  if (revents_ & (POLLERR | POLLNVAL))
  {
    if (errorCallback_) errorCallback_();
  }
  // 读回调
  if (revents_ & (POLLIN | POLLPRI | POLLRDHUP))
  {
    if (readCallback_) readCallback_(receiveTime);
  }
  // 写回调
  if (revents_ & POLLOUT)
  {
    if (writeCallback_) writeCallback_();
  }
  eventHandling_ = false;
}

// string Channel::reventsToString() const
// {
//   return eventsToString(fd_, revents_);
// }

// string Channel::eventsToString() const
// {
//   return eventsToString(fd_, events_);
// }

// string Channel::eventsToString(int fd, int ev)
// {
//   std::ostringstream oss;
//   oss << fd << ": ";
//   if (ev & POLLIN)
//     oss << "IN ";
//   if (ev & POLLPRI)
//     oss << "PRI ";
//   if (ev & POLLOUT)
//     oss << "OUT ";
//   if (ev & POLLHUP)
//     oss << "HUP ";
//   if (ev & POLLRDHUP)
//     oss << "RDHUP ";
//   if (ev & POLLERR)
//     oss << "ERR ";
//   if (ev & POLLNVAL)
//     oss << "NVAL ";

//   return oss.str();
// }