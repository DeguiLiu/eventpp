# eventpp -- C++ 高性能事件派发和回调列表库

> 基于 [wqking/eventpp](https://github.com/wqking/eventpp) v0.1.3 独立维护，实施了 14 项跨平台性能优化。
> 非常感谢 [marsCatXdu](https://github.com/marsCatXdu) 的中文翻译。

文档

* 核心类和函数库
    * [概述](introduction.md)
    * [CallbackList 教程](tutorial_callbacklist.md)
    * [EventDispatcher 教程](tutorial_eventdispatcher.md)
    * [EventQueue 教程](tutorial_eventqueue.md)
    * [CallbackList 类参考手册](callbacklist.md)
    * [EventDispatcher 类参考手册](eventdispatcher.md)
    * [EventQueue 类参考手册](eventqueue.md)
    * [Policies -- 配置 eventpp](policies.md)
    * [Mixins -- 扩展 eventpp](mixins.md)

### HighPerfPolicy（v0.3.0 推荐）

只需将第三个模板参数设为 `eventpp::HighPerfPolicy`，即可获得 SpinLock + 池化分配器 + 读写锁分离的最优组合：

```cpp
#include <eventpp/eventqueue.h>

struct SensorData {
    uint32_t id;
    float value;
};

// 第三个模板参数传 HighPerfPolicy
eventpp::EventQueue<int, void(const SensorData&), eventpp::HighPerfPolicy> queue;

// 注册监听器
queue.appendListener(1, [](const SensorData& d) {
    printf("Sensor %u: %.1f\n", d.id, d.value);
});

// 入队（线程安全，可从任意线程调用）
queue.enqueue(1, SensorData{42, 25.3f});

// 处理（在消费者线程调用）
queue.process();
```

更多 Policy 配置细节见 [policies](policies.md)。

