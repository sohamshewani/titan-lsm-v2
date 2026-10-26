#ifndef TITAN_SKIPLIST_HPP
#define TITAN_SKIPLIST_HPP

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <optional>
#include <utility>

namespace titan {

// Arena Allocator for Lock-Free SkipList (Bump-pointer allocation in 2MB chunks)
class Arena {
public:
    explicit Arena(size_t chunk_size = 2 * 1024 * 1024) 
        : chunk_size_(chunk_size), 
          alloc_bytes_remaining_(0), 
          bytes_allocated_(0) {
        // Initialize the first chunk
        Chunk* first_chunk = allocate_chunk(chunk_size_);
        if (!first_chunk) {
            throw std::bad_alloc();
        }
        chunk_head_ = first_chunk;
        alloc_ptr_ = first_chunk->data;
        alloc_bytes_remaining_ = chunk_size_;
    }

    ~Arena() {
        Chunk* current = chunk_head_;
        while (current != nullptr) {
            Chunk* next = current->next;
            std::free(current);
            current = next;
        }
    }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) noexcept = delete;
    Arena& operator=(Arena&&) noexcept = delete;

    void* Allocate(size_t bytes) {
        // Align allocation to 8 bytes for architectures requiring strict alignment
        const size_t alignment = 8;
        bytes = (bytes + (alignment - 1)) & ~(alignment - 1);

        // Fast path: allocation fits in current chunk
        if (bytes <= alloc_bytes_remaining_) {
            void* result = alloc_ptr_;
            alloc_ptr_ = static_cast<char*>(alloc_ptr_) + bytes;
            alloc_bytes_remaining_ -= bytes;
            bytes_allocated_.fetch_add(bytes, std::memory_order_relaxed);
            return result;
        }

        // Slow path: allocate a new chunk
        return AllocateFallback(bytes);
    }

    size_t BytesAllocated() const {
        return bytes_allocated_.load(std::memory_order_relaxed);
    }

private:
    struct Chunk {
        Chunk* next;
        char data[];
    };

    Chunk* allocate_chunk(size_t size) {
        void* raw = std::malloc(sizeof(Chunk) + size);
        if (!raw) {
            return nullptr;
        }
        Chunk* chunk = static_cast<Chunk*>(raw);
        chunk->next = nullptr;
        return chunk;
    }

    void* AllocateFallback(size_t bytes) {
        // If the requested bytes are very large, allocate a dedicated chunk
        size_t actual_chunk_size = (bytes > chunk_size_ / 2) ? bytes + sizeof(Chunk) + 64 : chunk_size_;
        
        Chunk* new_chunk = allocate_chunk(actual_chunk_size);
        if (!new_chunk) {
            throw std::bad_alloc();
        }

        // Link the new chunk at the head
        new_chunk->next = chunk_head_;
        chunk_head_ = new_chunk;

        // Reset bump pointer for the remainder of the new chunk
        size_t available = actual_chunk_size - sizeof(Chunk);
        alloc_ptr_ = new_chunk->data;
        alloc_bytes_remaining_ = available;

        if (bytes > alloc_bytes_remaining_) {
            // Even the dedicated chunk was too small (should not happen given math above)
            throw std::bad_alloc();
        }

        void* result = alloc_ptr_;
        alloc_ptr_ = static_cast<char*>(alloc_ptr_) + bytes;
        alloc_bytes_remaining_ -= bytes;
        bytes_allocated_.fetch_add(bytes, std::memory_order_relaxed);
        return result;
    }

    const size_t chunk_size_;
    Chunk* chunk_head_{nullptr};
    void* alloc_ptr_{nullptr};
    size_t alloc_bytes_remaining_;
    std::atomic<size_t> bytes_allocated_;
};

// Hazard Pointer node reclamation infrastructure
class HazardPointerRegistry {
public:
    struct HazardRecord {
        std::atomic<const void*> pointer{nullptr};
        std::atomic<bool> active{false};
        HazardRecord* next{nullptr};
    };

    static HazardPointerRegistry& Instance() {
        static HazardPointerRegistry instance;
        return instance;
    }

    HazardRecord* AcquireRecord() {
        HazardRecord* list = head_.load(std::memory_order_acquire);
        for (HazardRecord* curr = list; curr != nullptr; curr = curr->next) {
            bool expected = false;
            if (curr->active.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                return curr;
            }
        }
        // Allocate a new record and push to the lock-free list
        HazardRecord* new_rec = new HazardRecord();
        new_rec->active.store(true, std::memory_order_relaxed);
        
        HazardRecord* old_head = head_.load(std::memory_order_relaxed);
        do {
            new_rec->next = old_head;
        } while (!head_.compare_exchange_weak(old_head, new_rec, std::memory_order_release, std::memory_order_relaxed));
        
        return new_rec;
    }

