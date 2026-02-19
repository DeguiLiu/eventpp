[中文](readme_zh.md) | **English**

# eventpp -- High-Performance C++ Event Library

> Independently maintained fork of [wqking/eventpp](https://github.com/wqking/eventpp) v0.1.3, fully compatible with the original API.

[![CI](https://github.com/DeguiLiu/eventpp/actions/workflows/main.yml/badge.svg)](https://github.com/DeguiLiu/eventpp/actions/workflows/main.yml)
[![License: Apache-2.0](https://img.shields.io/badge/License-Apache--2.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)

eventpp provides three core components: CallbackList, EventDispatcher, and EventQueue, supporting common patterns such as signal/slot, publish/subscribe, and observer.

## Problems Solved (vs v0.1.3)

The original eventpp v0.1.3 has several issues under high-concurrency and embedded real-time scenarios. This fork addresses them:

| Problem | Impact | Solution | Benefit |
|---------|--------|----------|---------|
| SpinLock without CPU hint | High power consumption during spin on ARM/x86 | Platform-aware `YIELD`/`PAUSE` + exponential backoff | Reduced spin power, less cache-line bouncing |
| CallbackList locks on every callback | Tail latency spikes with many listeners | Batched prefetch (lock once per 8 nodes) | Significant P99 latency reduction |
| EventDispatcher read-write mutual exclusion | Multi-threaded dispatch blocks each other | `shared_mutex` read-write separation | Concurrent dispatch without blocking |
| EventQueue enqueue lock contention | Multi-producer enqueue serialized | `try_lock` non-blocking freeList + lock-free CAS | Eliminated enqueue contention |
| malloc on every enqueue | Heap allocation jitter, unpredictable latency | PoolAllocator pooled allocator | Small-batch throughput +33% |
| Pool exhaustion falls back to malloc | Pool advantage lost at 100K+ events | Multi-level slab chain expansion (inspired by iceoryx MemPool) | 100K throughput +26%, 1M throughput +45% |
| Cache-line size hardcoded to 64B | False sharing on Apple Silicon (128B prefetch) | Platform auto-detection (Apple Silicon 128B / others 64B) | Correct false sharing prevention |
| All memory ordering `seq_cst` | Unnecessary barrier instruction overhead | Precise `acq_rel` semantics | Fewer barrier instructions |
| waitFor uses futex directly | Syscall overhead for short waits | Adaptive spin-then-futex | Fewer syscalls |
| High-perf config requires manual Policy combination | Users must know SpinLock, PoolQueueList, GeneralThreading | `HighPerfPolicy` one-stop preset | Zero-config optimal combination |
| process() dispatch chain overhead | shared_lock + map.find + CallbackList + std::function unnecessary for single consumer | `processQueueWith<Visitor>` zero-overhead compile-time dispatch | **15x speedup** in single-consumer scenarios |

## Performance Data

Test environment: Ubuntu 24.04, GCC 13.3, `-O3 -march=native`

### EventQueue Throughput

| Scenario | Before (v0.1.3) | After (PoolQueueList) | Improvement |
|----------|:---------------:|:---------------------:|:-----------:|
| 10K msg | 23.5 M/s | 33.1 M/s | +41% |
| 100K msg | baseline | 31.7 M/s | -- |
| 1M msg | baseline | 31.2 M/s | -- |

### processQueueWith vs process()

| Scenario | process() | processQueueWith() | Speedup |
|----------|:---------:|:-------------------:|:-------:|
| 1 event ID, 100K msg | 152 ns/msg | 9 ns/msg | **16.7x** |
| 10 event IDs, 100K msg | 152 ns/msg | 10 ns/msg | **15.2x** |
| 10 event IDs, 1M msg | 77 ns/msg | 21 ns/msg | **3.6x** |

### End-to-End

| Metric | Before | After | Improvement |
|--------|:------:|:-----:|:-----------:|
| Active Object throughput (10K) | ~1.6 M/s | 3.9 M/s | 2.5x |
| Active Object sustained (5s) | ~1.25 M/s | 3.5 M/s | 2.8x |
| E2E P50 latency | ~1,200 ns | 626 ns | 48% lower |

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

// HighPerfPolicy = SpinLock + PoolAllocator + shared_mutex, zero config
eventpp::EventQueue<int, void(const SensorData&), eventpp::HighPerfPolicy> queue;

queue.appendListener(1, [](const SensorData& d) {
    printf("Sensor %u: %.1f\n", d.id, d.value);
});

queue.enqueue(1, SensorData{42, 25.3f});
queue.process();
```

### processQueueWith (Zero-Overhead Dispatch)

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

// Bypasses shared_lock + map.find + CallbackList + std::function
// 15x faster than process()
queue.processQueueWith(MyVisitor{});
```

See [doc/design_process_queue_with_zh.md](doc/design_process_queue_with_zh.md) for design details.

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

## Examples

| Example | File | Description |
|---------|------|-------------|
| HighPerfPolicy basics | `example_highperf_eventqueue.cpp` | DefaultPolicies vs HighPerfPolicy MPSC throughput comparison |
| Active Object + HSM | `example_active_object_hsm.cpp` | Active Object pattern, hierarchical state machine, shared_ptr zero-copy |

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
