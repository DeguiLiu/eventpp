# processQueueWith -- 零开销编译期访问者分发

## 1. 动机

### 1.1 问题分析

eventpp `EventQueue::process()` 的分发热路径经过 5 层间接调用:

```
process()
  for each queued event:
    doDispatchQueuedEvent()            // tuple 解包
      directDispatch(event, args...)   // EventDispatcher 入口
        shared_lock<SharedMutex>       // listenerMutex 读锁
        map.find(event)                // 事件 ID -> CallbackList 查找
        CallbackList::operator()()     // 回调链表调用
          doForEachIf()                // 批量预取遍历 (每 8 节点加锁)
            shared_ptr<Node> traversal // 引用计数开销
            std::function(args...)     // 类型擦除间接调用
```

对于**单消费者**场景 (一个线程消费所有事件，事件处理逻辑编译期已知)，上述基础设施开销完全不必要:
- 不需要 shared_lock -- 只有一个消费者
- 不需要 map.find -- 消费者已知如何处理所有事件
- 不需要 CallbackList -- 不需要动态注册/注销回调
- 不需要 std::function -- 处理函数编译期已知

### 1.2 newosp 验证

newosp 项目的 `ProcessBatchWith<Visitor>` 已验证此优化方向:
- 绕过 FixedFunction 回调表 + SharedSpinLock
- 使用 `std::visit` 编译期跳转表 (C++17)
- 实测 15x 加速 (2 ns/msg vs 30 ns/msg)

## 2. API 设计

### 2.1 processQueueWith

```cpp
template <typename Visitor>
bool processQueueWith(Visitor && visitor);
```

- 处理队列中的**所有**事件
- 每个事件直接调用 `visitor(event, args...)` -- 无间接调用
- 返回 `true` 如果处理了至少一个事件
- Visitor 签名: `void operator()(EventType event, Args... args)`

### 2.2 processOneWith

```cpp
template <typename Visitor>
bool processOneWith(Visitor && visitor);
```

- 处理队列中的**一个**事件
- 返回 `true` 如果处理了一个事件

### 2.3 Visitor 协议

```cpp
// 函数对象 (推荐: 编译器可内联)
struct MyVisitor {
    void operator()(int event, const std::string & data) {
        switch(event) {
            case EVENT_SENSOR: handleSensor(data); break;
            case EVENT_MOTOR:  handleMotor(data);  break;
        }
    }
};

// Lambda
queue.processQueueWith([](int event, const std::string & data) {
    // ...
});
```

Visitor 接收的参数:
- 第一个参数: 事件 ID (EventType)
- 后续参数: 与 EventQueue 原型签名中的 Args... 相同

## 3. 热路径对比

| 开销项 | process() | processQueueWith() |
|--------|:---------:|:-------------------:|
| `shared_lock<SharedMutex>` (listenerMutex) | 每条消息 | **无** |
| `map.find(event)` 查找 | 每条消息 | **无** |
| CallbackList mutex (每 8 节点加锁) | 每批次 | **无** |
| `shared_ptr<Node>` 链表遍历 | 每个回调 | **无** |
| `std::function` 间接调用 | 每个回调 | **无** |
| Mixin beforeDispatch 检查 | 每条消息 | **无** |
| Visitor 直接调用 (可内联) | -- | **每条消息** |

共享的基础设施 (无差异):
- 队列 swap (lock_guard + std::swap) -- 两者相同
- CounterGuard (emptyQueue 语义) -- 两者相同
- BufferedItem clear + freeList 回收 -- 两者相同

## 4. 实现细节

### 4.1 核心实现

