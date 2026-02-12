/**
 * @file b9_eventqueue_raw_benchmark.cpp
 * @brief eventpp::EventQueue Performance Benchmark with Statistical Analysis
 *
 * Validates OPT-1~8 performance optimizations:
 * - OPT-1: SpinLock CPU hint (ARM YIELD / x86 PAUSE)
 * - OPT-2: CallbackList batched prefetch traversal
 * - OPT-3: EventDispatcher read-write lock separation (shared_mutex)
 * - OPT-4: doEnqueue try_lock (non-blocking freeList)
 * - OPT-5: PoolAllocator for std::list (opt-in via Policy)
 * - OPT-6: Cache-line alignment
 * - OPT-7: Memory order optimization (seq_cst -> acq_rel)
 * - OPT-8: waitFor adaptive spin (Spin -> Yield -> Sleep)
 *
 * Measurement methodology:
 * - Throughput: messages / publish_time (producer-side only)
 * - Latency: publish_time / messages (average per-message enqueue time)
 * - Does NOT include consumer processing time
 *
 * Statistical method:
 * - Multiple rounds per scenario
 * - Reports: mean, std deviation, min, max, P50, P95, P99
 *
 * Build:
 *   cd tests/build && cmake .. -DCMAKE_BUILD_TYPE=Release
 *   cmake --build . --target b9_raw_benchmark
 *   ./benchmark/b9_raw_benchmark
 */

#include "bench_utils.hpp"

#include <eventpp/eventqueue.h>
#include <eventpp/internal/poolallocator_i.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <new>
#include <numeric>
#include <thread>
#include <vector>

using namespace std::chrono;

// ============================================================================
// Configuration
// ============================================================================

namespace config {
constexpr uint32_t WARMUP_ROUNDS = 3U;
constexpr uint32_t TEST_ROUNDS = 10U;
}  // namespace config

// ============================================================================
// Statistical Functions
// ============================================================================

struct Statistics {
  double mean;
  double std_dev;
  double min_val;
  double max_val;
  double p50;
  double p95;
  double p99;
};

Statistics calculate_statistics(const std::vector<double>& data) {
  Statistics stats{};

  if (data.empty()) {
    return stats;
  }

  double sum = std::accumulate(data.begin(), data.end(), 0.0);
  stats.mean = sum / static_cast<double>(data.size());

  double sq_sum = 0.0;
  for (const auto& val : data) {
    sq_sum += (val - stats.mean) * (val - stats.mean);
  }
  stats.std_dev = std::sqrt(sq_sum / static_cast<double>(data.size()));

  auto minmax = std::minmax_element(data.begin(), data.end());
  stats.min_val = *minmax.first;
  stats.max_val = *minmax.second;

  std::vector<double> sorted_data = data;
  std::sort(sorted_data.begin(), sorted_data.end());

  size_t n = sorted_data.size();
  stats.p50 = sorted_data[n * 50 / 100];
  stats.p95 = sorted_data[std::min(n * 95 / 100, n - 1)];
  stats.p99 = sorted_data[std::min(n * 99 / 100, n - 1)];

  return stats;
}

// ============================================================================
// Test Message Structure
// ============================================================================

struct TestMessage {
  uint64_t id;
  float data[4];

  TestMessage() : id(0), data{0, 0, 0, 0} {}
  TestMessage(uint64_t id_, float d0, float d1, float d2, float d3) : id(id_), data{d0, d1, d2, d3} {}
};

// ============================================================================
// Benchmark Results
// ============================================================================

struct BenchmarkResult {
  uint64_t messages_sent;
  uint64_t messages_processed;
  double total_time_us;
  double throughput_mps;
  double avg_latency_ns;
};

// ============================================================================
// Policy: PoolQueueList (OPT-5)
// ============================================================================

struct PoolQueueListPolicies {
  template <typename T>
  using QueueList = eventpp::PoolQueueList<T, 8192>;
};

// ============================================================================
// Generic Benchmark Function (works with any EventQueue policy)
// ============================================================================

template <typename QueueType>
BenchmarkResult benchmark_eventpp_queue(uint32_t message_count) {
  QueueType queue;

  std::atomic<uint64_t> processed{0};

  queue.appendListener(1, [&processed](const TestMessage& msg) {
    processed.fetch_add(1, std::memory_order_relaxed);
    (void)msg;
  });

  auto start = high_resolution_clock::now();

  for (uint32_t i = 0; i < message_count; ++i) {
    float fi = static_cast<float>(i);
    queue.enqueue(1, TestMessage(i, fi, fi * 2, fi * 3, fi * 4));
  }

  auto publish_end = high_resolution_clock::now();

  queue.process();

  auto publish_duration = duration_cast<nanoseconds>(publish_end - start);

  BenchmarkResult result;
  result.messages_sent = message_count;
  result.messages_processed = processed.load();
  result.total_time_us = static_cast<double>(publish_duration.count()) / 1000.0;
  result.throughput_mps = (static_cast<double>(message_count) / static_cast<double>(publish_duration.count())) * 1000.0;
  result.avg_latency_ns = static_cast<double>(publish_duration.count()) / static_cast<double>(message_count);

  return result;
}