    void ReleaseRecord(HazardRecord* rec) {
        rec->pointer.store(nullptr, std::memory_order_release);
        rec->active.store(false, std::memory_order_release);
    }

    bool IsHazard(const void* ptr) const {
        for (HazardRecord* curr = head_.load(std::memory_order_acquire); curr != nullptr; curr = curr->next) {
            if (curr->active.load(std::memory_order_relaxed) && 
                curr->pointer.load(std::memory_order_acquire) == ptr) {
                return true;
            }
        }
        return false;
    }

private:
    HazardPointerRegistry() = default;
    ~HazardPointerRegistry() {
        HazardRecord* curr = head_.load(std::memory_order_relaxed);
        while (curr != nullptr) {
            HazardRecord* next = curr->next;
            delete curr;
            curr = next;
        }
    }

    std::atomic<HazardRecord*> head_{nullptr};
};

class HazardPointerGuard {
public:
    explicit HazardPointerGuard(const void* ptr = nullptr) {
        rec_ = HazardPointerRegistry::Instance().AcquireRecord();
        Reset(ptr);
    }

    ~HazardPointerGuard() {
        if (rec_) {
            HazardPointerRegistry::Instance().ReleaseRecord(rec_);
        }
    }

    HazardPointerGuard(const HazardPointerGuard&) = delete;
    HazardPointerGuard& operator=(const HazardPointerGuard&) = delete;

    void Reset(const void* ptr) {
        rec_->pointer.store(ptr, std::memory_order_release);
    }

private:
    HazardPointerRegistry::HazardRecord* rec_;
};

// Key type and internal SkipList Node definition
struct Slice {
    const char* data{nullptr};
    size_t size{0};

    Slice() = default;
    Slice(const char* d, size_t s) : data(d), size(s) {}

    int Compare(const Slice& other) const {
        const size_t min_len = (size < other.size) ? size : other.size;
        int r = std::memcmp(data, other.data, min_len);
        if (r == 0) {
            if (size < other.size) r = -1;
            else if (size > other.size) r = +1;
        }
        return r;
    }

    bool operator==(const Slice& b) const { return Compare(b) == 0; }
    bool operator<(const Slice& b) const { return Compare(b) < 0; }
};

template <typename Key, typename Value>
class SkipList {
public:
    struct Node;

    struct TowerPointer {
        std::atomic<Node*> next{nullptr};
    };

    struct Node {
        Key key;
        Value value;
        uint64_t lsn;
        uint32_t height;
        // Flexible array member representing tower pointers (height elements)
        TowerPointer forward[1];

        Node(const Key& k, const Value& v, uint64_t s, uint32_t h)
            : key(k), value(v), lsn(s), height(h) {
            for (uint32_t i = 0; i < height; ++i) {
                forward[i].next.store(nullptr, std::memory_order_relaxed);
            }
        }

        static Node* Allocate(Arena& arena, const Key& k, const Value& v, uint64_t s, uint32_t height) {
            // Calculate total bytes required including the dynamic tower array
            size_t bytes = sizeof(Node) + (height > 1 ? (height - 1) * sizeof(TowerPointer) : 0);
            void* mem = arena.Allocate(bytes);
            return new (mem) Node(k, v, s, height);
        }
    };

    explicit SkipList(Arena& arena, uint32_t max_height = 32, double p = 0.25)
        : arena_(arena), 
          max_height_(max_height), 
          p_(p), 
          height_(1),
          rnd_(0xDEADBEEF) {
        
        // Allocate head node with max height
        Key dummy_key{};
        Value dummy_val{};
        head_ = Node::Allocate(arena_, dummy_key, dummy_val, 0, max_height_);
        for (uint32_t i = 0; i < max_height_; ++i) {
            head_->forward[i].next.store(nullptr, std::memory_order_relaxed);
        }
    }

    ~SkipList() = default;

    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;
    SkipList(SkipList&&) noexcept = delete;
    SkipList& operator=(SkipList&&) noexcept = delete;