```cpp
// eventqueue.h, EventQueueBase 类内

template <typename Visitor>
bool processQueueWith(Visitor && visitor)
{
    if(! queueList.empty()) {
        BufferedItemList tempList;
        CounterGuard<decltype(queueEmptyCounter)> counterGuard(queueEmptyCounter);
        {
            std::lock_guard<Mutex> queueListLock(queueListMutex);
            std::swap(queueList, tempList);
        }
        if(! tempList.empty()) {
            for(auto & item : tempList) {
                doVisitQueuedEvent(
                    visitor,
                    item.get(),
                    typename MakeIndexSequence<sizeof...(Args)>::Type()
                );
                item.clear();
            }
            std::lock_guard<Mutex> queueListLock(freeListMutex);
            freeList.splice(freeList.end(), tempList);
            return true;
        }
    }
    return false;
}

// Helper: tuple 解包 + visitor 直接调用
template <typename V, typename T, size_t ...Indexes>
void doVisitQueuedEvent(V && visitor, T && item, IndexSequence<Indexes...>)
{
    visitor(item.event, std::get<Indexes>(item.arguments)...);
}
```

### 4.2 C++14 兼容性

| 特性 | C++17 (newosp) | C++14 (eventpp) |
|------|:-:|:-:|
| 分发机制 | `std::visit` + `std::variant` | `visitor(event, args...)` 直接调用 |
| 参数展开 | fold expression | IndexSequence + pack expansion |
| 条件编译 | `if constexpr` | SFINAE / `enable_if` |
| 索引序列 | `std::index_sequence` | eventpp 自有 `MakeIndexSequence` |

eventpp 的 EventQueue 是**同构**的 (所有事件共享相同回调签名)，不需要 variant/visit。
Visitor 接收的参数类型在编译期由模板参数确定，天然 C++14 兼容。

### 4.3 与 process() 的关系

- **processQueueWith** 是 `process()` 的**替代品**，不是叠加使用
- 两者消费同一个队列 (queueList)
- 适用于单消费者 (MPSC) 场景
- 如需多消费者或动态注册回调，仍使用 `process()`

## 5. 使用场景

### 5.1 单消费者事件循环

```cpp
eventpp::EventQueue<int, void(int, const SensorData &)> queue;

struct EventHandler {
    void operator()(int event, int id, const SensorData & data) {
        switch(event) {
            case SENSOR_UPDATE: processSensor(id, data); break;
            case MOTOR_CMD:     executeMotor(id, data);  break;
        }
    }
};

// 事件循环 -- 零开销分发
EventHandler handler;
while(running) {
    queue.processQueueWith(handler);
}
```

### 5.2 与 newosp ProcessBatchWith 的对照

| 维度 | newosp | eventpp |
|------|--------|---------|
| 队列 | Lock-free MPSC ring buffer | std::list + swap |
| 类型系统 | std::variant (异构) | 同构 (相同签名) |
| 分发 | std::visit 跳转表 | visitor(event, args...) 直接调用 |
| C++ 标准 | C++17 | C++14 |
| 回调模式 | FixedFunction + callback_table | std::function + CallbackList |
| 绕过的层 | SharedSpinLock + callback遍历 + FixedFunction | SharedMutex + map.find + CallbackList + std::function |

## 6. 测试计划

- processQueueWith 基本分发
- processQueueWith 多事件全量处理
- processQueueWith 空队列返回 false
- processQueueWith 事件顺序保持
- processOneWith 单事件处理
- processOneWith 剩余事件保留
- 自定义 Policy (SingleThreading) 兼容
- processQueueWith vs process 结果一致性
- 非整型事件 ID (std::string)
- 复杂参数 (多参数、移动语义)

**测试结果**: 10/10 全部通过 (220 test cases, 1980 assertions)

## 7. Benchmark 结果

测试环境: Linux x86_64, GCC, Release (-O2), CPU pinned to core 1

| 场景 | process() | processQueueWith() | 加速比 |
|------|:---------:|:-------------------:|:------:|
| 单事件 ID, 100K 消息 | 152.4 ns/msg | 9.1 ns/msg | **16.7x** |
| 10 个事件 ID, 100K 消息 | 151.5 ns/msg | 10.0 ns/msg | **15.2x** |
| 10 个事件 ID, 1M 消息 | 76.6 ns/msg | 21.2 ns/msg | **3.6x** |

分析:
- 100K 消息场景加速比约 15-17x，与 newosp 的 15x 加速一致
- 1M 消息场景加速比降至 3.6x，因为大队列下 std::list 的 freeList 回收成为共同瓶颈
- 中位数 (P50) 更能反映稳态性能: 6.1 ns/msg vs 154.9 ns/msg = 25x
