#ifndef TITAN_TYPES_HPP
#define TITAN_TYPES_HPP

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <chrono>
#include <system_error>
#include <optional>
#include <memory>

namespace titan {

// ============================================================================
// SYSTEM-WIDE TYPE ALIASES & CONSTANTS
// ============================================================================

using SequenceNumber = uint64_t;
using Timestamp      = uint64_t;

constexpr SequenceNumber kMaxSequenceNumber = UINT64_MAX;
constexpr SequenceNumber kMinSequenceNumber = 0;

constexpr size_t kDefaultBlockSize      = 4096; // 4KB
constexpr size_t kDefaultArenaChunkSize = 2 * 1024 * 1024; // 2MB
constexpr int    kMaxSkipListHeight     = 32;
constexpr double kSkipListProbability   = 0.25;

// ============================================================================
// ERROR HANDLING HIERARCHY
// ============================================================================

enum class StatusCode : int {
    kOk = 0,
    kNotFound,
    kCorruption,
    kNotSupported,
    kInvalidArgument,
    kIOError,
    kIncomplete,
    kBusy,
    kOutOfMemory
};

class TitanError : public std::exception {
public:
    TitanError(StatusCode code, std::string message)
        : code_(code), message_(std::move(message)) {}

    StatusCode code() const noexcept { return code_; }
    const char* what() const noexcept override { return message_.c_str(); }

    bool ok() const noexcept { return code_ == StatusCode::kOk; }

private:
    StatusCode code_;
    std::string message_;
};

inline std::error_code make_error_code(StatusCode sc) {
    class TitanErrorCategory : public std::error_category {
    public:
        const char* name() const noexcept override { return "titan"; }
        std::string message(int ev) const override {
            switch (static_cast<StatusCode>(ev)) {
                case StatusCode::kOk: return "OK";
                case StatusCode::kNotFound: return "Not Found";
                case StatusCode::kCorruption: return "Data Corruption";
                case StatusCode::kNotSupported: return "Not Supported";
                case StatusCode::kInvalidArgument: return "Invalid Argument";
                case StatusCode::kIOError: return "I/O Error";
                case StatusCode::kIncomplete: return "Incomplete";
                case StatusCode::kBusy: return "Busy";
                case StatusCode::kOutOfMemory: return "Out of Memory";
            }
            return "Unknown Titan Error";
        }
    };
    static const TitanErrorCategory kCategory;
    return std::error_code(static_cast<int>(sc), kCategory);
}

// ============================================================================
// SLICE (ZERO-COPY VIEW)
// ============================================================================

class Slice {
public:
    Slice() noexcept : data_(""), size_(0) {}

    Slice(const char* d, size_t n) noexcept : data_(d), size_(n) {}

    Slice(const std::string& s) noexcept : data_(s.data()), size_(s.size()) {}

    Slice(std::string_view sv) noexcept : data_(sv.data()), size_(sv.size()) {}

    Slice(const char* s) noexcept : data_(s), size_(s ? std::strlen(s) : 0) {}

    const char* data() const noexcept { return data_; }
    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    char operator[](size_t n) const noexcept {
        return data_[n];
    }

    void clear() noexcept {
        data_ = "";
        size_ = 0;
    }

    void remove_prefix(size_t n) noexcept {
        if (n > size_) {
            n = size_;
        }
        data_ += n;
        size_ -= n;
    }

    std::string toString() const {
        return std::string(data_, size_);
    }

    std::string_view toStringView() const noexcept {
        return std::string_view(data_, size_);
    }

    int compare(const Slice& b) const noexcept {
        const size_t min_len = (size_ < b.size_) ? size_ : b.size_;
        int r = std::memcmp(data_, b.data_, min_len);
        if (r == 0) {
            if (size_ < b.size_) r = -1;
            else if (size_ > b.size_) r = +1;
        }
        return r;
    }

    bool starts_with(const Slice& x) const noexcept {
        return ((size_ >= x.size_) && (std::memcmp(data_, x.data_, x.size_) == 0));
    }

