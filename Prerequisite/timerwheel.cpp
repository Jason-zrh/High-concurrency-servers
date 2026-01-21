/*
    时间轮思想
    利用定时器，我们可以看一下每间隔几秒就检查一下链接状况，将不活跃的链接直接断开
    但是如果有成千上万的链接，那每一次都要遍历一次消耗是巨大的，所以我们提出了时间轮的思想

    维护一个数组和一个指针tick，每一秒tick向后移动一次，走到哪里就代表哪里任务应该被执行了
    如果同一时间有多个需要被同时执行的任务则使用下拉数组完成


    如果要时间到了以后自动执行任务，可以将该任务放到一个类的析构函数中，当时间到了以后对象被销毁析构函数自动被执行
    同时如果这个链接在规定的秒数中有活跃操作，则应该刷新它的销毁时间，所以这里可以用shared_ptr,如果
    这个链接在第10s时产生通信则在(10 + 30s)的地方再创建一个shared_ptr对象,这样可以使它内部的引用计数+1
*/


#include <iostream>
#include <memory>
#include <vector>
#include <unordered_map>
#include <unistd.h>
#include <sys/types.h>
#include <functional>

// ==========================================================
// 定时任务对象
// ==========================================================

// 定时任务真正要执行的回调函数类型
using TaskFunc = std::function<void()>;

// 定时任务释放时调用的回调（用于从时间轮中移除索引）
using RealseFunc = std::function<void()>;

/*
 * TimerTask 表示一个“定时任务实体”
 * - 生命周期由 shared_ptr 管理
 * - 当最后一个 shared_ptr 被释放时触发析构
 * - 析构中根据是否被取消决定是否执行任务回调
 */
class TimerTask
{
public:
    /*
     * @id       : 定时任务唯一标识
     * @timeout  : 超时时间（秒）
     * @cb       : 定时任务到期后要执行的回调函数
     */
    TimerTask(uint64_t id, uint32_t timeout, const TaskFunc &cb)
        : _id(id),
          _timeout(timeout),
          _task_cb(cb),
          _isCanncel(false)   // 默认任务未被取消
    { }

    /*
     * 析构函数：
     * - 时间轮中保存该任务的 shared_ptr 被释放时触发
     * - 如果任务未被取消，则执行任务回调
     * - 无论是否取消，都会调用释放回调清理时间轮中的索引
     */
    ~TimerTask()
    {
        // 如果被取消了就不执行任务回调
        if(!_isCanncel)
            _task_cb();

        // 通知时间轮移除该任务对应的 weak_ptr 索引
        _release_cb();
    }

    /*
     * 设置释放回调
     * 该回调由 TimerWheel 提供，用于在任务销毁时
     * 从 _timers 哈希表中移除对应条目
     */
    void SetRealse(const RealseFunc &cb)
    {
        _release_cb = cb;
    }

    // 返回任务的超时时间（用于刷新任务）
    uint32_t DelayTime()
    {
        return _timeout;
    }

    /*
     * 取消定时任务
     * - 并不会立刻删除任务
     * - 只是标记状态，在析构时不再执行任务回调
     */
    void Canncel()
    {
        _isCanncel = true;
    }

private:
    uint64_t _id;           // 定时器任务唯一 ID
    uint32_t _timeout;      // 定时任务超时时间（秒）
    TaskFunc _task_cb;      // 定时任务到期要执行的回调
    RealseFunc _release_cb; // 释放回调：用于清理时间轮索引
    bool _isCanncel;        // 是否被取消的标志位
};

// ==========================================================
// 时间轮
// ==========================================================

// shared_ptr：真正拥有 TimerTask 对象生命周期
using PtrTask = std::shared_ptr<TimerTask>;

// weak_ptr：仅用于索引，不参与生命周期管理
using WeakTask = std::weak_ptr<TimerTask>;

/*
 * TimerWheel：时间轮定时器
 *
 * 核心设计思想：
 * 1. 时间轮槽位（_wheel）使用 shared_ptr 管理任务生命周期
 * 2. _timers 使用 weak_ptr 保存任务索引，避免循环引用
 * 3. tick 每推进一次，就清理当前槽位，触发任务析构
 */
class TimerWheel
{
public:
    TimerWheel()
        : _capacity(60),       // 时间轮大小（60 秒一圈）
          _tick(0),            // 当前指针位置
          _wheel(_capacity)    // 初始化时间轮槽位
    { }

    /*
     * 添加定时任务
     * - 创建 TimerTask 对象（shared_ptr）
     * - 设置释放回调，用于任务析构时移除索引
     * - 将任务放入未来 timeout 秒对应的槽位
     * - 用 weak_ptr 保存任务索引
     */
    void TimerAdd(uint64_t id, uint32_t timeout, const TaskFunc &cb)
    {
        // 创建定时任务对象（生命周期由时间轮槽位管理）
        PtrTask pt(new TimerTask(id, timeout, cb));

        // 设置任务析构时的回调，用于从 _timers 中移除
        pt->SetRealse(std::bind(&TimerWheel::RemoveTimer, this, id));

        // 将任务放入未来 timeout 秒对应的槽位
        _wheel[(_tick + timeout) % _capacity].push_back(pt);

        // 用 weak_ptr 保存任务索引（不影响生命周期）
        _timers[id] = WeakTask(pt);
    }

    /*
     * 延迟（刷新）定时任务
     * - 通过 weak_ptr 获取任务对象
     * - 如果任务仍然存在，将其重新放入未来的槽位
     */
    void TimerRefresh(uint64_t id)
    {
        auto it = _timers.find(id);
        if(it == _timers.end())
            return;

        // 通过 weak_ptr 安全地获取 shared_ptr
        PtrTask pt = it->second.lock();
        if (!pt)
            return;

        // 将任务重新加入未来 DelayTime 秒后的槽位
        _wheel[(_tick + pt->DelayTime()) % _capacity].push_back(pt);
    }

    /*
     * 取消定时任务
     * - 通过 weak_ptr 获取任务对象
     * - 设置取消标志位
     * - 任务仍会在到期时析构，但不会执行任务回调
     */
    void TimerCannel(uint64_t id)
    {
        auto it = _timers.find(id);
        if(it == _timers.end())
            return;

        PtrTask pt = it->second.lock();
        if(pt)
            pt->Canncel();
    }

    /*
     * 时间轮推进（每秒调用一次）
     * - tick 前进一格
     * - 清空当前槽位
     * - 槽位中的 shared_ptr 被释放，触发 TimerTask 析构
     */
    void Run()
    {
        _tick = (_tick + 1) % _capacity;

        // 清空当前槽位，触发定时任务析构
        _wheel[_tick].clear();
    }

private:
    /*
     * 移除定时任务索引
     * - 由 TimerTask 析构时调用
     * - 清理 _timers 中对应的 weak_ptr
     */
    void RemoveTimer(uint64_t id)
    {
        auto it = _timers.find(id);
        if(it != _timers.end())
            _timers.erase(it);
    }

private:
    // 时间轮槽位，每个槽位保存多个定时任务（shared_ptr）
    std::vector<std::vector<PtrTask>> _wheel;

    // 任务索引表：id -> weak_ptr，不参与生命周期管理
    std::unordered_map<uint64_t, WeakTask> _timers;

    int _tick;       // 当前时间指针（秒针）
    int _capacity;   // 时间轮容量
};