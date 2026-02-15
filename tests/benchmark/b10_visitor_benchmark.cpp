/**
 * @file b10_visitor_benchmark.cpp
 * @brief processQueueWith vs process() performance comparison
 *
 * OPT-15: Zero-overhead visitor dispatch benchmark.
 * Compares the full dispatch chain (process: map + CallbackList + std::function)
 * against direct visitor dispatch (processQueueWith: visitor(event, args...)).
 *
 * Build:
 *   cd tests/build && cmake .. -DCMAKE_BUILD_TYPE=Release
 *   cmake --build . --target b10_visitor_benchmark
 *   ./benchmark/b10_visitor_benchmark
 */

#include "bench_utils.hpp"

#include <eventpp/eventqueue.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <vector>

using namespace std::chrono;

// ============================================================================
// Configuration
// ============================================================================

namespace config {
constexpr uint32_t WARMUP_ROUNDS = 3U;
constexpr uint32_t TEST_ROUNDS = 10U;
constexpr uint32_t QUEUE_SIZE = 100000U;
constexpr uint32_t EVENT_COUNT = 10U;
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
};

static Statistics calculate_statistics(const std::vector<double>& data) {
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

  return stats;
}

// ============================================================================
// Test Message
// ============================================================================

struct TestMessage {
  uint64_t id;
  float data[4];
};

// ============================================================================
// Benchmark: process() -- standard dispatch chain
// ============================================================================

static double bench_process(uint32_t queue_size, uint32_t event_count) {
  using EQ = eventpp::EventQueue<uint32_t, void(const TestMessage&)>;
  EQ queue;

  volatile uint64_t sink = 0;

  for (uint32_t e = 0; e < event_count; ++e) {
    queue.appendListener(e, [&sink](const TestMessage& msg) {
      sink += msg.id;
    });
  }

  // Enqueue
  for (uint32_t i = 0; i < queue_size; ++i) {
    TestMessage msg{};
    msg.id = i;
    queue.enqueue(i % event_count, msg);
  }

  // Measure dispatch
  auto t0 = steady_clock::now();
  queue.process();
  auto t1 = steady_clock::now();

  return duration_cast<nanoseconds>(t1 - t0).count()
      / static_cast<double>(queue_size);
}

// ============================================================================
// Benchmark: processQueueWith() -- zero-overhead visitor dispatch
// ============================================================================

static double bench_process_queue_with(uint32_t queue_size,
                                       uint32_t event_count) {
  using EQ = eventpp::EventQueue<uint32_t, void(const TestMessage&)>;
  EQ queue;

  volatile uint64_t sink = 0;

  // Enqueue
  for (uint32_t i = 0; i < queue_size; ++i) {
    TestMessage msg{};
    msg.id = i;
    queue.enqueue(i % event_count, msg);
  }

  // Measure dispatch with visitor
  auto t0 = steady_clock::now();
  queue.processQueueWith([&sink](uint32_t /*event*/, const TestMessage& msg) {
    sink += msg.id;
  });
  auto t1 = steady_clock::now();

  return duration_cast<nanoseconds>(t1 - t0).count()
      / static_cast<double>(queue_size);
}

// ============================================================================
// Run Benchmark Suite
// ============================================================================

static void run_benchmark(const char* label,
                          double (*bench_fn)(uint32_t, uint32_t),
                          uint32_t queue_size, uint32_t event_count) {
  // Warmup
  for (uint32_t i = 0; i < config::WARMUP_ROUNDS; ++i) {
    bench_fn(queue_size, event_count);
  }

  // Test rounds
  std::vector<double> results;
  results.reserve(config::TEST_ROUNDS);
  for (uint32_t i = 0; i < config::TEST_ROUNDS; ++i) {
    results.push_back(bench_fn(queue_size, event_count));
  }

  Statistics stats = calculate_statistics(results);

  std::printf("  %-35s  mean=%7.1f ns/msg  std=%5.1f  "
              "min=%7.1f  max=%7.1f  P50=%7.1f  P95=%7.1f\n",
              label, stats.mean, stats.std_dev,
              stats.min_val, stats.max_val, stats.p50, stats.p95);
}

// ============================================================================
// Main
// ============================================================================

int main() {
  bench::pin_thread_to_core(1);

  std::printf("================================================================\n");
  std::printf("OPT-15: processQueueWith vs process() Benchmark\n");
  std::printf("================================================================\n");
  std::printf("Queue size: %u messages, Test rounds: %u\n\n",
              config::QUEUE_SIZE, config::TEST_ROUNDS);

  std::printf("--- Single event ID ---\n");
  run_benchmark("process() [1 event]",
                bench_process, config::QUEUE_SIZE, 1);
  run_benchmark("processQueueWith() [1 event]",
                bench_process_queue_with, config::QUEUE_SIZE, 1);

  std::printf("\n--- %u event IDs ---\n", config::EVENT_COUNT);
  run_benchmark("process() [10 events]",
                bench_process, config::QUEUE_SIZE, config::EVENT_COUNT);
  run_benchmark("processQueueWith() [10 events]",
                bench_process_queue_with, config::QUEUE_SIZE,
                config::EVENT_COUNT);

  std::printf("\n--- Large queue (1M messages) ---\n");
  run_benchmark("process() [1M, 10 events]",
                bench_process, 1000000U, config::EVENT_COUNT);
  run_benchmark("processQueueWith() [1M, 10 events]",
                bench_process_queue_with, 1000000U, config::EVENT_COUNT);

  std::printf("\n================================================================\n");
  std::printf("Done.\n");

  return 0;
}
