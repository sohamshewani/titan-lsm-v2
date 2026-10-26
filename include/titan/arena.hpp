#ifndef TITAN_ARENA_HPP
#define TITAN_ARENA_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <vector>
#include <mutex>
#include <utility>

namespace titan {

class Arena {
public:
    explicit Arena(std::size_t block_size = 2 * 1024 * 1024)
        : block_size_(block_size),
          current_alloc_bytes_(0),
          total_memory_usage_(0) {
        // Pre-allocate the first chunk to guarantee immediate readiness.
        AllocateBlock(block_size_);
    }

    ~Arena() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (char* block : blocks_) {
            std::free(block);
        }
    }

    // Delete copy and move semantics to maintain allocator integrity.
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) = delete;
    Arena& operator=(Arena&&) = delete;

    /**
     * @brief Allocates bytes with the specified alignment thread-safely.
     * 
     * Uses a lock-free atomic load/CAS fast-path for high-concurrency bump pointer
     * allocation within the current active block. Falls back to a mutex-protected
     * block exhaustion path when the current block cannot satisfy the request.
     * 
     * @param bytes Number of bytes to allocate.
     * @param alignment Required memory alignment (must be a power of 2).
     * @return Pointer to the allocated memory block.
     */
    void* Allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t)) {
        if (bytes == 0) {
            return nullptr;
        }

        // Fast path: Try atomic bump pointer allocation in the current block.
        while (true) {
            char* current_block = current_block_.load(std::memory_order_acquire);
            if (current_block == nullptr) {
                // Should only happen during race conditions or very early states; fall back to slow path.
                return AllocateFallback(bytes, alignment);
            }

            std::size_t current_offset = current_offset_.load(std::memory_order_relaxed);
            
            // Compute current address and align it upward.
            char* raw_address = current_block + current_offset;
            std::size_t space_left = (current_offset < block_size_) ? (block_size_ - current_offset) : 0;
            
            void* aligned_address = static_cast<void*>(raw_address);
            std::size_t space = space_left;
            
            if (std::align(alignment, bytes, aligned_address, space) == nullptr) {
                // Current block lacks sufficient aligned space; trigger slow path.
                return AllocateFallback(bytes, alignment);
            }

            // Calculate the new offset including alignment padding.
            std::size_t padding = static_cast<char*>(aligned_address) - raw_address;
            std::size_t new_offset = current_offset + padding + bytes;

            if (new_offset <= block_size_) {
                // Attempt atomic CAS update of the bump pointer.
                if (current_offset_.compare_exchange_weak(
                        current_offset, new_offset,
                        std::memory_order_release,
                        std::memory_order_relaxed)) {
                    
                    current_alloc_bytes_.fetch_add(padding + bytes, std::memory_order_relaxed);
                    return aligned_address;
                }
                // CAS failed; retry fast path loop.
            } else {
                // Exceeded block capacity; trigger slow path.
                return AllocateFallback(bytes, alignment);
            }
        }
    }

    /**
     * @brief Type-safe helper to construct an object of type T within the Arena.
     * 
     * @tparam T Type of the object to construct.
     * @tparam Args Constructor argument types.
     * @param args Arguments forwarded to T's constructor.
     * @return Pointer to the constructed object.
     */
    template <typename T, typename... Args>
    T* AllocateObject(Args&&... args) {
        void* mem = Allocate(sizeof(T), alignof(T));
        if (mem == nullptr) {
            throw std::bad_alloc();
        }
        return ::new (mem) T(std::forward<Args>(args)...);
    }

    /**
     * @brief Returns total memory consumed by all allocated blocks from the system.
     */
    std::size_t TotalMemoryUsage() const {
        return total_memory_usage_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Returns total active payload bytes allocated via Allocate().
     */
    std::size_t CurrentAllocBytes() const {
        return current_alloc_bytes_.load(std::memory_order_relaxed);
    }

private:
    const std::size_t block_size_;
    std::atomic<std::size_t> current_alloc_bytes_;
    std::atomic<std::size_t> total_memory_usage_;

    // Mutex protecting block list additions and chunk allocations.
    std::mutex mutex_;
    std::vector<char*> blocks_;

    // Active block pointer and bump pointer offset.
    std::atomic<char*> current_block_{nullptr};
    std::atomic<std::size_t> current_offset_{0};

    /**
     * @brief Allocates a new physical memory chunk via std::aligned_alloc.
     */
    void AllocateBlock(std::size_t block_size) {
        // Enforce 64-byte or cache-line alignment for chunks.
        const std::size_t kChunkAlignment = 64;
        std::size_t alloc_size = (block_size + kChunkAlignment - 1) & ~(kChunkAlignment - 1);
        
        char* new_block = static_cast<char*>(std::aligned_alloc(kChunkAlignment, alloc_size));
        if (new_block == nullptr) {
            throw std::bad_alloc();
        }

        blocks_.push_back(new_block);
        total_memory_usage_.fetch_add(alloc_size, std::memory_order_relaxed);

        current_block_.store(new_block, std::memory_order_release);
        current_offset_.store(0, std::memory_order_relaxed);
    }

    /**
     * @brief Slow-path allocation handler when the current block is exhausted.
     */
    void* AllocateFallback(std::size_t bytes, std::size_t alignment) {
        std::lock_guard<std::mutex> lock(mutex_);

        // Re-check state inside the lock to handle race conditions safely.
        char* current_block = current_block_.load(std::memory_order_relaxed);
        std::size_t current_offset = current_offset_.load(std::memory_order_relaxed);

        char* raw_address = current_block + current_offset;
        std::size_t space_left = (current_offset < block_size_) ? (block_size_ - current_offset) : 0;
        
        void* aligned_address = static_cast<void*>(raw_address);
        std::size_t space = space_left;

        if (current_block != nullptr && std::align(alignment, bytes, aligned_address, space) != nullptr) {
            std::size_t padding = static_cast<char*>(aligned_address) - raw_address;
            std::size_t new_offset = current_offset + padding + bytes;
            if (new_offset <= block_size_) {
                current_offset_.store(new_offset, std::memory_order_relaxed);
                current_alloc_bytes_.fetch_add(padding + bytes, std::memory_order_relaxed);
                return aligned_address;
            }
        }

        // If the requested allocation size is larger than the standard block size,
        // allocate a dedicated block tailored to this allocation to prevent excessive waste.
        std::size_t required_block_size = block_size_;
        if (bytes + alignment > block_size_) {
            required_block_size = bytes + alignment + 64;
        }

        AllocateBlock(required_block_size);

        // Try allocation on the newly allocated block.
        current_block = current_block_.load(std::memory_order_relaxed);
        raw_address = current_block;
        space = required_block_size;
        aligned_address = static_cast<void*>(raw_address);

        if (std::align(alignment, bytes, aligned_address, space) == nullptr) {
            throw std::bad_alloc(); // Alignment calculation failed on newly allocated chunk.
        }

        std::size_t padding = static_cast<char*>(aligned_address) - raw_address;
        std::size_t new_offset = padding + bytes;

        current_offset_.store(new_offset, std::memory_order_relaxed);
        current_alloc_bytes_.fetch_add(new_offset, std::memory_order_relaxed);

        return aligned_address;
    }
};

} // namespace titan

#endif // TITAN_ARENA_HPP