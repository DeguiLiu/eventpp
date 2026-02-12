/**
 * @file example_highperf_eventqueue.cpp
 * @brief eventpp HighPerfPolicy 高性能事件队列示例
 *
 * 演示内容:
 * 1. HighPerfPolicy 零配置高性能事件队列
 * 2. 多生产者单消费者 (MPSC) 模式
 * 3. 吞吐量对比: DefaultPolicies vs HighPerfPolicy
 *
 * 编译:
 *   g++ -std=c++14 -O3 -pthread -I../include example_highperf_eventqueue.cpp -o example_highperf
 */

#include <eventpp/eventqueue.h>
#include <eventpp/eventdispatcher.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

// =============================================================================
// 1. 基础用法: HighPerfPolicy EventQueue
// =============================================================================

namespace example_basic {

// 事件 ID
enum EventType { kSensorData = 1, kAlarm = 2, kHeartbeat = 3 };

// 事件载荷
struct SensorData {
    uint32_t sensor_id;
    float temperature;
    float humidity;
    uint64_t timestamp;
};

void Run() {
    printf("\n=== Example 1: HighPerfPolicy Basic Usage ===\n\n");

    // 使用 HighPerfPolicy: SpinLock + PoolAllocator, 零配置
    eventpp::EventQueue<int, void(const SensorData&), eventpp::HighPerfPolicy> queue;

    // 注册监听器
    queue.appendListener(kSensorData, [](const SensorData& data) {
        printf("  [Sensor %u] temp=%.1f°C humidity=%.1f%% ts=%lu\n",
               data.sensor_id, data.temperature, data.humidity,
               static_cast<unsigned long>(data.timestamp));
    });

    queue.appendListener(kAlarm, [](const SensorData& data) {
        printf("  [ALARM] Sensor %u: temperature %.1f°C exceeds threshold!\n",
               data.sensor_id, data.temperature);
    });

    // 入队事件
    uint64_t ts = 1000;
    queue.enqueue(kSensorData, SensorData{1, 25.3f, 60.0f, ts++});
    queue.enqueue(kSensorData, SensorData{2, 22.1f, 55.0f, ts++});
    queue.enqueue(kAlarm,      SensorData{3, 85.0f, 30.0f, ts++});
    queue.enqueue(kSensorData, SensorData{1, 25.5f, 61.0f, ts++});

    // 批量处理
    printf("Processing %s:\n", "4 events");
    queue.process();

    printf("\nDone.\n");
}

} // namespace example_basic

// =============================================================================
// 2. 多生产者单消费者 (MPSC) + 吞吐量测试
// =============================================================================

namespace example_mpsc {

struct Message {
    uint32_t producer_id;
    uint32_t sequence;
    uint64_t payload;
};

// 吞吐量测试模板
template <typename QueueType>
double MeasureThroughput(const char* label, size_t num_producers,
                         size_t msgs_per_producer) {
    QueueType queue;
    std::atomic<size_t> consumed{0};
    const size_t total = num_producers * msgs_per_producer;

    queue.appendListener(1, [&consumed](const Message&) {
        consumed.fetch_add(1, std::memory_order_relaxed);
    });

    // 消费者线程
    std::atomic<bool> done{false};
    std::thread consumer([&]() {
        while (!done.load(std::memory_order_acquire)) {
            queue.process();
        }
        // 排空剩余事件
        queue.process();
    });

    // 计时开始
    auto start = std::chrono::high_resolution_clock::now();

    // 生产者线程
    std::vector<std::thread> producers;
    for (size_t p = 0; p < num_producers; ++p) {
        producers.emplace_back([&queue, p, msgs_per_producer]() {
            for (size_t i = 0; i < msgs_per_producer; ++i) {
                queue.enqueue(1, Message{
                    static_cast<uint32_t>(p),
                    static_cast<uint32_t>(i),
                    static_cast<uint64_t>(p * 1000000 + i)
                });
            }
        });
    }

    // 等待生产者完成
    for (auto& t : producers) {
        t.join();
    }

    // 等待消费完成
    while (consumed.load(std::memory_order_acquire) < total) {
        std::this_thread::yield();
    }
    done.store(true, std::memory_order_release);
    consumer.join();

    auto end = std::chrono::high_resolution_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
    double throughput = static_cast<double>(total) / (elapsed_ms / 1000.0) / 1e6;

    printf("  %-25s %zu producers x %zu msgs = %zu total | %.1f ms | %.1f M/s\n",
           label, num_producers, msgs_per_producer, total, elapsed_ms, throughput);

    return throughput;
}

void Run() {
    printf("\n=== Example 2: MPSC Throughput Comparison ===\n\n");

    using DefaultQueue = eventpp::EventQueue<int, void(const Message&)>;
    using HighPerfQueue = eventpp::EventQueue<int, void(const Message&),
                                              eventpp::HighPerfPolicy>;

    constexpr size_t kMsgsPerProducer = 100000;

    // 单生产者
    printf("Single producer:\n");
    MeasureThroughput<DefaultQueue>("DefaultPolicies", 1, kMsgsPerProducer);
    MeasureThroughput<HighPerfQueue>("HighPerfPolicy", 1, kMsgsPerProducer);

    printf("\n");

    // 4 生产者
    printf("4 producers:\n");
    MeasureThroughput<DefaultQueue>("DefaultPolicies", 4, kMsgsPerProducer);
    MeasureThroughput<HighPerfQueue>("HighPerfPolicy", 4, kMsgsPerProducer);

    printf("\nDone.\n");
}

} // namespace example_mpsc

// =============================================================================
// Main
// =============================================================================

int main() {
    printf("========================================\n");
    printf("  eventpp HighPerfPolicy Examples\n");
    printf("========================================\n");

    example_basic::Run();
    example_mpsc::Run();

    printf("\nAll examples completed.\n");
    return 0;
}
