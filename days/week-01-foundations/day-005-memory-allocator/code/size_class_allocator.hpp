// size_class_allocator.hpp
//
// Day 05 的教学实现：尺寸分级 + slab + thread-local cache 的分配器。
//
// 对照 jemalloc / tcmalloc 的核心结构：
//   - size class：把请求向上取整到固定尺寸，减少碎片和分支；
//   - slab：一次向 OS 申请一批块，切成固定大小；
//   - thread cache：每线程每尺寸一条无锁（本实现用普通链表）空闲链表，
//     减少全局锁竞争；
//   - central free list：线程缓存不足/溢出时与全局交换；
//   - large allocation：超过最大 size class 时回退到 operator new。
//
// 本实现刻意省略：
//   - 页/arena/NUMA 管理；
//   - 真正的 lock-free 线程缓存；
//   - 内存回收给 OS、碎片整理；
//   - 对齐超过 max_align_t 的分配；
//   - 安全的双重释放检测（只做 magic 校验，错误指针仍可能 UB）。

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace archdesign {

class SizeClassAllocator {
public:
    static constexpr std::size_t kHeaderSize = 16;
    static constexpr std::size_t kMaxSmall = 4096;
    static constexpr std::size_t kBatch = 32;
    static constexpr std::size_t kCacheLimit = 128;
    static constexpr std::size_t kSlabBytes = 64 * 1024;
    static constexpr std::uint32_t kLargeClass = 0xFFFFFFFFu;

    struct Stats {
        std::uint64_t allocations = 0;
        std::uint64_t deallocations = 0;
        std::uint64_t large_allocations = 0;
        std::uint64_t active = 0;
        std::uint64_t slabs = 0;
    };

    static SizeClassAllocator& instance() {
        // 故意不析构：线程局部缓存的析构可能发生在静态对象析构之后。
        static SizeClassAllocator* allocator = new SizeClassAllocator();
        return *allocator;
    }

    void* allocate(std::size_t size) {
        if (size == 0) {
            size = 1;
        }
        if (size > kMaxSmall) {
            return allocate_large(size);
        }

        const auto index = class_index(size);
        auto& cache = thread_cache();
        if (cache.heads[index] == nullptr) {
            refill(cache, index);
        }

        FreeNode* node = cache.heads[index];
        cache.heads[index] = node->next;
        --cache.counts[index];

        auto* header = reinterpret_cast<Header*>(
            reinterpret_cast<char*>(node) - kHeaderSize);
        header->magic = kMagic;
        header->class_index = static_cast<std::uint32_t>(index);
        header->requested = static_cast<std::uint32_t>(size);

        ++allocations_;
        ++active_;
        return node;
    }

    void deallocate(void* pointer) {
        if (pointer == nullptr) {
            return;
        }

        auto* header = reinterpret_cast<Header*>(
            static_cast<char*>(pointer) - kHeaderSize);
        if (header->magic != kMagic) {
            std::abort();  // 无效指针/重复释放：直接失败，避免继续破坏堆
        }

        const auto index = header->class_index;
        if (index == kLargeClass) {
            --active_;
            ++deallocations_;
            ::operator delete(header);
            return;
        }
        if (index >= kNumClasses) {
            std::abort();
        }

        auto& cache = thread_cache();
        auto* node = static_cast<FreeNode*>(pointer);
        node->next = cache.heads[index];
        cache.heads[index] = node;
        ++cache.counts[index];

        --active_;
        ++deallocations_;

        if (cache.counts[index] > kCacheLimit) {
            flush_class(cache, index, cache.counts[index] / 2);
        }
    }

    [[nodiscard]] Stats stats() const {
        Stats result;
        result.allocations = allocations_.load(std::memory_order_relaxed);
        result.deallocations = deallocations_.load(std::memory_order_relaxed);
        result.large_allocations = large_allocations_.load(std::memory_order_relaxed);
        result.active = active_.load(std::memory_order_relaxed);
        result.slabs = slab_count_.load(std::memory_order_relaxed);
        return result;
    }

private:
    static constexpr std::uint64_t kMagic = 0x4152434844455349ull;  // "ARCHDESI"

    struct Header {
        std::uint64_t magic;
        std::uint32_t class_index;
        std::uint32_t requested;
    };
    static_assert(sizeof(Header) == kHeaderSize, "header must be 16 bytes");

    struct FreeNode {
        FreeNode* next;
    };

    struct Slab {
        void* base;
        std::size_t bytes;
        Slab* next;
    };

    struct ClassState {
        std::mutex mutex;
        FreeNode* free_list = nullptr;
        std::size_t free_count = 0;
        std::size_t block_size = 0;
        Slab* slabs = nullptr;
    };

    static constexpr std::size_t kNumClasses = 17;
    static constexpr std::array<std::size_t, kNumClasses> kSizes = {
        16,  32,  48,  64,  80,   96,   128, 160, 192,
        256, 320, 384, 512, 768, 1024, 2048, 4096,
    };

    struct ThreadCache {
        std::array<FreeNode*, kNumClasses> heads;
        std::array<std::uint32_t, kNumClasses> counts;

        ThreadCache() : heads{}, counts{} {}

        ~ThreadCache() {
            SizeClassAllocator::instance().flush_cache(*this);
        }
    };

    static ThreadCache& thread_cache() {
        // 把较大的 ThreadCache 放在堆上，TLS 里只保存一个很小的 holder。
        // 某些平台上 TLS 析构阶段会先覆盖/回收对象存储，直接放 ThreadCache
        // 可能在析构时读到损坏的链表；holder 缩小了这个生命周期窗口。
        struct CacheHolder {
            ThreadCache* cache = new ThreadCache();
            ~CacheHolder() { delete cache; }
        };
        static thread_local CacheHolder holder;
        return *holder.cache;
    }

    SizeClassAllocator() {
        for (std::size_t i = 0; i < kNumClasses; ++i) {
            classes_[i].block_size = round_up(kHeaderSize + kSizes[i], 16);
        }
    }

    static constexpr std::size_t round_up(std::size_t value,
                                          std::size_t alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    static std::size_t class_index(std::size_t size) {
        for (std::size_t i = 0; i < kNumClasses; ++i) {
            if (size <= kSizes[i]) {
                return i;
            }
        }
        return kNumClasses;  // unreachable for size <= kMaxSmall
    }

    void* allocate_large(std::size_t size) {
        if (size > static_cast<std::size_t>(
                       std::numeric_limits<std::uint32_t>::max())) {
            throw std::bad_alloc();
        }
        if (size > static_cast<std::size_t>(-1) - kHeaderSize) {
            throw std::bad_alloc();
        }

        void* raw = ::operator new(kHeaderSize + size);
        auto* header = static_cast<Header*>(raw);
        header->magic = kMagic;
        header->class_index = kLargeClass;
        header->requested = static_cast<std::uint32_t>(size);

        ++large_allocations_;
        ++allocations_;
        ++active_;
        return static_cast<char*>(raw) + kHeaderSize;
    }

    void refill(ThreadCache& cache, std::size_t index) {
        std::lock_guard<std::mutex> lock(classes_[index].mutex);
        auto& state = classes_[index];

        if (state.free_list == nullptr) {
            add_slab(index);
        }

        const auto count = std::min(kBatch, state.free_count);
        for (std::size_t i = 0; i < count; ++i) {
            FreeNode* node = state.free_list;
            state.free_list = node->next;
            --state.free_count;

            node->next = cache.heads[index];
            cache.heads[index] = node;
            ++cache.counts[index];
        }
    }

    void add_slab(std::size_t index) {
        auto& state = classes_[index];
        const std::size_t count =
            std::max<std::size_t>(1, kSlabBytes / state.block_size);
        const std::size_t bytes = count * state.block_size;

        void* base = ::operator new(bytes);
        auto* slab = static_cast<Slab*>(::operator new(sizeof(Slab)));
        slab->base = base;
        slab->bytes = bytes;
        slab->next = state.slabs;
        state.slabs = slab;
        ++slab_count_;

        for (std::size_t i = 0; i < count; ++i) {
            auto* block = static_cast<char*>(base) + i * state.block_size;
            auto* header = reinterpret_cast<Header*>(block);
            header->magic = kMagic;
            header->class_index = static_cast<std::uint32_t>(index);
            header->requested = 0;

            auto* node = reinterpret_cast<FreeNode*>(block + kHeaderSize);
            node->next = state.free_list;
            state.free_list = node;
            ++state.free_count;
        }
    }

    void flush_class(ThreadCache& cache, std::size_t index, std::size_t count) {
        FreeNode* list = nullptr;
        for (std::size_t i = 0; i < count; ++i) {
            FreeNode* node = cache.heads[index];
            cache.heads[index] = node->next;
            --cache.counts[index];
            node->next = list;
            list = node;
        }

        std::lock_guard<std::mutex> lock(classes_[index].mutex);
        auto& state = classes_[index];
        while (list != nullptr) {
            FreeNode* next = list->next;
            list->next = state.free_list;
            state.free_list = list;
            ++state.free_count;
            list = next;
        }
    }

    void flush_cache(ThreadCache& cache) {
        for (std::size_t i = 0; i < kNumClasses; ++i) {
            if (cache.heads[i] != nullptr) {
                flush_class(cache, i, cache.counts[i]);
            }
        }
    }

    std::array<ClassState, kNumClasses> classes_;
    std::atomic<std::uint64_t> allocations_{0};
    std::atomic<std::uint64_t> deallocations_{0};
    std::atomic<std::uint64_t> large_allocations_{0};
    std::atomic<std::uint64_t> active_{0};
    std::atomic<std::uint64_t> slab_count_{0};
};

} // namespace archdesign