// Queue type aliases
using RawQueue = eventpp::EventQueue<int, void(const TestMessage&)>;
using PoolQueue = eventpp::EventQueue<int, void(const TestMessage&), PoolQueueListPolicies>;

// ============================================================================
// Active Object style benchmark (with shared_ptr overhead)
// ============================================================================

struct EventPayloadLite {
  int event_id;
  std::shared_ptr<void> data;

  template <typename T>
  EventPayloadLite(int id, T&& payload)
      : event_id(id), data(std::make_shared<typename std::decay<T>::type>(std::forward<T>(payload))) {}
};

BenchmarkResult benchmark_eventpp_with_shared_ptr(uint32_t message_count) {
  using WrapperQueue = eventpp::EventQueue<int, void(const EventPayloadLite&)>;
  WrapperQueue queue;

  std::atomic<uint64_t> processed{0};

  queue.appendListener(1, [&processed](const EventPayloadLite& event) {
    processed.fetch_add(1, std::memory_order_relaxed);
    (void)event;
  });

  auto start = high_resolution_clock::now();

  for (uint32_t i = 0; i < message_count; ++i) {
    float fi = static_cast<float>(i);
    queue.enqueue(1, EventPayloadLite(1, TestMessage(i, fi, fi * 2, fi * 3, fi * 4)));
  }

  auto publish_end = high_resolution_clock::now();

  queue.process();

  auto publish_duration = duration_cast<nanoseconds>(publish_end - start);

  BenchmarkResult result;
  result.messages_sent = message_count;
  result.messages_processed = processed.load();
  result.total_time_us = static_cast<double>(publish_duration.count()) / 1000.0;
  result.throughput_mps = (static_cast<double>(message_count) / static_cast<double>(publish_duration.count())) * 1000.0;
  result.avg_latency_ns = static_cast<double>(publish_duration.count()) / static_cast<double>(message_count);

  return result;
}

// ============================================================================
// Multi-Round Benchmark with Statistics
// ============================================================================

enum class BenchMode { kRaw, kPool, kSharedPtr };

void run_benchmark_with_stats(const char* name, uint32_t message_count, uint32_t rounds, BenchMode mode) {
  std::vector<double> throughputs;
  std::vector<double> latencies;
  throughputs.reserve(rounds);
  latencies.reserve(rounds);

  std::printf("\n========== %s (%u messages, %u rounds) ==========\n", name, message_count, rounds);

  for (uint32_t r = 0U; r < rounds; ++r) {
    BenchmarkResult result;
    switch (mode) {
      case BenchMode::kRaw:
        result = benchmark_eventpp_queue<RawQueue>(message_count);
        break;
      case BenchMode::kPool:
        result = benchmark_eventpp_queue<PoolQueue>(message_count);
        break;
      case BenchMode::kSharedPtr:
        result = benchmark_eventpp_with_shared_ptr(message_count);
        break;
    }
    throughputs.push_back(result.throughput_mps);
    latencies.push_back(result.avg_latency_ns);

    std::this_thread::sleep_for(milliseconds(50));
  }

  Statistics tp_stats = calculate_statistics(throughputs);
  Statistics lat_stats = calculate_statistics(latencies);

  std::printf("\n[%s] Throughput (M msg/s):\n", name);
  std::printf("  Mean:    %.2f\n", tp_stats.mean);
  std::printf("  StdDev:  %.2f\n", tp_stats.std_dev);
  std::printf("  Min:     %.2f\n", tp_stats.min_val);
  std::printf("  Max:     %.2f\n", tp_stats.max_val);
  std::printf("  P50:     %.2f\n", tp_stats.p50);
  std::printf("  P95:     %.2f\n", tp_stats.p95);
  std::printf("\n[%s] Latency (ns/msg):\n", name);
  std::printf("  Mean:    %.2f\n", lat_stats.mean);
  std::printf("  StdDev:  %.2f\n", lat_stats.std_dev);
  std::printf("  Min:     %.2f\n", lat_stats.min_val);
  std::printf("  Max:     %.2f\n", lat_stats.max_val);
  std::printf("  P50:     %.2f\n", lat_stats.p50);
  std::printf("  P95:     %.2f\n", lat_stats.p95);
}

