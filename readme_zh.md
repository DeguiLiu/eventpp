**中文** | [English](readme.md)

# eventpp -- 高性能 C++ 事件库

> 基于 [wqking/eventpp](https://github.com/wqking/eventpp) v0.1.3 独立维护，完全兼容原版 API。

[![CI](https://github.com/DeguiLiu/eventpp/actions/workflows/main.yml/badge.svg)](https://github.com/DeguiLiu/eventpp/actions/workflows/main.yml)
[![License: Apache-2.0](https://img.shields.io/badge/License-Apache--2.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)

eventpp 提供三大核心组件：回调列表（CallbackList）、事件分发器（EventDispatcher）和异步事件队列（EventQueue），支持信号/槽、发布/订阅、观察者等常见模式。

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

## 性能数据

测试环境：Ubuntu 24.04, GCC 13.3, `-O3 -march=native`

### EventQueue 吞吐量

| 场景 | 优化前 (v0.1.3) | 优化后 (PoolQueueList) | 提升 |
|------|:---------------:|:---------------------:|:----:|
| 10K msg | 23.5 M/s | 33.1 M/s | +41% |
| 100K msg | 基线 | 31.7 M/s | -- |
| 1M msg | 基线 | 31.2 M/s | -- |

### processQueueWith vs process()

| 场景 | process() | processQueueWith() | 加速比 |
|------|:---------:|:-------------------:|:------:|
| 单事件 ID, 100K 消息 | 152 ns/msg | 9 ns/msg | **16.7x** |
| 10 事件 ID, 100K 消息 | 152 ns/msg | 10 ns/msg | **15.2x** |
| 10 事件 ID, 1M 消息 | 77 ns/msg | 21 ns/msg | **3.6x** |

### 端到端

| 指标 | 优化前 | 优化后 | 提升 |
|------|:------:|:-----:|:----:|
| Active Object 吞吐 (10K) | ~1.6 M/s | 3.9 M/s | 2.5x |
| Active Object 持续吞吐 (5s) | ~1.25 M/s | 3.5 M/s | 2.8x |
| E2E P50 延迟 | ~1,200 ns | 626 ns | 48% 降低 |

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

### HighPerfPolicy

```cpp
#include <eventpp/eventqueue.h>

struct SensorData {
    uint32_t id;
    float value;
};

// HighPerfPolicy = SpinLock + PoolAllocator + shared_mutex，零配置
eventpp::EventQueue<int, void(const SensorData&), eventpp::HighPerfPolicy> queue;

queue.appendListener(1, [](const SensorData& d) {
    printf("Sensor %u: %.1f\n", d.id, d.value);
});

queue.enqueue(1, SensorData{42, 25.3f});
queue.process();
```

### processQueueWith（零开销分发）

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

// 绕过 shared_lock + map.find + CallbackList + std::function
// 比 process() 快 15x
queue.processQueueWith(MyVisitor{});
```

详见 [doc/design_process_queue_with_zh.md](doc/design_process_queue_with_zh.md)。

## 文档

- [安装指南](doc/install.md)
- [CallbackList 教程](doc/tutorial_callbacklist.md) / [API 参考](doc/callbacklist.md)
- [EventDispatcher 教程](doc/tutorial_eventdispatcher.md) / [API 参考](doc/eventdispatcher.md)
- [EventQueue 教程](doc/tutorial_eventqueue.md) / [API 参考](doc/eventqueue.md)
- [Policy 配置](doc/policies.md) / [Mixin 扩展](doc/mixins.md)
- [AnyData -- 零堆分配事件数据](doc/anydata.md)
- [性能基准](doc/benchmark.md)
- [FAQ](doc/faq.md)
- [中文文档](doc/cn/readme.md)

## 修改文件

| 文件 | 涉及优化 |
|------|----------|
| `include/eventpp/eventpolicies.h` | OPT-1, OPT-3, OPT-6 |
| `include/eventpp/callbacklist.h` | OPT-2 |
| `include/eventpp/eventdispatcher.h` | OPT-3 |
| `include/eventpp/hetereventdispatcher.h` | OPT-3 |
| `include/eventpp/eventqueue.h` | OPT-4, OPT-6, OPT-8, OPT-15 |
| `include/eventpp/internal/eventqueue_i.h` | OPT-7 |
| `include/eventpp/internal/poolallocator_i.h` | OPT-5 (新增) |

## 示例

| 示例 | 文件 | 说明 |
|------|------|------|
| HighPerfPolicy 基础 | `example_highperf_eventqueue.cpp` | DefaultPolicies vs HighPerfPolicy MPSC 吞吐量对比 |
| Active Object + HSM | `example_active_object_hsm.cpp` | Active Object 模式、层次状态机、shared_ptr 零拷贝 |

## 编译与测试

```bash
cd tests && mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target unittest --target b9_raw_benchmark --target b10_visitor_benchmark -j$(nproc)
ctest --output-on-failure    # 220 测试用例
./benchmark/b9_raw_benchmark       # 性能基准测试
./benchmark/b10_visitor_benchmark  # Visitor 分发性能对比
```

详细优化技术报告见 [doc/optimization_report.md](doc/optimization_report.md)。

## C++ 标准

- 库本身：C++14（OPT-3 使用 `shared_timed_mutex`）
- 单元测试：C++17

## 许可证

Apache License, Version 2.0

## 致谢

- [wqking/eventpp](https://github.com/wqking/eventpp) -- 原始库及所有贡献者
- [iceoryx](https://github.com/eclipse-iceoryx/iceoryx) -- PoolAllocator / MemPool 设计灵感