    bool ends_with(const Slice& x) const noexcept {
        return ((size_ >= x.size_) && (std::memcmp(data_ + size_ - x.size_, x.data_, x.size_) == 0));
    }

private:
    const char* data_;
    size_t size_;
};

inline bool operator==(const Slice& x, const Slice& y) noexcept {
    return ((x.size() == y.size()) && (x.compare(y) == 0));
}

inline bool operator!=(const Slice& x, const Slice& y) noexcept {
    return !(x == y);
}

inline bool operator<(const Slice& x, const Slice& y) noexcept {
    return (x.compare(y) < 0);
}

inline bool operator<=(const Slice& x, const Slice& y) noexcept {
    return (x.compare(y) <= 0);
}

inline bool operator>(const Slice& x, const Slice& y) noexcept {
    return (x.compare(y) > 0);
}

inline bool operator>=(const Slice& x, const Slice& y) noexcept {
    return (x.compare(y) >= 0);
}

// ============================================================================
// MUTATION & VALUE TYPES
// ============================================================================

enum class ValueType : uint8_t {
    kTypeDeletion = 0x0,
    kTypeValue     = 0x1
};

struct ParsedKey {
    Slice user_key;
    SequenceNumber sequence;
    ValueType type;

    ParsedKey() : sequence(kMinSequenceNumber), type(ValueType::kTypeValue) {}
    ParsedKey(Slice k, SequenceNumber s, ValueType t)
        : user_key(k), sequence(s), type(t) {}
};

// Internal key representation: [user_key][seq_num (8 bytes, big-endian or little-endian with type)][type (1 byte)]
class InternalKey {
public:
    InternalKey() = default;
    InternalKey(Slice user_key, SequenceNumber s, ValueType t) {
        size_t usize = user_key.size();
        rep_.resize(usize + 8 + 1);
        std::memcpy(rep_.data(), user_key.data(), usize);
        
        // Pack sequence number and type into the footer
        uint64_t packed = (s << 8) | static_cast<uint8_t>(t);
        // Store in little endian for internal consistency
        std::memcpy(rep_.data() + usize, &packed, 8);
    }

    Slice user_key() const noexcept {
        return Slice(rep_.data(), rep_.size() - 9);
    }

    SequenceNumber sequence() const noexcept {
        uint64_t packed;
        std::memcpy(&packed, rep_.data() + rep_.size() - 9, 8);
        return packed >> 8;
    }

    ValueType type() const noexcept {
        uint8_t t;
        std::memcpy(&t, rep_.data() + rep_.size() - 1, 1);
        return static_cast<ValueType>(t);
    }

    Slice encode() const noexcept {
        return Slice(rep_.data(), rep_.size());
    }

    void decode(Slice slice) {
        rep_.assign(slice.data(), slice.size());
    }

    int compare(const InternalKey& other) const noexcept {
        Slice u1 = user_key();
        Slice u2 = other.user_key();
        int r = u1.compare(u2);
        if (r != 0) {
            return r;
        }
        SequenceNumber s1 = sequence();
        SequenceNumber s2 = other.sequence();
        if (s1 > s2) {
            return -1; // Higher sequence numbers come first (newer)
        } else if (s1 < s2) {
            return +1;
        }
        
        ValueType t1 = type();
        ValueType t2 = other.type();
        if (t1 > t2) {
            return -1;
        } else if (t1 < t2) {
            return +1;
        }
        return 0;
    }

private:
    std::string rep_;
};

// ============================================================================
// FILE SYSTEM & OPTIONS CONFIGURATION
// ============================================================================

enum class CompressionType : uint8_t {
    kNoCompression = 0x0,
    kLZ4Compression = 0x1
};

struct Options {
    size_t block_size = kDefaultBlockSize;
    size_t write_buffer_size = 64 * 1024 * 1024; // 64MB MemTable limit
    size_t max_open_files = 5000;
    size_t block_cache_size = 8 * 1024 * 1024;   // 8MB block cache
    CompressionType compression = CompressionType::kLZ4Compression;
    bool create_if_missing = true;
    bool error_if_exists = false;
    uint32_t max_background_compactions = 2;
};

struct ReadOptions {
    bool verify_checksums = true;
    bool fill_cache = true;
    std::optional<SequenceNumber> snapshot = std::nullopt;
};

struct WriteOptions {
    bool sync = false; // WAL fsync on write
};

// ============================================================================
// METADATA & MANIFEST TYPES
// ============================================================================

struct FileMetaData {
    uint64_t file_number;
    uint64_t file_size;
    InternalKey smallest;
    InternalKey largest;
    int allowed_seeks;

    FileMetaData()
        : file_number(0), file_size(0), allowed_seeks(1 << 30) {}
};

struct CompactionStats {
    int64_t micros = 0;
    int64_t bytes_read = 0;
    int64_t bytes_written = 0;
};

} // namespace titan

#endif // TITAN_TYPES_HPP