// eventpp library
// Copyright (C) 2018 Wang Qi (wqking)
// Github: https://github.com/wqking/eventpp
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//   http://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef EVENTOPTIONS_H_730367862613
#define EVENTOPTIONS_H_730367862613

#include "internal/typeutil_i.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <unordered_map>
#include <list>
#include <shared_mutex>

namespace eventpp {

// OPT-10: Cache-line size for alignment with platform auto-detection.
// Apple Silicon (M1/M2/M3) uses 128-byte L2 prefetch granularity;
// x86 and Cortex-A53/A55/A72/A76/Neoverse use 64-byte cache lines.
// User can override via compile flag: -DEVENTPP_CACHELINE_SIZE=128
#ifndef EVENTPP_CACHELINE_SIZE
	#if defined(__APPLE__) && defined(__aarch64__)
		#define EVENTPP_CACHELINE_SIZE 128
	#else
		#define EVENTPP_CACHELINE_SIZE 64
	#endif
#endif

#define EVENTPP_ALIGN_CACHELINE alignas(EVENTPP_CACHELINE_SIZE)

struct TagHomo {};
struct TagCallbackList : public TagHomo {};
struct TagEventDispatcher : public TagHomo {};
struct TagEventQueue : public TagHomo {};

struct TagHeter {};
struct TagHeterCallbackList : public TagHeter {};
struct TagHeterEventDispatcher : public TagHeter {};
struct TagHeterEventQueue : public TagHeter {};

// OPT-11: SpinLock with exponential backoff.
// Fast path: single test_and_set for uncontended case (zero overhead).
// Slow path: exponential backoff reduces cache-line bouncing under contention.
struct SpinLock
{
public:
	void lock() {
		// Fast path: no contention
		if(!locked.test_and_set(std::memory_order_acquire)) {
			return;
		}
		// Slow path: exponential backoff
		unsigned backoff = 1;
		while(locked.test_and_set(std::memory_order_acquire)) {
			for(unsigned i = 0; i < backoff; ++i) {
#if defined(__aarch64__) || defined(__arm__)
				__asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
				__builtin_ia32_pause();
#endif
			}
			if(backoff < kMaxBackoff) {
				backoff <<= 1;
			}
		}
	}

	bool try_lock() {
		return !locked.test_and_set(std::memory_order_acquire);
	}

	void unlock() {
		locked.clear(std::memory_order_release);
	}

private:
	static constexpr unsigned kMaxBackoff = 64;
	std::atomic_flag locked = ATOMIC_FLAG_INIT;
};

template <
	typename Mutex_,
	template <typename > class Atomic_ = std::atomic,
	typename ConditionVariable_ = std::condition_variable,
	typename SharedMutex_ = std::shared_timed_mutex
>
struct GeneralThreading
{
	using Mutex = Mutex_;

	using SharedMutex = SharedMutex_;

	template <typename T>
	using Atomic = Atomic_<T>;

	using ConditionVariable = ConditionVariable_;
};

struct MultipleThreading
{
	using Mutex = std::mutex;

	// OPT-3: SharedMutex for read-write lock separation in EventDispatcher.
	// std::shared_timed_mutex is C++14 compatible.
	using SharedMutex = std::shared_timed_mutex;

	template <typename T>
	using Atomic = std::atomic<T>;

	using ConditionVariable = std::condition_variable;
};

struct SingleThreading
{
	struct Mutex
	{
		void lock() {}
		void unlock() {}
		bool try_lock() { return true; }
	};

	// OPT-3: No-op SharedMutex for single-threaded use.
	struct SharedMutex
	{
		void lock() {}
		void unlock() {}
		void lock_shared() {}
		void unlock_shared() {}
	};
	
	template <typename T>
	struct Atomic
	{
		Atomic() noexcept = default;
		constexpr Atomic(T desired) noexcept
			: value(desired)
		{
		}

		void store(T desired, std::memory_order /*order*/ = std::memory_order_seq_cst) noexcept
		{
			value = desired;
		}
		
		T load(std::memory_order /*order*/ = std::memory_order_seq_cst) const noexcept
		{
			return value;
		}

		T exchange(T desired, std::memory_order /*order*/ = std::memory_order_seq_cst) noexcept
		{
			const T previous = value;
			value = desired;
			return previous;
		}
		
		T operator ++ () noexcept
		{
			return ++value;
		}

		T operator -- () noexcept
		{
			return --value;
		}

		T fetch_add(T arg, std::memory_order /*order*/ = std::memory_order_seq_cst) noexcept
		{
			const T previous = value;
			value += arg;
			return previous;
		}

		T fetch_sub(T arg, std::memory_order /*order*/ = std::memory_order_seq_cst) noexcept
		{
			const T previous = value;
			value -= arg;
			return previous;
		}

		T value;
	};

	struct ConditionVariable
	{
		void notify_one() noexcept
		{
		}
		
		template <class Predicate>
		void wait(std::unique_lock<std::mutex> & /*lock*/, Predicate /*pred*/)
		{
		}
		
		template <class Rep, class Period, class Predicate>
		bool wait_for(std::unique_lock<std::mutex> & /*lock*/,
				const std::chrono::duration<Rep, Period> & /*rel_time*/,
				Predicate /*pred*/
			)
		{
			return true;
		}
	};
};

struct ArgumentPassingAutoDetect
{
	enum {
		canIncludeEventType = true,
		canExcludeEventType = true
	};
};

struct ArgumentPassingIncludeEvent
{
	enum {
		canIncludeEventType = true,
		canExcludeEventType = false
	};
};

struct ArgumentPassingExcludeEvent
{
	enum {
		canIncludeEventType = false,
		canExcludeEventType = true
	};
};

struct DefaultPolicies
{
};

template <template <typename> class ...Mixins>
struct MixinList
{
};

#include "internal/eventpolicies_i.h"


} //namespace eventpp


#endif