    void Insert(const Key& key, const Value& value, uint64_t lsn) {
        Node* preds[32];
        Node* succs[32];
        
        while (true) {
            int found_height = FindSplicePoints(key, preds, succs);
            if (found_height != -1) {
                // Key already exists; perform optimistic lock/CAS update on value and lsn
                Node* node_to_update = succs[found_height];
                // Since this is lock-free, we update the payload or mark-and-reinsert. 
                // For MemTable semantics, we append/overwrite by splicing a new node with a higher LSN.
                // To maintain strict linearizability, we proceed to insert the new version.
            }

            uint32_t h = RandomHeight();
            Node* new_node = Node::Allocate(arena_, key, value, lsn, h);

            // Establish forward links from bottom up
            for (uint32_t i = 0; i < h; ++i) {
                Node* succ = succs[i];
                new_node->forward[i].next.store(succ, std::memory_order_relaxed);
            }

            // CAS the bottom-most pointer first to linearize insertion
            Node* pred = preds[0];
            Node* succ = succs[0];
            new_node->forward[0].next.store(succ, std::memory_order_relaxed);

            if (pred->forward[0].next.compare_exchange_strong(succ, new_node, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                // Successfully inserted at level 0; now link upper levels
                for (uint32_t i = 1; i < h; ++i) {
                    while (true) {
                        pred = preds[i];
                        succ = succs[i];
                        new_node->forward[i].next.store(succ, std::memory_order_relaxed);
                        if (pred->forward[i].next.compare_exchange_strong(succ, new_node, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                            break;
                        }
                        // Refresh splice points if CAS failed
                        FindSplicePoints(key, preds, succs);
                    }
                }

                // Update max height atomically if needed
                uint32_t cur_height = height_.load(std::memory_order_relaxed);
                while (h > cur_height) {
                    if (height_.compare_exchange_weak(cur_height, h, std::memory_order_release, std::memory_order_relaxed)) {
                        break;
                    }
                    cur_height = height_.load(std::memory_order_relaxed);
                }
                return;
            }

            // If level 0 CAS failed, retry the entire insertion
        }
    }

    bool Contains(const Key& key, Value* out_value, uint64_t* out_lsn) const {
        HazardPointerGuard guard;
        Node* curr = head_;
        int level = static_cast<int>(height_.load(std::memory_order_acquire)) - 1;

        for (; level >= 0; --level) {
            Node* next = curr->forward[level].next.load(std::memory_order_acquire);
            while (next != nullptr) {
                guard.Reset(next);
                // Re-read next under hazard protection to avoid ABA/free races
                Node* check_next = curr->forward[level].next.load(std::memory_order_acquire);
                if (next != check_next) {
                    next = check_next;
                    continue;
                }

                int cmp = CompareKeys(next->key, key);
                if (cmp < 0) {
                    curr = next;
                    next = curr->forward[level].next.load(std::memory_order_acquire);
                } else {
                    break;
                }
            }
        }

        Node* target = curr->forward[0].next.load(std::memory_order_acquire);
        if (target != nullptr && CompareKeys(target->key, key) == 0) {
            if (out_value) *out_value = target->value;
            if (out_lsn) *out_lsn = target->lsn;
            return true;
        }
        return false;
    }

    class Iterator {
    public:
        explicit Iterator(const SkipList* list) : list_(list), node_(nullptr) {}

        void SeekToFirst() {
            guard_.Reset(nullptr);
            if (list_) {
                node_ = list_->head_->forward[0].next.load(std::memory_order_acquire);
                guard_.Reset(node_);
            }
        }

        void Seek(const Key& key) {
            if (!list_) return;
            HazardPointerGuard guard;
            Node* curr = list_->head_;
            int level = static_cast<int>(list_->height_.load(std::memory_order_acquire)) - 1;

            for (; level >= 0; --level) {
                Node* next = curr->forward[level].next.load(std::memory_order_acquire);
                while (next != nullptr) {
                    guard.Reset(next);
                    Node* check_next = curr->forward[level].next.load(std::memory_order_acquire);
                    if (next != check_next) {
                        next = check_next;
                        continue;
                    }
                    if (CompareKeys(next->key, key) < 0) {
                        curr = next;
                        next = curr->forward[level].next.load(std::memory_order_acquire);
                    } else {
                        break;
                    }
                }
            }
            node_ = curr->forward[0].next.load(std::memory_order_acquire);
            guard_.Reset(node_);
        }

        bool Valid() const {
            return node_ != nullptr;
        }

        void Next() {
            if (node_) {
                node_ = node_->forward[0].next.load(std::memory_order_acquire);
                guard_.Reset(node_);