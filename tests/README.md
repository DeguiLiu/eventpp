# eventpp 测试目录

> 适用于 eventpp v0.3.0+

---

## 目录结构

```
tests/
├── unittest/       27 个测试文件, 210 用例, 1939 断言
├── benchmark/      8 个基准测试
├── tutorial/       9 个教程文件
└── CMakeLists.txt
```

---

## 单元测试 (unittest/)

### 核心组件

| 文件 | 目的 |
|------|------|
| `test_callbacklist_basic.cpp` | CallbackList 基础功能：嵌套回调、append/prepend、调用顺序 |
| `test_callbacklist_ctors.cpp` | CallbackList 拷贝/移动构造和赋值 |
| `test_callbacklist_multithread.cpp` | CallbackList 线程安全：256 线程 x 4K 任务并发 append |
| `test_dispatcher_basic.cpp` | EventDispatcher 基础功能：string/int 事件类型分发 |
| `test_dispatcher_ctors.cpp` | EventDispatcher 拷贝/移动构造和赋值 |
| `test_dispatcher_multithread.cpp` | EventDispatcher 线程安全：256 线程 x 4K 事件并发 dispatch |
| `test_queue_basic.cpp` | EventQueue 基础功能：enqueue、process、监听器管理 |
| `test_queue_ctors.cpp` | EventQueue 拷贝/移动构造和赋值 |
| `test_queue_multithread.cpp` | EventQueue 线程安全：256 线程 x 4K 事件并发 enqueue |
| `test_queue_ordered_list.cpp` | 有序队列检测：升序、降序、无序 |

### 异构变体 (Heterogeneous)

| 文件 | 目的 |
|------|------|
| `test_hetercallbacklist_basic.cpp` | HeterCallbackList：多种回调签名混合使用 |
| `test_hetercallbacklist_ctors.cpp` | HeterCallbackList 拷贝/移动构造 |
| `test_heterdispatcher_basic.cpp` | HeterEventDispatcher：多种事件签名混合分发 |
| `test_heterdispatcher_ctors.cpp` | HeterEventDispatcher 拷贝/移动构造 |
| `test_heterdispatcher_multithread.cpp` | HeterEventDispatcher 线程安全 |
| `test_heterqueue_basic.cpp` | HeterEventQueue：多种事件类型混合入队处理 |

### 工具类

| 文件 | 目的 |
|------|------|
| `test_eventutil.cpp` | 事件工具函数：监听器移除、过滤 |
| `test_eventmaker.cpp` | EventMaker：事件对象创建工具 |
| `test_conditionalremover.cpp` | ConditionalRemover：按条件自动移除监听器 |
| `test_counterremover.cpp` | CounterRemover：调用 N 次后自动移除监听器 |
| `test_scopedremover.cpp` | ScopedRemover：RAII 风格监听器生命周期管理 |
| `test_argumentadapter.cpp` | ArgumentAdapter：回调签名适配器 |
| `test_conditionalfunctor.cpp` | ConditionalFunctor：带条件的回调包装器 |
| `test_anyid.cpp` | AnyId：使用 std::any 作为事件 ID |
| `test_anydata.cpp` | AnyData：使用 std::any 作为事件数据 |
| `test_no_extra_copy_move.cpp` | 验证事件分发过程中无多余拷贝/移动操作 |

---

## 性能基准 (benchmark/)

| 文件 | 目的 |
|------|------|
| `b1_callbacklist_invoking_vs_cpp.cpp` | CallbackList 调用开销 vs 原生 C++ 函数调用 |
| `b2_map_vs_unordered_map.cpp` | map vs unordered_map 作为事件存储的性能对比 |
| `b3_b5_eventqueue.cpp` | EventQueue 吞吐量基准（覆盖各项优化） |
| `b6_callbacklist_add_remove_callbacks.cpp` | CallbackList 100K 轮 x 1000 回调的 append/remove 性能 |
| `b7_callbacklist_vs_function_list.cpp` | CallbackList vs std::function 列表性能对比 |
| `b8_eventqueue_anydata.cpp` | EventQueue + AnyData 吞吐量 |
| `b9_eventqueue_raw_benchmark.cpp` | EventQueue 原始吞吐量基准，含统计分析（mean/stddev/P50/P95/P99） |

构建目标：
- `benchmark` — 编译 b1~b8
- `b9_raw_benchmark` — 独立目标，用于 CI 性能回归检测

---

## 教程 (tutorial/)

| 文件 | 目的 |
|------|------|
| `tutorial_callbacklist.cpp` | CallbackList 入门用法 |
| `tutorial_eventdispatcher.cpp` | EventDispatcher 入门用法 |
| `tutorial_eventqueue.cpp` | EventQueue 入门用法 |
| `tutorial_hetercallbacklist.cpp` | HeterCallbackList 多签名用法 |
| `tutorial_hetereventdispatcher.cpp` | HeterEventDispatcher 多签名用法 |
| `tutorial_argumentadapter.cpp` | ArgumentAdapter 用法示例 |
| `tutorial_anydata.cpp` | AnyData 用法示例 |
| `tip_use_type_as_id.cpp` | 使用类型作为事件 ID 的技巧 |
| `tutorialmain.cpp` | 教程入口 |

---

## 编译与运行

```bash
cd tests && mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target unittest --target b9_raw_benchmark -j$(nproc)
ctest --output-on-failure
./benchmark/b9_raw_benchmark
```
