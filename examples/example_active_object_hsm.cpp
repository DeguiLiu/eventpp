/**
 * @file example_active_object_hsm.cpp
 * @brief Active Object + HSM (层次状态机) 模式示例
 *
 * 演示内容:
 * 1. 基于 eventpp EventQueue 的 Active Object 模式
 * 2. 生产者-消费者 pipeline (Sensor → Processor → Logger)
 * 3. 层次状态机 (HSM) 控制处理器行为:
 *    - 复合状态: Running 包含 Normal / Degraded 两个子状态
 *    - Entry/Exit 动作: 状态进入/退出时执行副作用
 *    - Guard 条件: Error 恢复有重试次数限制 (最多 3 次)
 * 4. 零拷贝数据传递 (shared_ptr 引用计数)
 *
 * HSM 状态图:
 *
 *   ┌──────────────── Running ────────────────┐
 *   │  ┌──────────┐  Degrade  ┌────────────┐  │
 *   │  │  Normal  │ ────────> │  Degraded  │  │
 *   │  │          │ <──────── │            │  │
 *   │  └──────────┘  Recover  └────────────┘  │
 *   └─────────────────────────────────────────┘
 *        ↑ Start                  │ Pause / Stop / Error
 *      Idle  <──── Stop ──── Paused
 *        ↑                     ↑ Resume
 *        │ Stop                │
 *      Error ── Reset[retries<3] ──> Running::Normal
 *
 * 架构:
 *   SensorAO ──DataReady──> ProcessorAO ──Result──> LoggerAO
 *                               │
 *                          ProcessorHSM
 *
 * 编译:
 *   g++ -std=c++14 -O3 -pthread -I../include example_active_object_hsm.cpp -o example_ao_hsm
 */

#include <eventpp/eventqueue.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// =============================================================================
// Event System
// =============================================================================

namespace EventID {
constexpr uint32_t kStart         = 1;
constexpr uint32_t kStop          = 2;
constexpr uint32_t kPause         = 3;
constexpr uint32_t kResume        = 4;
constexpr uint32_t kDegrade       = 5;   // 数据质量下降
constexpr uint32_t kRecover       = 6;   // 数据质量恢复
constexpr uint32_t kReset         = 7;   // 错误恢复尝试
constexpr uint32_t kDataReady     = 100;
constexpr uint32_t kProcessResult = 101;
constexpr uint32_t kError         = 300;
}

// Type-erased event payload (shared_ptr for multi-consumer)
struct EventPayload {
    uint32_t event_id;
    std::shared_ptr<void> data;

    template <typename T>
    EventPayload(uint32_t id, T&& payload)
        : event_id(id)
        , data(std::make_shared<typename std::decay<T>::type>(
              std::forward<T>(payload))) {}

    explicit EventPayload(uint32_t id) : event_id(id), data(nullptr) {}

    template <typename T>
    const T& Get() const { return *static_cast<const T*>(data.get()); }

    bool HasData() const { return data != nullptr; }
};

// =============================================================================
// Minimal Active Object (eventpp-based)
// =============================================================================

class ActiveObject {
public:
    using Queue = eventpp::EventQueue<uint32_t, void(const EventPayload&),
                                      eventpp::HighPerfPolicy>;
    using Callback = std::function<void(const EventPayload&)>;

    explicit ActiveObject(const char* name) : name_(name), running_(false) {}

    virtual ~ActiveObject() { Stop(); }

    ActiveObject(const ActiveObject&) = delete;
    ActiveObject& operator=(const ActiveObject&) = delete;

    void Subscribe(uint32_t event_id, Callback cb) {
        queue_.appendListener(event_id, std::move(cb));
    }

    void Post(const EventPayload& event) {
        queue_.enqueue(event.event_id, event);
    }

    void Post(uint32_t event_id) {
        queue_.enqueue(event_id, EventPayload(event_id));
    }

    void Start() {
        running_.store(true, std::memory_order_release);
        thread_ = std::thread(&ActiveObject::Run, this);
    }

