# eventpp — 高性能 C++ 事件库

> 基于 [wqking/eventpp](https://github.com/wqking/eventpp) v0.1.3 独立维护，完全兼容原版 API。

eventpp 提供三大核心组件：回调列表（CallbackList）、事件分发器（EventDispatcher）和异步事件队列（EventQueue），支持信号/槽、发布/订阅、观察者等常见模式。

![C++](https://img.shields.io/badge/C%2B%2B-14-blue)
![License](https://img.shields.io/badge/License-Apache--2.0-blue)
![Platform](https://img.shields.io/badge/Platform-ARM%20%7C%20x86-green)
![Version](https://img.shields.io/badge/Version-v0.4.0-orange)

## 相比 v0.1.3 解决的问题

原版 eventpp v0.1.3 在高并发和嵌入式实时场景下存在以下问题，本维护版本逐一解决：

| 问题 | 影响 | 解决方案 | 收益 |
|------|------|----------|------|
| SpinLock 空转无 CPU hint | ARM/x86 自旋时功耗高、缓存行频繁 bouncing | 平台感知的 `YIELD`/`PAUSE` 指令 + 指数退避 | 降低自旋功耗，减少高竞争下缓存行抖动 |
| CallbackList 每次回调都加锁 | 监听器多时，多线程并发调用个别请求延迟飙高 | 批量预取（每 8 节点加锁一次） | 尾部延迟大幅降低 |
| EventDispatcher 读写互斥 | 多线程 dispatch 相互阻塞 | `shared_mutex` 读写分离 | 多线程 dispatch 不互斥 |
| EventQueue 入队锁竞争 | 多生产者 enqueue 串行化 | `try_lock` 非阻塞 freeList + lock-free CAS 分配 | 消除入队锁竞争 |
| 每次 enqueue 都 malloc | 堆分配抖动，延迟不可预测 | PoolAllocator 池化分配器 | 小批量吞吐 +33% |
| 大批量事件 Pool 耗尽回退 malloc | 100K+ 事件时 Pool 优势消失 | 多级 slab 链式扩展（借鉴 iceoryx MemPool） | 100K 吞吐 +26%，1M 吞吐 +45% |
| Cache-line 大小硬编码 64B | Apple Silicon 128B prefetch 下 false sharing | 平台自动检测（Apple Silicon 128B / 其他 64B） | 正确的 false sharing 防御 |
| 内存序全部 `seq_cst` | 不必要的屏障指令开销 | 精确使用 `acq_rel` 语义 | 减少屏障指令 |
| waitFor 直接 futex | 短等待场景系统调用开销大 | 自适应 spin-then-futex | 减少系统调用 |
| 高性能配置需手动组合多个 Policy | 用户需了解 SpinLock、PoolQueueList、GeneralThreading 才能配置 | `HighPerfPolicy` 一站式预设 | 零配置获得最优组合 |
| process() 分发链开销大 | 单消费者场景下 shared_lock + map.find + CallbackList + std::function 开销不必要 | `processQueueWith<Visitor>` 零开销编译期分发 | 单消费者场景 **15x 加速** |

## 快速开始

### CMake 集成

```cmake
include(FetchContent)
FetchContent_Declare(
    eventpp
    GIT_REPOSITORY https://gitee.com/liudegui/eventpp.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(eventpp)
target_include_directories(your_target PRIVATE ${eventpp_SOURCE_DIR}/include)
```

### 基本用法

```cpp
#include <eventpp/callbacklist.h>
#include <eventpp/eventdispatcher.h>
#include <eventpp/eventqueue.h>

// 回调列表
eventpp::CallbackList<void(const std::string &, bool)> callbackList;
callbackList.append([](const std::string & s, bool b) {
    std::cout << "s=" << s << " b=" << b << std::endl;
});
callbackList("Hello", true);

// 事件分发器
eventpp::EventDispatcher<int, void()> dispatcher;
dispatcher.appendListener(3, []() { std::cout << "Event 3" << std::endl; });
dispatcher.dispatch(3);

// 异步事件队列
eventpp::EventQueue<int, void(const std::string &)> queue;
queue.appendListener(1, [](const std::string & s) {
    std::cout << "Got: " << s << std::endl;
});
queue.enqueue(1, "Hello");
queue.process();
```

### HighPerfPolicy（推荐）

原版 `EventQueue` 默认使用 `std::mutex` 加锁 + `std::list` 每次 enqueue 都 malloc，在高频事件场景下锁竞争和堆分配抖动是主要瓶颈。

`HighPerfPolicy` 将上述问题打包解决——只需加一个模板参数，内部自动切换为：
- **SpinLock**（短临界区下比 mutex 快）
- **池化分配器**（预分配内存池，消除 malloc 抖动）
- **读写锁分离**（多线程 dispatch 不互斥）

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

更多 Policy 配置细节见 [doc/policies.md](doc/policies.md)。

### processQueueWith（零开销分发）

单消费者场景下，`process()` 需要经过 shared_lock + map.find + CallbackList + std::function 多层间接调用。
`processQueueWith` 绕过全部分发基础设施，直接将事件传递给编译期已知的 Visitor，编译器可完全内联：

```cpp
// 定义 Visitor（编译期类型已知，可内联）
struct MyVisitor {
    void operator()(int event, const SensorData& d) {
        switch(event) {
            case 1: printf("Sensor %u: %.1f\n", d.id, d.value); break;
            case 2: /* handle event 2 */ break;
        }
    }
};

eventpp::EventQueue<int, void(const SensorData&)> queue;
queue.enqueue(1, SensorData{42, 25.3f});

// 零开销分发: 15x faster than process()
queue.processQueueWith(MyVisitor{});

// Lambda 也支持
queue.enqueue(1, SensorData{99, 36.6f});
queue.processQueueWith([](int event, const SensorData& d) {
    printf("Event %d: sensor %u = %.1f\n", event, d.id, d.value);
});
```

详见 [docs/design_process_queue_with_zh.md](docs/design_process_queue_with_zh.md)。

## 性能数据

测试环境：Ubuntu 24.04, GCC 13.3, `-O3 -march=native`

### EventQueue 吞吐量

| 场景 | 原版 v0.1.3 (std::list) | 本版本 PoolQueueList | 提升 |
|------|:------:|:------:|:----:|
| 1K msg | 基线 | 25.9 M/s | — |
| 10K msg | 基线 | 33.1 M/s | — |
| 100K msg | 基线 | 31.7 M/s | — |
| 1M msg | 基线 | 31.2 M/s | — |

> 原版在大批量场景下每次 enqueue 都走 malloc，延迟不可预测。本版本 Pool 吞吐稳定在 25~33 M/s，无回退。

### processQueueWith vs process() 分发延迟

| 场景 | process() | processQueueWith() | 加速比 |
|------|:---------:|:-------------------:|:------:|
| 单事件 ID, 100K 消息 | 152 ns/msg | 9 ns/msg | **16.7x** |
| 10 事件 ID, 100K 消息 | 152 ns/msg | 10 ns/msg | **15.2x** |
| 10 事件 ID, 1M 消息 | 77 ns/msg | 21 ns/msg | **3.6x** |

> processQueueWith 绕过 shared_lock + map.find + CallbackList + std::function，直接调用 Visitor。中位数 (P50): 6 ns/msg vs 155 ns/msg = 25x。

## 示例程序

`examples/` 目录包含两个示例，详见 [examples/README.md](examples/README.md)：

| 示例 | 文件 | 目的 |
|------|------|------|
| HighPerfPolicy 基础 | `example_highperf_eventqueue.cpp` | 演示 HighPerfPolicy 零配置用法，对比 DefaultPolicies vs HighPerfPolicy 在 MPSC 场景下的吞吐量 |
| Active Object + HSM | `example_active_object_hsm.cpp` | 演示 Active Object 模式（独立线程 + 事件队列）、层次状态机（复合状态、Entry/Exit 动作、Guard 条件）、shared_ptr 零拷贝数据传递 |

## 测试

`tests/` 目录包含单元测试、性能基准和教程代码，详见 [tests/README.md](tests/README.md)：

| 目录 | 内容 | 说明 |
|------|------|------|
| `unittest/` | 28 个测试文件, 220 用例, 1980 断言 | 覆盖核心组件、异构变体、线程安全、工具类、Visitor 分发 |
| `benchmark/` | 9 个基准测试 | EventQueue 吞吐、CallbackList 调用开销、Pool 分配器、Visitor 分发性能 |
| `tutorial/` | 9 个教程文件 | 各组件的入门用法示例 |

## 文档

完整文档位于 `doc/` 目录：

- [安装指南](doc/install.md)
- [CallbackList 教程](doc/tutorial_callbacklist.md) / [API 参考](doc/callbacklist.md)
- [EventDispatcher 教程](doc/tutorial_eventdispatcher.md) / [API 参考](doc/eventdispatcher.md)
- [EventQueue 教程](doc/tutorial_eventqueue.md) / [API 参考](doc/eventqueue.md)
- [Policy 配置](doc/policies.md) / [Mixin 扩展](doc/mixins.md)
- [AnyData — 零堆分配事件数据](doc/anydata.md)
- [性能基准](doc/benchmark.md)
- [FAQ](doc/faq.md)
- [中文文档](doc/cn/readme.md)

## 编译与测试

```bash
cd tests && mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target unittest --target b9_raw_benchmark --target b10_visitor_benchmark -j$(nproc)
ctest --output-on-failure    # 220 测试用例
./benchmark/b9_raw_benchmark       # 性能基准测试
./benchmark/b10_visitor_benchmark  # Visitor 分发性能对比
```

## C++ 标准

- 库本身：C++14
- 单元测试：C++17

## 许可证

Apache License, Version 2.0

## 致谢

- [wqking/eventpp](https://github.com/wqking/eventpp) — 原始库及所有贡献者
- [iceoryx](https://github.com/eclipse-iceoryx/iceoryx) — PoolAllocator / MemPool 设计灵感
