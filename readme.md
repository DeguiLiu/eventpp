# eventpp — 高性能 C++ 事件库

> 基于 [wqking/eventpp](https://github.com/wqking/eventpp) v0.1.3 独立维护，实施了 14 项跨平台性能优化。

eventpp 提供三大核心组件：回调列表（CallbackList）、事件分发器（EventDispatcher）和异步事件队列（EventQueue），支持信号/槽、发布/订阅、观察者等常见模式。

![C++](https://img.shields.io/badge/C%2B%2B-14-blue)
![License](https://img.shields.io/badge/License-Apache--2.0-blue)
![Platform](https://img.shields.io/badge/Platform-ARM%20%7C%20x86-green)
![Version](https://img.shields.io/badge/Version-v0.3.0-orange)

## 特性

- 同步事件分发 + 异步事件队列
- 支持嵌套事件（处理事件时可安全分发新事件、增删监听器）
- 线程安全，异常安全
- 通过 Policy 和 Mixin 机制灵活配置和扩展
- Header-only，无外部依赖
- 完全兼容原版 eventpp v0.1.3 API
- HighPerfPolicy 一站式高性能预设（v0.3.0）

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

零配置获得最优性能组合 — SpinLock + PoolAllocator + shared_mutex：

```cpp
#include <eventpp/eventqueue.h>
#include <eventpp/internal/poolallocator_i.h>

// 一行搞定
eventpp::EventQueue<int, void(const Message&), eventpp::HighPerfPolicy> queue;
```

等价于手动配置：

```cpp
struct ManualHighPerf {
    using Threading = eventpp::GeneralThreading<eventpp::SpinLock>;
    template <typename T>
    using QueueList = eventpp::PoolQueueList<T, 8192>;
};
```

## 性能优化

### v0.1.0 → v0.2.0 (OPT-1 ~ OPT-8)

| OPT | 优化内容 | 收益 |
|:---:|----------|------|
| 1 | SpinLock CPU hint（ARM `YIELD` / x86 `PAUSE`） | 降低自旋功耗 |
| 2 | CallbackList 批量预取（每 8 节点加锁一次） | P99 延迟大幅降低 |
| 3 | EventDispatcher `shared_mutex` 读写分离 | 多线程 dispatch 不互斥 |
| 4 | doEnqueue `try_lock`（非阻塞 freeList） | 减少入队锁竞争 |
| 5 | PoolAllocator 池化分配器（opt-in） | 小批量吞吐 +33% |
| 6 | Cache-line 对齐 | 消除 false sharing |
| 7 | 内存序 `acq_rel`（替代 `seq_cst`） | 减少屏障指令 |
| 8 | waitFor 自适应 spin | 减少 futex 系统调用 |

### v0.2.0 → v0.3.0 (OPT-9 ~ OPT-14)

借鉴 [iceoryx](https://github.com/eclipse-iceoryx/iceoryx) MemPool 设计的多平台适配优化：

| OPT | 优化内容 | 收益 |
|:---:|----------|------|
| 9a | PoolAllocator 多级 slab 链式扩展 | 消除大批量 slab 耗尽回退 |
| 9b | Lock-free free list（atomic CAS 栈） | 消除 allocate/deallocate 锁竞争 |
| 10 | Cache-line 平台自动检测（Apple Silicon 128B） | 正确的 false sharing 防御 |
| 11 | SpinLock 指数退避（fast path + max=64） | 减少高竞争下缓存行 bouncing |
| 14 | HighPerfPolicy 一站式预设 | 用户零配置 |

### 性能数据

测试环境：Ubuntu 24.04, GCC 13.3, `-O3 -march=native`

#### Pool 吞吐量 (v0.2.0 → v0.3.0)

| 场景 | v0.2.0 | v0.3.0 | 提升 |
|------|:------:|:------:|:----:|
| 1K msg | 22.7 M/s | 25.9 M/s | +14% |
| 10K msg | 31.9 M/s | 33.1 M/s | +4% |
| 100K msg | 25.2 M/s | 31.7 M/s | +26% |
| 1M msg | 21.5 M/s | 31.2 M/s | +45% |

> v0.2.0 的 100K/1M slab 耗尽回退问题已被 OPT-9a 多级 slab 彻底消除。

## 文档

完整文档继承自原版 eventpp，位于 `doc/` 目录：

- [安装指南](doc/install.md)
- [CallbackList 教程](doc/tutorial_callbacklist.md) / [API 参考](doc/callbacklist.md)
- [EventDispatcher 教程](doc/tutorial_eventdispatcher.md) / [API 参考](doc/eventdispatcher.md)
- [EventQueue 教程](doc/tutorial_eventqueue.md) / [API 参考](doc/eventqueue.md)
- [Policy 配置](doc/policies.md) / [Mixin 扩展](doc/mixins.md)
- [AnyData — 零堆分配事件数据](doc/anydata.md)
- [性能基准](doc/benchmark.md)
- [FAQ](doc/faq.md)
- [中文文档](doc/cn/readme.md)

## 修改文件

| 文件 | 涉及优化 |
|------|----------|
| `include/eventpp/eventpolicies.h` | OPT-1, OPT-3, OPT-6, OPT-10, OPT-11, OPT-14 |
| `include/eventpp/callbacklist.h` | OPT-2 |
| `include/eventpp/eventdispatcher.h` | OPT-3 |
| `include/eventpp/hetereventdispatcher.h` | OPT-3 |
| `include/eventpp/eventqueue.h` | OPT-4, OPT-6, OPT-8 |
| `include/eventpp/internal/eventqueue_i.h` | OPT-7 |
| `include/eventpp/internal/poolallocator_i.h` | OPT-5, OPT-9a, OPT-9b, OPT-11, OPT-14 |

## 编译与测试

```bash
cd tests && mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target unittest --target b9_raw_benchmark -j$(nproc)
ctest --output-on-failure    # 210 测试用例
./benchmark/b9_raw_benchmark  # 性能基准测试
```

## C++ 标准

- 库本身：C++14（OPT-3 使用 `shared_timed_mutex`）
- 单元测试：C++17

## 许可证

Apache License, Version 2.0

## 致谢

- [wqking/eventpp](https://github.com/wqking/eventpp) — 原始库及所有贡献者
- [iceoryx](https://github.com/eclipse-iceoryx/iceoryx) — PoolAllocator / MemPool 设计灵感