    void Stop() {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    bool IsRunning() const { return running_.load(std::memory_order_acquire); }
    const char* GetName() const { return name_; }

private:
    void Run() {
        while (running_.load(std::memory_order_acquire)) {
            if (!queue_.processOne()) {
                // No events, yield to avoid busy-wait
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
        queue_.process(); // drain remaining
    }

    const char* name_;
    Queue queue_;
    std::thread thread_;
    std::atomic<bool> running_;
};

// =============================================================================
// ProcessorHSM — 层次状态机
//
// 特性:
//   - 复合状态: Running 包含 Normal 和 Degraded 两个子状态
//   - Entry 动作: 进入状态时执行副作用 (重置计数器、记录日志)
//   - Exit 动作: 离开状态时执行清理
//   - Guard 条件: Reset 事件仅在 retry_count < kMaxRetries 时允许
// =============================================================================

class ProcessorHSM {
public:
    enum class State : uint8_t {
        kIdle,
        kRunning_Normal,    // Running 复合状态 — 正常处理
        kRunning_Degraded,  // Running 复合状态 — 降级处理 (数据质量差)
        kPaused,
        kError
    };

    ProcessorHSM() : state_(State::kIdle), retry_count_(0) {}

    // 事件分发，返回 true 表示状态发生了转移
    bool Dispatch(uint32_t event_id) {
        switch (state_) {

        // --- Idle ---
        case State::kIdle:
            if (event_id == EventID::kStart) {
                DoTransition(State::kRunning_Normal);
                return true;
            }
            break;

        // --- Running (复合状态: Normal + Degraded 共享父状态转移) ---
        case State::kRunning_Normal:
        case State::kRunning_Degraded:
            // 父状态 Running 的公共转移
            if (event_id == EventID::kPause) {
                DoTransition(State::kPaused);
                return true;
            }
            if (event_id == EventID::kStop) {
                DoTransition(State::kIdle);
                return true;
            }
            if (event_id == EventID::kError) {
                DoTransition(State::kError);
                return true;
            }
            // 子状态特有转移
            if (state_ == State::kRunning_Normal
                && event_id == EventID::kDegrade) {
                DoTransition(State::kRunning_Degraded);
                return true;
            }
            if (state_ == State::kRunning_Degraded
                && event_id == EventID::kRecover) {
                DoTransition(State::kRunning_Normal);
                return true;
            }
            break;

        // --- Paused ---
        case State::kPaused:
            if (event_id == EventID::kResume) {
                DoTransition(State::kRunning_Normal);
                return true;
            }
            if (event_id == EventID::kStop) {
                DoTransition(State::kIdle);
                return true;
            }
            break;

        // --- Error ---
        case State::kError:
            if (event_id == EventID::kReset) {
                // Guard 条件: 重试次数限制
                if (retry_count_ <= kMaxRetries) {
                    DoTransition(State::kRunning_Normal);
                    return true;
                }
                printf("  [HSM] Reset REJECTED: retry limit reached (%u/%u)\n",
                       retry_count_, kMaxRetries);
                return false;
            }
            if (event_id == EventID::kStop) {
                DoTransition(State::kIdle);
                return true;
            }
            break;
        }
        return false;
    }

    State GetState() const { return state_; }

    // Running 复合状态判断 (Normal 和 Degraded 都算 Running)
    bool IsRunning() const {
        return state_ == State::kRunning_Normal
            || state_ == State::kRunning_Degraded;
    }

    bool IsDegraded() const {
        return state_ == State::kRunning_Degraded;
    }

    uint32_t RetryCount() const { return retry_count_; }

    const char* StateName() const { return StateToString(state_); }

private:
    static constexpr uint32_t kMaxRetries = 3;

    static const char* StateToString(State s) {
        switch (s) {
        case State::kIdle:             return "Idle";
        case State::kRunning_Normal:   return "Running::Normal";
        case State::kRunning_Degraded: return "Running::Degraded";
        case State::kPaused:           return "Paused";
        case State::kError:            return "Error";
        }
        return "Unknown";
    }

    // --- Entry 动作 ---
    void OnEnter(State s) {
        switch (s) {
        case State::kIdle:
            retry_count_ = 0;  // 回到 Idle 重置重试计数
            printf("  [HSM]   entry: retry counter reset\n");
            break;
        case State::kRunning_Normal:
            printf("  [HSM]   entry: processing normally\n");
            break;
        case State::kRunning_Degraded:
            printf("  [HSM]   entry: WARNING — degraded mode, reduced quality\n");
            break;
        case State::kPaused:
            printf("  [HSM]   entry: data processing suspended\n");
            break;
        case State::kError:
            ++retry_count_;
            printf("  [HSM]   entry: error #%u (max retries: %u)\n",
                   retry_count_, kMaxRetries);
            break;
        }
    }

    // --- Exit 动作 ---
    void OnExit(State s) {
        switch (s) {
        case State::kRunning_Degraded:
            printf("  [HSM]   exit: leaving degraded mode\n");
            break;
        case State::kError:
            printf("  [HSM]   exit: attempting recovery\n");
            break;
        default:
            break;
        }
    }

    void DoTransition(State new_state) {
        const char* old_name = StateToString(state_);
        const char* new_name = StateToString(new_state);
        printf("  [HSM] %s -> %s\n", old_name, new_name);
        OnExit(state_);
        state_ = new_state;
        OnEnter(state_);
    }

    State state_;
    uint32_t retry_count_;
};

// =============================================================================
// Data Structures
// =============================================================================

struct SensorFrame {
    uint32_t frame_id;
    uint64_t timestamp_us;
    uint32_t point_count;
    float data[256]; // simulated sensor readings
};

struct ProcessResult {
    uint32_t frame_id;
    uint32_t valid_count;
    uint32_t total_count;
    float mean_value;
    float max_value;
    bool degraded;  // 是否在降级模式下处理
};

// =============================================================================
// Pipeline: Sensor → Processor → Logger
// =============================================================================

// --- Sensor Active Object ---
class SensorAO : public ActiveObject {
public:
    SensorAO(ActiveObject& downstream)
        : ActiveObject("Sensor"), downstream_(downstream), frame_id_(0)
        , generating_(false) {
        Subscribe(EventID::kStart, [this](const EventPayload&) { OnStart(); });
        Subscribe(EventID::kStop,  [this](const EventPayload&) { OnStop(); });
    }

    uint32_t FrameCount() const {
        return frame_id_.load(std::memory_order_acquire);
    }

private:
    void OnStart() {
        printf("  [Sensor] Start generating\n");
        generating_.store(true, std::memory_order_release);
        gen_thread_ = std::thread(&SensorAO::Generate, this);
    }

    void OnStop() {
        printf("  [Sensor] Stop generating\n");
        generating_.store(false, std::memory_order_release);
        if (gen_thread_.joinable()) {
            gen_thread_.join();
        }
    }

    void Generate() {
        while (generating_.load(std::memory_order_acquire)) {
            auto frame = std::make_shared<SensorFrame>();
            frame->frame_id = frame_id_.fetch_add(1, std::memory_order_relaxed);
            frame->timestamp_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
            frame->point_count = 128 + (frame->frame_id % 128);

            for (uint32_t i = 0; i < frame->point_count; ++i) {
                frame->data[i] = static_cast<float>(
                    (frame->frame_id * 7 + i * 13) % 1000) / 10.0f;
            }

            // Zero-copy: shared_ptr 引用传递
            EventPayload payload(EventID::kDataReady, frame);
            downstream_.Post(payload);

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    ActiveObject& downstream_;
    std::atomic<uint32_t> frame_id_;
    std::atomic<bool> generating_;
    std::thread gen_thread_;
};

// --- Processor Active Object (with HSM) ---
class ProcessorAO : public ActiveObject {
public:
    ProcessorAO(ActiveObject& downstream)
        : ActiveObject("Processor"), downstream_(downstream)
        , processed_(0), dropped_(0) {
        Subscribe(EventID::kDataReady,
                  [this](const EventPayload& e) { OnDataReady(e); });
    }

    void SendCommand(uint32_t cmd) { hsm_.Dispatch(cmd); }
    const char* StateName() const { return hsm_.StateName(); }
    uint32_t RetryCount() const { return hsm_.RetryCount(); }

    uint32_t ProcessedCount() const {
        return processed_.load(std::memory_order_acquire);
    }

    uint32_t DroppedCount() const {
        return dropped_.load(std::memory_order_acquire);
    }

private:
    void OnDataReady(const EventPayload& event) {
        if (!hsm_.IsRunning()) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return; // 非 Running 状态丢弃帧
        }

        auto frame = event.Get<std::shared_ptr<SensorFrame>>();
        if (!frame) { return; }

        // Process: compute statistics
        float sum = 0.0f;
        float max_val = 0.0f;
        uint32_t valid = 0;

        for (uint32_t i = 0; i < frame->point_count && i < 256; ++i) {
            float v = frame->data[i];
            if (v > 1.0f && v < 90.0f) {
                sum += v;
                if (v > max_val) { max_val = v; }
                ++valid;
            }
        }

        ProcessResult result{};
        result.frame_id = frame->frame_id;
        result.valid_count = valid;
        result.total_count = frame->point_count;
        result.mean_value = valid > 0 ? sum / static_cast<float>(valid) : 0.0f;
        result.max_value = max_val;
        result.degraded = hsm_.IsDegraded();

        downstream_.Post(EventPayload(EventID::kProcessResult, result));
        processed_.fetch_add(1, std::memory_order_relaxed);
    }

    ActiveObject& downstream_;
    ProcessorHSM hsm_;
    std::atomic<uint32_t> processed_;
    std::atomic<uint32_t> dropped_;
};

// --- Logger Active Object ---
class LoggerAO : public ActiveObject {
public:
    LoggerAO() : ActiveObject("Logger"), logged_(0), degraded_count_(0) {
        Subscribe(EventID::kProcessResult,
                  [this](const EventPayload& e) { OnResult(e); });
    }

    uint32_t LoggedCount() const {
        return logged_.load(std::memory_order_acquire);
    }

    uint32_t DegradedCount() const {
        return degraded_count_.load(std::memory_order_acquire);
    }

private:
    void OnResult(const EventPayload& event) {
        auto result = event.Get<ProcessResult>();
        uint32_t count = logged_.fetch_add(1, std::memory_order_relaxed);

        if (result.degraded) {
            degraded_count_.fetch_add(1, std::memory_order_relaxed);
        }

        if (count % 50 == 0) {
            printf("  [Logger] Frame %u: %u/%u valid, mean=%.1f, max=%.1f%s\n",
                   result.frame_id, result.valid_count, result.total_count,
                   result.mean_value, result.max_value,
                   result.degraded ? " [DEGRADED]" : "");
        }
    }

    std::atomic<uint32_t> logged_;
    std::atomic<uint32_t> degraded_count_;
};

// =============================================================================
// Main Demo
// =============================================================================

static void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

int main() {
    printf("========================================\n");
    printf("  Active Object + HSM Pipeline Demo\n");
    printf("  eventpp HighPerfPolicy\n");
    printf("========================================\n\n");

    // --- Build pipeline ---
    LoggerAO logger;
    ProcessorAO processor(logger);
    SensorAO sensor(processor);

    // --- Start all AOs ---
    logger.Start();
    processor.Start();
    sensor.Start();

    printf("--- Pipeline started ---\n\n");

    // =========================================================================
    // Phase 1: Idle -> Running::Normal
    // =========================================================================
    printf("[Phase 1] Start — Idle -> Running::Normal\n");
    processor.SendCommand(EventID::kStart);
    sensor.Post(EventID::kStart);

    printf("[Run] Normal processing for 2 seconds...\n\n");
    sleep_ms(2000);

    // =========================================================================
    // Phase 2: Running::Normal -> Running::Degraded (子状态转移)
    // =========================================================================
    printf("\n[Phase 2] Degrade — Running::Normal -> Running::Degraded\n");
    processor.SendCommand(EventID::kDegrade);

    printf("[Run] Degraded processing for 1 second...\n");
    sleep_ms(1000);

    // =========================================================================
    // Phase 3: Running::Degraded -> Running::Normal (子状态恢复)
    // =========================================================================
    printf("\n[Phase 3] Recover — Running::Degraded -> Running::Normal\n");
    processor.SendCommand(EventID::kRecover);

    printf("[Run] Normal processing for 1 second...\n");
    sleep_ms(1000);

    // =========================================================================
    // Phase 4: Pause / Resume (从 Running 复合状态暂停)
    // =========================================================================
    printf("\n[Phase 4] Pause / Resume\n");
    processor.SendCommand(EventID::kPause);

    uint32_t before_pause = processor.ProcessedCount();
    sleep_ms(500);
    uint32_t after_pause = processor.ProcessedCount();
    printf("[Info] Processed during pause: %u (should be 0)\n",
           after_pause - before_pause);

    processor.SendCommand(EventID::kResume);
    printf("[Run] Resumed for 1 second...\n");
    sleep_ms(1000);

    // =========================================================================
    // Phase 5: Error -> Reset (Guard 条件: 重试次数限制)
    // =========================================================================
    printf("\n[Phase 5] Error recovery with retry limit (max 3)\n");

    // 第 1 次错误 + 恢复
    printf("\n  --- Error #1 ---\n");
    processor.SendCommand(EventID::kError);
    sleep_ms(100);
    processor.SendCommand(EventID::kReset);  // retry 1/3, 允许
    sleep_ms(500);

    // 第 2 次错误 + 恢复
    printf("\n  --- Error #2 ---\n");
    processor.SendCommand(EventID::kError);
    sleep_ms(100);
    processor.SendCommand(EventID::kReset);  // retry 2/3, 允许
    sleep_ms(500);

    // 第 3 次错误 + 恢复
    printf("\n  --- Error #3 ---\n");
    processor.SendCommand(EventID::kError);
    sleep_ms(100);
    processor.SendCommand(EventID::kReset);  // retry 3/3, 允许
    sleep_ms(500);

    // 第 4 次错误 — Reset 被 Guard 拒绝
    printf("\n  --- Error #4 (Guard rejects Reset) ---\n");
    processor.SendCommand(EventID::kError);
    sleep_ms(100);
    processor.SendCommand(EventID::kReset);  // retry 4/3, 拒绝!
    printf("[Info] Processor stuck in Error, must Stop to reset\n");

    // =========================================================================
    // Phase 6: Stop -> Idle (重置所有计数器)
    // =========================================================================
    printf("\n[Phase 6] Stop — cleanup\n");
    processor.SendCommand(EventID::kStop);
    sensor.Post(EventID::kStop);
    sleep_ms(200);

    sensor.Stop();
    processor.Stop();
    logger.Stop();

    // --- Statistics ---
    printf("\n========================================\n");
    printf("  Statistics\n");
    printf("========================================\n");
    printf("  Sensor frames generated:  %u\n", sensor.FrameCount());
    printf("  Processor frames handled: %u\n", processor.ProcessedCount());
    printf("  Processor frames dropped: %u\n", processor.DroppedCount());
    printf("  Logger entries written:   %u\n", logger.LoggedCount());
    printf("  Logger degraded entries:  %u\n", logger.DegradedCount());
    printf("  Processor retry count:    %u\n", processor.RetryCount());
    printf("  Processor final state:    %s\n", processor.StateName());
    printf("\nDone.\n");

    return 0;
}
