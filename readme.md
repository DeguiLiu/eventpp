[中文](readme_zh.md) | **English**

# eventpp -- High-Performance C++ Event Library

> Independently maintained fork of [wqking/eventpp](https://github.com/wqking/eventpp) v0.1.3, with 8 cross-platform performance optimizations.

eventpp provides three core components: CallbackList, EventDispatcher, and EventQueue, supporting common patterns such as signal/slot, publish/subscribe, and observer.

![C++](https://img.shields.io/badge/C%2B%2B-14-blue)
![License](https://img.shields.io/badge/License-Apache--2.0-blue)
![Platform](https://img.shields.io/badge/Platform-ARM%20%7C%20x86-green)

## Features

- Synchronous event dispatch + asynchronous event queue
- Nested event support (safely dispatch new events and add/remove listeners during event processing)
- Thread-safe and exception-safe
- Flexible configuration and extension via Policy and Mixin mechanisms
- Header-only, zero external dependencies
- Fully compatible with original eventpp v0.1.3 API

## Performance Optimizations (OPT-1 ~ OPT-8)

All optimizations are cross-platform, benefiting both ARM and x86:

| OPT | Optimization | Benefit |
|:---:|--------------|---------|
| 1 | SpinLock CPU hint (ARM `YIELD` / x86 `PAUSE`) | Reduced spin power consumption |
| 2 | CallbackList batched prefetch (lock once per 8 nodes) | Significant P99 latency reduction |
| 3 | EventDispatcher `shared_mutex` read-write separation | Multi-threaded dispatch without mutual exclusion |
| 4 | doEnqueue `try_lock` (non-blocking freeList) | Reduced enqueue lock contention |
| 5 | PoolAllocator pooled allocator (opt-in) | Small-batch throughput +33% |
| 6 | Cache-line alignment | Eliminates false sharing |
| 7 | Memory ordering `acq_rel` (replacing `seq_cst`) | Fewer barrier instructions |
| 8 | waitFor adaptive spin | Fewer futex syscalls |

### Performance Data

Test environment: Ubuntu 24.04, GCC 13.3, `-O3 -march=native`

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

### Enable PoolQueueList (OPT-5)

```cpp
#include <eventpp/eventqueue.h>
#include <eventpp/internal/poolallocator_i.h>

struct MyPolicies {
    template <typename T>
    using QueueList = eventpp::PoolQueueList<T, 4096>;
};

eventpp::EventQueue<int, void(const Payload&), MyPolicies> queue;
```

## Documentation

Full documentation inherited from the original eventpp, located in `doc/`:

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
| `include/eventpp/eventqueue.h` | OPT-4, OPT-6, OPT-8 |
| `include/eventpp/internal/eventqueue_i.h` | OPT-7 |
| `include/eventpp/internal/poolallocator_i.h` | OPT-5 (new) |

## Build and Test

```bash
cd tests && mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target unittest --target b9_raw_benchmark -j$(nproc)
ctest --output-on-failure    # 209+ test cases
./benchmark/b9_raw_benchmark  # Performance benchmark
```

For the detailed optimization technical report, see [doc/optimization_report.md](doc/optimization_report.md).

## C++ Standard

- Library: C++14 (OPT-3 uses `shared_timed_mutex`)
- Unit tests: C++17

## License

Apache License, Version 2.0

## Acknowledgments

- [wqking/eventpp](https://github.com/wqking/eventpp) -- Original library and all contributors
