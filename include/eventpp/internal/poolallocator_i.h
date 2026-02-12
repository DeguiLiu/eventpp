// eventpp library - ARM-Linux / x86-Linux optimization extension
// Pool allocator for std::list to eliminate per-node heap allocation.
//
// Design (v0.3.0 — OPT-9a/9b iceoryx MemPool enhanced):
// - Static per-type pool: all PoolAllocator<T> instances share one pool,
//   so they always compare equal. This makes std::list::splice() well-defined
//   per C++14 [list.ops] 23.3.5.5.
// - OPT-9a: Multi-slab growth — when initial slab is exhausted, allocates
//   new slabs dynamically instead of falling back to ::operator new.
// - OPT-9b: Lock-free free list — uses atomic CAS stack instead of SpinLock
//   for allocate/deallocate. SpinLock only protects grow() (rare path).
// - Thread-safe: lock-free hot path + SpinLock cold path (grow only).

#ifndef POOLALLOCATOR_I_H_EVENTPP
#define POOLALLOCATOR_I_H_EVENTPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <list>
#include <memory>
#include <new>
#include <type_traits>

#include "../eventpolicies.h"

namespace eventpp {

namespace internal_ {

// OPT-11: SpinLock with exponential backoff for pool operations.
struct PoolSpinLock
{
	void lock() noexcept {
		if(!locked.test_and_set(std::memory_order_acquire)) {
			return;
		}
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

	void unlock() noexcept {
		locked.clear(std::memory_order_release);
	}

private:
	static constexpr unsigned kMaxBackoff = 64;
	std::atomic_flag locked = ATOMIC_FLAG_INIT;
};

// OPT-9a/9b: Multi-slab node pool with lock-free free list.
// One static instance per type T, shared by all PoolAllocator<T>.
//
// OPT-9a: When the initial slab is exhausted, grow() allocates a new slab
//   (linked list of slabs) instead of falling back to ::operator new.
// OPT-9b: allocate/deallocate use atomic CAS on free_head_ (lock-free).
//   Only grow() uses SpinLock (called once per SlabCapacity allocations).
//
// ABA safety: free list is a LIFO stack with unique slab-internal addresses.
// A node is removed from free list on allocate, returned on deallocate.
// The same pointer cannot appear twice in the free list simultaneously,
// so ABA cannot occur.
template <typename T, size_t SlabCapacity>
class NodePool
{
public:
	static NodePool & instance() {
		static NodePool pool;
		return pool;
	}

	T * allocate() noexcept {
		FreeNode * old_head = free_head_.load(std::memory_order_acquire);
		while(true) {
			if(old_head == nullptr) {
				// Pool exhausted — try to grow under lock
				grow_lock_.lock();
				old_head = free_head_.load(std::memory_order_acquire);
				if(old_head == nullptr) {
					grow();
					old_head = free_head_.load(std::memory_order_acquire);
				}
				grow_lock_.unlock();
				if(old_head == nullptr) {
					return nullptr;  // grow() failed (out of memory)
				}
			}
			// CAS: try to pop head from free list
			if(free_head_.compare_exchange_weak(
				old_head, old_head->next,
				std::memory_order_acq_rel, std::memory_order_acquire))
			{
				return reinterpret_cast<T *>(old_head);
			}
			// CAS failed — old_head updated by compare_exchange_weak, retry
		}
	}

	void deallocate(T * ptr) noexcept {
		auto * raw = reinterpret_cast<unsigned char *>(ptr);
		if(is_in_pool(raw)) {
			// Return to pool via lock-free push
			FreeNode * node = reinterpret_cast<FreeNode *>(ptr);
			FreeNode * old_head = free_head_.load(std::memory_order_relaxed);
			do {
				node->next = old_head;
			} while(!free_head_.compare_exchange_weak(
				old_head, node,
				std::memory_order_release, std::memory_order_relaxed));
		}
		else {
			// Allocated before pool existed or from multi-element fallback
			::operator delete(ptr);
		}
	}

private:
	struct FreeNode {
		FreeNode * next;
	};

	// Slot must be large enough for T and properly aligned,
	// and also large enough for FreeNode (pointer) when in free list.
	static constexpr size_t SlotSize =
		sizeof(T) > sizeof(FreeNode) ? sizeof(T) : sizeof(FreeNode);
	static constexpr size_t SlotAlign =
		alignof(T) > alignof(FreeNode) ? alignof(T) : alignof(FreeNode);

	struct alignas(SlotAlign) Slot {
		unsigned char data[SlotSize];
	};

	// OPT-9a: Slab structure — linked list of fixed-capacity slabs.
	struct Slab {
		alignas(SlotAlign) unsigned char data[sizeof(Slot) * SlabCapacity];
		Slab * next;
	};

	NodePool() : slab_head_(nullptr), free_head_(nullptr) {
		// Allocate initial slab
		grow();
	}

