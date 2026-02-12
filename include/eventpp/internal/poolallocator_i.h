// eventpp library - ARM-Linux optimization extension
// Pool allocator for std::list to eliminate per-node heap allocation.
//
// Design:
// - Static per-type pool: all PoolAllocator<T> instances share one pool,
//   so they always compare equal. This makes std::list::splice() well-defined
//   per C++14 [list.ops] 23.3.5.5.
// - Pre-allocated fixed-capacity slab of nodes.
// - Free list for O(1) alloc/dealloc within pool.
// - Falls back to ::operator new when pool is exhausted.
// - Thread-safe via SpinLock (minimal overhead vs mutex).

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

namespace eventpp {

namespace internal_ {

// SpinLock for pool operations (same design as eventpolicies.h)
struct PoolSpinLock
{
	void lock() noexcept {
		while(locked.test_and_set(std::memory_order_acquire)) {
#if defined(__aarch64__) || defined(__arm__)
			__asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
			__builtin_ia32_pause();
#endif
		}
	}

	void unlock() noexcept {
		locked.clear(std::memory_order_release);
	}

private:
	std::atomic_flag locked = ATOMIC_FLAG_INIT;
};

// Fixed-capacity node pool with free list.
// One static instance per type T, shared by all PoolAllocator<T>.
template <typename T, size_t Capacity>
class NodePool
{
public:
	static NodePool & instance() {
		static NodePool pool;
		return pool;
	}

	T * allocate() noexcept {
		lock_.lock();
		if(free_head_ != nullptr) {
			FreeNode * node = free_head_;
			free_head_ = node->next;
			lock_.unlock();
			return reinterpret_cast<T *>(node);
		}
		lock_.unlock();
		// Pool exhausted, fall back to heap
		return static_cast<T *>(::operator new(sizeof(T), std::nothrow));
	}

	void deallocate(T * ptr) noexcept {
		// Check if ptr is within our slab
		auto * raw = reinterpret_cast<unsigned char *>(ptr);
		if(raw >= slab_ && raw < slab_ + sizeof(Slot) * Capacity) {
			FreeNode * node = reinterpret_cast<FreeNode *>(ptr);
			lock_.lock();
			node->next = free_head_;
			free_head_ = node;
			lock_.unlock();
		}
		else {
			// Was allocated from heap fallback
			::operator delete(ptr);
		}
	}

private:
	NodePool() : free_head_(nullptr) {
		// Build free list from slab
		for(size_t i = 0; i < Capacity; ++i) {
			FreeNode * node = reinterpret_cast<FreeNode *>(&slab_[i * sizeof(Slot)]);
			node->next = free_head_;
			free_head_ = node;
		}
	}

	~NodePool() = default;

	NodePool(const NodePool &) = delete;
	NodePool & operator=(const NodePool &) = delete;

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

	alignas(SlotAlign) unsigned char slab_[sizeof(Slot) * Capacity];
	FreeNode * free_head_;
	PoolSpinLock lock_;
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


} // namespace eventpp

#endif // POOLALLOCATOR_I_H_EVENTPP
