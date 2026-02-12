# eventpp 性能优化技术报告

> 仓库: [gitee.com/liudegui/eventpp](https://gitee.com/liudegui/eventpp) v0.2.0
> 基准版本: eventpp v0.1.3 (wqking/eventpp)
> 平台: 跨平台 (ARM + x86) | C++14

---

## 一、根因分析

通过逐行阅读 eventpp v0.1.3 核心头文件，定位到 6 个性能瓶颈：

| # | 根因 | 位置 | 严重程度 |
|---|------|------|:--------:|
| 1 | CallbackList 遍历时**每个节点都加锁** | `callbacklist.h` doForEachIf | 致命 |
| 2 | EventQueue enqueue **双锁** (freeListMutex + queueListMutex) | `eventqueue.h` doEnqueue | 高 |
| 3 | EventDispatcher dispatch 时**加排他锁查 map** | `eventdispatcher.h` | 高 |
| 4 | SpinLock 无 YIELD 指令，纯烧 CPU | `eventpolicies.h` | 中 |
| 5 | std::list 每节点堆分配，cache 不友好 | `eventpolicies_i.h` | 中 |
| 6 | 无 cache-line 对齐，多核 false sharing | 全局 | 中 |

---

## 二、优化方案 (OPT-1 ~ OPT-8)

### OPT-1: SpinLock CPU Hint [Batch 1]

添加平台特定的 CPU hint，降低自旋功耗：

```cpp
void lock() {
    while(locked.test_and_set(std::memory_order_acquire)) {
#if defined(__aarch64__) || defined(__arm__)
        __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
        __builtin_ia32_pause();
#endif
    }
}
```

### OPT-6: Cache-Line 对齐 [Batch 1]

对 EventQueue 热成员进行 cache-line 隔离，消除 false sharing：

```cpp
#define EVENTPP_ALIGN_CACHELINE alignas(64)

EVENTPP_ALIGN_CACHELINE mutable ConditionVariable queueListConditionVariable;
EVENTPP_ALIGN_CACHELINE mutable Mutex queueListMutex;
EVENTPP_ALIGN_CACHELINE Mutex freeListMutex;
```

### OPT-7: 内存序降级 [Batch 1]

`CounterGuard` 从 `seq_cst` 降级为 `acq_rel`/`release`。ARM 上避免额外的 `dmb ish` 全屏障：

```cpp
struct CounterGuard {
    explicit CounterGuard(T & v) : value(v) {
        value.fetch_add(1, std::memory_order_acq_rel);
    }
    ~CounterGuard() {
        value.fetch_sub(1, std::memory_order_release);
    }
};
```

### OPT-2: CallbackList 批量预取 [Batch 2] ★ 核心改动

原始代码每访问一个 `node->next` 都加锁（N 个回调 = N 次 mutex），这是最大延迟的根因。

改为批量预取：一次加锁读取 8 个节点，无锁遍历批次，再加锁取下一批。锁操作减少约 8 倍。

```cpp
static constexpr size_t kBatchSize = 8;
NodePtr batch[kBatchSize];
while(node) {
    size_t count = 0;
    {
        std::lock_guard<Mutex> lockGuard(mutex);  // 每 8 个节点锁一次
        NodePtr cur = node;
        while(cur && count < kBatchSize) { batch[count++] = cur; cur = cur->next; }
    }
    for(size_t i = 0; i < count; ++i) { /* 无锁执行回调 */ }
    { std::lock_guard<Mutex> lockGuard(mutex); node = batch[count - 1]->next; }
}
```

**安全性说明**：
- `NodePtr = std::shared_ptr<Node>` — batch 持有 shared_ptr 拷贝，保证遍历期间节点内存不被释放
- `doFreeNode()` 明确不修改 `node->next/previous`（源码注释："don't modify node->previous or node->next because node may be still used in a loop"）
- 跨批次删除单元测试覆盖 16/24 callbacks 场景，验证 batch boundary 安全性

> 最初尝试"一次快照全部节点"，但破坏了重入 append 语义（counter overflow 测试失败）。批量预取保留了重入语义。

### OPT-3: EventDispatcher 读写锁分离 [Batch 3]

dispatch（高频）用读锁，appendListener（低频）用写锁，多线程 dispatch 不再互斥：

```cpp
using SharedMutex = std::shared_timed_mutex;  // C++14
// dispatch: std::shared_lock<SharedMutex>   (读锁)
// append:   std::unique_lock<SharedMutex>   (写锁)
```

### OPT-4: doEnqueue try_lock [Batch 4]

`freeListMutex` 改为 `try_lock`，竞争时跳过回收直接分配新节点，不阻塞热路径：

```cpp
std::unique_lock<Mutex> lock(freeListMutex, std::try_to_lock);
if(lock.owns_lock() && !freeList.empty()) {
    tempList.splice(tempList.end(), freeList, freeList.begin());
}
```

### OPT-8: waitFor 自适应退避 [Batch 4]

四阶段等待：快速检查 → CPU hint spin → yield → 回退到 futex：

```cpp
if(doCanProcess()) return true;              // Phase 1: 快速检查 (0ns)
for(int i = 0; i < 128; ++i) {              // Phase 2: spin + CPU hint (~0.5-2us)
    if(doCanProcess()) return true;
    /* yield / pause */
}
for(int i = 0; i < 16; ++i) {               // Phase 3: yield 时间片 (~2-20us)
    if(doCanProcess()) return true;
    std::this_thread::yield();
}
return cv.wait_for(lock, duration, ...);     // Phase 4: futex
```

### OPT-5: PoolAllocator 池化分配器 [Batch 5]

静态 per-type 池化分配器，通过 Policy 机制 opt-in。保留 `splice()` 兼容性（14 处调用）：

```cpp
struct MyPolicies {
    template <typename T>
    using QueueList = eventpp::PoolQueueList<T, 4096>;
};
eventpp::EventQueue<int, void(const Payload&), MyPolicies> queue;
```

关键设计：静态单例池 → `operator==` 恒 true → `splice()` 安全；池耗尽时透明回退到堆分配。

---

## 三、性能数据

测试环境：Ubuntu 24.04, GCC 13.3, `-O3 -march=native`
基准测试：`tests/benchmark/b9_raw_benchmark`（10 轮统计，CPU 亲和性绑定 core 0）

### Raw EventQueue 入队吞吐

| 消息量 | 吞吐量 (Mean) | 延迟 (Mean) | P50 延迟 |
|:------:|:-------------:|:-----------:|:--------:|
| 1K | 14.9 M/s | 68.9 ns | 67.9 ns |
| 10K | 21.4 M/s | 47.5 ns | 48.0 ns |
| 100K | 25.8 M/s | 41.0 ns | 37.4 ns |
| 1M | 23.6 M/s | 44.6 ns | 41.0 ns |

### PoolQueueList vs std::list 对比

| 消息量 | std::list 吞吐 | Pool 吞吐 | 提升 |
|:------:|:-------------:|:---------:|:----:|
| 1K | 14.9 M/s | 30.9 M/s | +107% |
| 10K | 21.4 M/s | 27.5 M/s | +29% |
| 100K | 25.8 M/s | 20.9 M/s | 持平* |
| 1M | 23.6 M/s | 21.5 M/s | 持平* |

> \* 大批量时 Pool 的 slab 耗尽回退到堆分配，优势消失。PoolQueueList 主要收益在中小批量场景。

### shared_ptr 封装开销

| 消息量 | Raw 吞吐 | shared_ptr 吞吐 | 开销 |
|:------:|:--------:|:--------------:|:----:|
| 10K | 21.4 M/s | 17.2 M/s | -20% |
| 1M | 23.6 M/s | 15.6 M/s | -34% |

> shared_ptr 封装模拟 Active Object 模式的类型擦除开销。实际多线程 Active Object 场景下还有额外的线程调度和 E2E 延迟。

---

## 四、设计决策

| 问题 | 选择 | 原因 |
|------|------|------|
| OPT-2: 快照 vs 批量预取 | 批量预取 (8 节点) | 快照破坏重入 append 语义 |
| OPT-3: shared_mutex vs 无锁 map | shared_mutex | 改动小，C++14 兼容 |
| OPT-5: Ring Buffer vs Pool Allocator | Pool Allocator | Ring Buffer 不支持 splice()（14 处调用） |
| OPT-8: 3 阶段 vs 4 阶段退避 | 4 阶段 (Spin→Yield→Sleep) | yield 阶段填补 spin 与 futex 之间的空白 |

---

## 五、修改文件

| 文件 | 涉及 OPT |
|------|----------|
| `include/eventpp/eventpolicies.h` | OPT-1, OPT-3, OPT-6 |
| `include/eventpp/callbacklist.h` | OPT-2 |
| `include/eventpp/eventdispatcher.h` | OPT-3 |
| `include/eventpp/hetereventdispatcher.h` | OPT-3 |
| `include/eventpp/eventqueue.h` | OPT-4, OPT-6, OPT-8 |
| `include/eventpp/internal/eventqueue_i.h` | OPT-7 |
| `include/eventpp/internal/poolallocator_i.h` | OPT-5 (新增) |

---

## 六、验证体系

| 验证项 | 方法 | 通过标准 |
|--------|------|----------|
| 编译 | `cmake --build . --target unittest` | 零错误 |
| 功能 | `ctest` (209+ 测试用例) | 全部 PASS |
| 跨批次安全 | `*cross-batch*` 测试组 (16/24 callbacks) | 20 断言全通过 |
| 线程安全 | `-fsanitize=thread` | 无新增 data race |
| 内存安全 | `-fsanitize=address` + `detect_leaks=1` | 零错误零泄漏 |
| 性能 | `b9_raw_benchmark` | 无回退 >5% |

```bash
cd tests && mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target unittest --target b9_raw_benchmark -j$(nproc)
ctest --output-on-failure
./benchmark/b9_raw_benchmark
```

---

## 七、致谢

- [wqking/eventpp](https://github.com/wqking/eventpp) — 原始库
- [iceoryx](https://github.com/eclipse-iceoryx/iceoryx) — PoolAllocator 设计灵感