// ============================================================================
// Main
// ============================================================================

int main() {
  std::printf("========================================\n");
  std::printf("   eventpp Performance Benchmark v2.0\n");
  std::printf("========================================\n\n");
  std::printf("Optimizations (OPT-1~8):\n");
  std::printf("  1. SpinLock CPU hint (ARM YIELD / x86 PAUSE)\n");
  std::printf("  2. CallbackList batched prefetch (8x fewer locks)\n");
  std::printf("  3. EventDispatcher shared_mutex (read-write separation)\n");
  std::printf("  4. doEnqueue try_lock (non-blocking freeList)\n");
  std::printf("  5. PoolAllocator for std::list (zero per-node malloc)\n");
  std::printf("  6. Cache-line alignment (anti false sharing)\n");
  std::printf("  7. Memory order acq_rel (barrier reduction)\n");
  std::printf("  8. waitFor adaptive spin (Spin -> Yield -> Sleep)\n");
  std::printf("\nMeasurement: enqueue-only throughput & latency\n");
  std::printf("Warmup: %u rounds | Test: %u rounds\n", config::WARMUP_ROUNDS, config::TEST_ROUNDS);

  // Pin to core 0 for stable measurements
  if (bench::pin_thread_to_core(0)) {
    std::printf("CPU affinity: core 0\n");
  } else {
    std::printf("CPU affinity: not available\n");
  }

  std::printf("\nComparing:\n");
  std::printf("  1. Raw eventpp (value semantics, std::list default allocator)\n");
  std::printf("  2. Raw eventpp + PoolQueueList (pool allocator, zero per-node malloc)\n");
  std::printf("  3. eventpp with shared_ptr wrapper (Active Object style)\n");

  // Warmup
  std::printf("\n[Warmup] Running %u warmup rounds...\n", config::WARMUP_ROUNDS);
  for (uint32_t w = 0U; w < config::WARMUP_ROUNDS; ++w) {
    benchmark_eventpp_queue<RawQueue>(10000);
    benchmark_eventpp_queue<PoolQueue>(10000);
    benchmark_eventpp_with_shared_ptr(10000);
    std::this_thread::sleep_for(milliseconds(100));
  }

  // ========== Section 1: Raw eventpp (default std::list) ==========
  std::printf("\n================================================================================\n");
  std::printf("              RAW EVENTPP (Value Semantics, Default std::list)\n");
  std::printf("================================================================================\n");

  run_benchmark_with_stats("Raw Small", 1000U, config::TEST_ROUNDS, BenchMode::kRaw);
  run_benchmark_with_stats("Raw Medium", 10000U, config::TEST_ROUNDS, BenchMode::kRaw);
  run_benchmark_with_stats("Raw Large", 100000U, config::TEST_ROUNDS, BenchMode::kRaw);
  run_benchmark_with_stats("Raw VeryLarge", 1000000U, config::TEST_ROUNDS, BenchMode::kRaw);

  // ========== Section 2: Raw eventpp + PoolQueueList ==========
  std::printf("\n================================================================================\n");
  std::printf("          RAW EVENTPP + POOLQUEUELIST (Pool Allocator, Zero Malloc)\n");
  std::printf("================================================================================\n");

  run_benchmark_with_stats("Pool Small", 1000U, config::TEST_ROUNDS, BenchMode::kPool);
  run_benchmark_with_stats("Pool Medium", 10000U, config::TEST_ROUNDS, BenchMode::kPool);
  run_benchmark_with_stats("Pool Large", 100000U, config::TEST_ROUNDS, BenchMode::kPool);
  run_benchmark_with_stats("Pool VeryLarge", 1000000U, config::TEST_ROUNDS, BenchMode::kPool);

  // ========== Section 3: eventpp + shared_ptr ==========
  std::printf("\n================================================================================\n");
  std::printf("                   EVENTPP WITH SHARED_PTR (Active Object Style)\n");
  std::printf("================================================================================\n");

  run_benchmark_with_stats("SharedPtr Small", 1000U, config::TEST_ROUNDS, BenchMode::kSharedPtr);
  run_benchmark_with_stats("SharedPtr Medium", 10000U, config::TEST_ROUNDS, BenchMode::kSharedPtr);
  run_benchmark_with_stats("SharedPtr Large", 100000U, config::TEST_ROUNDS, BenchMode::kSharedPtr);
  run_benchmark_with_stats("SharedPtr VeryLarge", 1000000U, config::TEST_ROUNDS, BenchMode::kSharedPtr);

  std::printf("\n========================================\n");
  std::printf("   Benchmark Completed!\n");
  std::printf("========================================\n");

  return 0;
}