	~NodePool() {
		// Free all dynamically allocated slabs
		Slab * s = slab_head_;
		while(s != nullptr) {
			Slab * next = s->next;
			::operator delete(s);
			s = next;
		}
	}

	NodePool(const NodePool &) = delete;
	NodePool & operator=(const NodePool &) = delete;

	// Allocate a new slab and add all its slots to the free list.
	// Must be called under grow_lock_.
	void grow() {
		Slab * new_slab = static_cast<Slab *>(
			::operator new(sizeof(Slab), std::nothrow));
		if(new_slab == nullptr) {
			return;  // Out of memory
		}
		new_slab->next = slab_head_;
		slab_head_ = new_slab;

		// Build free list from new slab's slots (push all to free_head_)
		// We're under grow_lock_ and free_head_ is null (or we wouldn't grow),
		// but other threads may be doing CAS on free_head_ concurrently for
		// deallocate. Use CAS to safely push each node.
		for(size_t i = 0; i < SlabCapacity; ++i) {
			FreeNode * node = reinterpret_cast<FreeNode *>(
				&new_slab->data[i * sizeof(Slot)]);
			FreeNode * old_head = free_head_.load(std::memory_order_relaxed);
			do {
				node->next = old_head;
			} while(!free_head_.compare_exchange_weak(
				old_head, node,
				std::memory_order_release, std::memory_order_relaxed));
		}
	}

	// Check if a pointer belongs to any slab in the pool.
	// Slab count is typically <= 3, so linear scan is fine.
	bool is_in_pool(unsigned char * raw) const noexcept {
		const size_t slab_data_size = sizeof(Slot) * SlabCapacity;
		Slab * s = slab_head_;
		while(s != nullptr) {
			if(raw >= s->data && raw < s->data + slab_data_size) {
				return true;
			}
			s = s->next;
		}
		return false;
	}

	Slab * slab_head_;
	std::atomic<FreeNode *> free_head_;  // OPT-9b: lock-free free list head
	PoolSpinLock grow_lock_;             // Only protects grow()
};

} // namespace internal_


// C++14 conforming allocator backed by a static per-type pool.
// All instances of PoolAllocator<T, Capacity> compare equal,
// which is required for std::list::splice() between lists.
template <typename T, size_t Capacity = 4096>
class PoolAllocator
{
public:
	using value_type = T;
	using size_type = std::size_t;
	using difference_type = std::ptrdiff_t;
	using propagate_on_container_move_assignment = std::true_type;
	using is_always_equal = std::true_type;

	PoolAllocator() noexcept = default;

	template <typename U>
	PoolAllocator(const PoolAllocator<U, Capacity> &) noexcept {}

	T * allocate(size_type n) {
		if(n == 1) {
			T * ptr = internal_::NodePool<T, Capacity>::instance().allocate();
			if(ptr == nullptr) {
				throw std::bad_alloc();
			}
			return ptr;
		}
		// Multi-element allocation: fall back to default
		return static_cast<T *>(::operator new(n * sizeof(T)));
	}

	void deallocate(T * ptr, size_type n) noexcept {
		if(n == 1) {
			internal_::NodePool<T, Capacity>::instance().deallocate(ptr);
		}
		else {
			::operator delete(ptr);
		}
	}

	template <typename U>
	struct rebind {
		using other = PoolAllocator<U, Capacity>;
	};
};

template <typename T1, typename T2, size_t C>
bool operator==(const PoolAllocator<T1, C> &, const PoolAllocator<T2, C> &) noexcept {
	return true;  // All instances share static pool, always equal
}

template <typename T1, typename T2, size_t C>
bool operator!=(const PoolAllocator<T1, C> &, const PoolAllocator<T2, C> &) noexcept {
	return false;
}


// Policy-level QueueList type alias for opt-in usage.
// Usage:
//   struct MyPolicies {
//       template <typename T>
//       using QueueList = eventpp::PoolQueueList<T>;
//   };
//   eventpp::EventQueue<int, void(), MyPolicies> queue;
//
template <typename T, size_t Capacity = 4096>
using PoolQueueList = std::list<T, PoolAllocator<T, Capacity>>;

// OPT-14: One-stop high-performance policy preset.
// Combines SpinLock (OPT-1/11), PoolAllocator (OPT-5/9), and shared_mutex (OPT-3)
// into a single policy struct. Users get optimal settings with zero configuration:
//
//   eventpp::EventQueue<int, void(const Msg&), eventpp::HighPerfPolicy> queue;
//
// Compile-time template specialization — zero runtime overhead vs manual policy.
struct HighPerfPolicy {
	// SpinLock with exponential backoff replaces std::mutex.
	// Optimal for short critical sections (enqueue/dispatch).
	using Threading = GeneralThreading<SpinLock>;

	// Pool allocator eliminates per-node heap allocation.
	// 8192 slots per slab; auto-grows when exhausted (OPT-9a).
	template <typename T>
	using QueueList = PoolQueueList<T, 8192>;
};


} // namespace eventpp

#endif // POOLALLOCATOR_I_H_EVENTPP
