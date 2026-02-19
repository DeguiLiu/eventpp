[中文](readme_zh.md) | **English**

> 基于 [wqking/eventpp](https://github.com/wqking/eventpp) v0.1.3 独立维护，完全兼容原版 API。

> Independently maintained fork of [wqking/eventpp](https://github.com/wqking/eventpp) v0.1.3, with 8 cross-platform performance optimizations.

[![CI](https://github.com/DeguiLiu/eventpp/actions/workflows/main.yml/badge.svg)](https://github.com/DeguiLiu/eventpp/actions/workflows/main.yml)
[![License: Apache-2.0](https://img.shields.io/badge/License-Apache--2.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)

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

| Scenario | Before (v0.1.3) | After | Improvement |
|----------|:---------------:|:-----:|:-----------:|
| Raw EventQueue (10K msg) | 23.5 M/s | 22.0 M/s | On par |
| PoolQueueList (10K msg) | -- | 29.2 M/s | +33% |
| Active Object throughput (10K) | ~1.6 M/s | 3.9 M/s | 2.5x |
| Active Object sustained (5s) | ~1.25 M/s | 3.5 M/s | 2.8x |
| E2E P50 latency | ~1,200 ns | 626 ns | 48% lower |
| Test suite runtime | ~23 s | ~18 s | 22% lower |

## Quick Start

### CMake Integration

```cmake
include(FetchContent)
FetchContent_Declare(
    eventpp
    GIT_REPOSITORY https://github.com/DeguiLiu/eventpp.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(eventpp)
target_include_directories(your_target PRIVATE ${eventpp_SOURCE_DIR}/include)
```

### Basic Usage

```cpp
#include <eventpp/callbacklist.h>
#include <eventpp/eventdispatcher.h>
#include <eventpp/eventqueue.h>

// CallbackList
eventpp::CallbackList<void(const std::string &, bool)> callbackList;
callbackList.append([](const std::string & s, bool b) {
    std::cout << "s=" << s << " b=" << b << std::endl;
});
callbackList("Hello", true);

// EventDispatcher
eventpp::EventDispatcher<int, void()> dispatcher;
dispatcher.appendListener(3, []() { std::cout << "Event 3" << std::endl; });
dispatcher.dispatch(3);

// Async EventQueue
eventpp::EventQueue<int, void(const std::string &)> queue;
queue.appendListener(1, [](const std::string & s) {
    std::cout << "Got: " << s << std::endl;
});
queue.enqueue(1, "Hello");
queue.process();
```

### HighPerfPolicy

```cpp
#include <eventpp/eventqueue.h>

struct SensorData {
    uint32_t id;
    float value;
};

eventpp::EventQueue<int, void(const SensorData&), eventpp::HighPerfPolicy> queue;

queue.appendListener(1, [](const SensorData& d) {
    printf("Sensor %u: %.1f\n", d.id, d.value);
});

queue.enqueue(1, SensorData{42, 25.3f});
queue.process();
```

### processQueueWith

```cpp
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

// 15x faster than process()
queue.processQueueWith(MyVisitor{});
```

## Documentation

- [Installation Guide](doc/install.md)
- [CallbackList Tutorial](doc/tutorial_callbacklist.md) / [API Reference](doc/callbacklist.md)
- [EventDispatcher Tutorial](doc/tutorial_eventdispatcher.md) / [API Reference](doc/eventdispatcher.md)
- [EventQueue Tutorial](doc/tutorial_eventqueue.md) / [API Reference](doc/eventqueue.md)
- [Policy Configuration](doc/policies.md) / [Mixin Extension](doc/mixins.md)
- [AnyData -- Zero-Heap Event Data](doc/anydata.md)
- [Performance Benchmark](doc/benchmark.md)
- [FAQ](doc/faq.md)
- [Chinese Documentation](doc/cn/readme.md)

## Modified Files

| File | Related Optimizations |
|------|----------------------|
| `include/eventpp/eventpolicies.h` | OPT-1, OPT-3, OPT-6 |
| `include/eventpp/callbacklist.h` | OPT-2 |
| `include/eventpp/eventdispatcher.h` | OPT-3 |
| `include/eventpp/hetereventdispatcher.h` | OPT-3 |
| `include/eventpp/eventqueue.h` | OPT-4, OPT-6, OPT-8, OPT-15 |
| `include/eventpp/internal/eventqueue_i.h` | OPT-7 |
| `include/eventpp/internal/poolallocator_i.h` | OPT-5 (new) |

## Build and Test

```bash
cd tests && mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target unittest --target b9_raw_benchmark --target b10_visitor_benchmark -j$(nproc)
ctest --output-on-failure    # 220 test cases
./benchmark/b9_raw_benchmark       # Performance benchmark
./benchmark/b10_visitor_benchmark  # Visitor dispatch benchmark
```

For the detailed optimization technical report, see [doc/optimization_report.md](doc/optimization_report.md).

## C++ Standard

- Library: C++14 (OPT-3 uses `shared_timed_mutex`)
- Unit tests: C++17

## License

Apache License, Version 2.0

## Acknowledgments

- [wqking/eventpp](https://github.com/wqking/eventpp) -- Original library and all contributors
- [iceoryx](https://github.com/eclipse-iceoryx/iceoryx) -- PoolAllocator / MemPool design inspiration
