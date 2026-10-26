#ifndef TITAN_SSTABLE_HPP
#define TITAN_SSTABLE_HPP

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <optional>
#include <system_error>
#include <filesystem>
#include <span>
#include <fstream>
#include <shared_mutex>
#include <algorithm>
#include <cstring>
#include <lz4.h>

namespace titan {

// Error handling hierarchy for SSTable operations
enum class SSTableError {
    kSuccess = 0,
    kFileNotFound,
    kFileReadError,
    kFileWriteError,
    kCorruptedBlock,
    kChecksumMismatch,
    kCompressionError,
    kDecompressionError,
    kInvalidArgument,
    kOutOfMemory
};

class SSTableCategory : public std::error_category {
public:
    const char* name() const noexcept override { return "titan.sstable"; }
    
    std::string message(int ev) const override {
        switch (static_cast<SSTableError>(ev)) {
            case SSTableError::kSuccess: return "Success";
            case SSTableError::kFileNotFound: return "File not found";
            case SSTableError::kFileReadError: return "File read error";
            case SSTableError::kFileWriteError: return "File write error";
            case SSTableError::kCorruptedBlock: return "Corrupted block structure";
            case SSTableError::kChecksumMismatch: return "CRC32 checksum mismatch";
            case SSTableError::kCompressionError: return "LZ4 compression failure";
            case SSTableError::kDecompressionError: return "LZ4 decompression failure";
            case SSTableError::kInvalidArgument: return "Invalid argument";
            case SSTableError::kOutOfMemory: return "Out of memory";
            default: return "Unknown SSTable error";
        }
    }
};

inline const std::error_category& GetSSTableCategory() {
    static SSTableCategory instance;
    return instance;
}

inline std::error_code make_error_code(SSTableError e) {
    return {static_cast<int>(e), GetSSTableCategory()};
}

} // namespace titan

namespace std {
template <>
struct is_error_code_enum<titan::SSTableError> : std::true_type {};
}

namespace titan {

// Internal configuration constants
constexpr uint32_t kDefaultBlockSize = 4096;
constexpr uint32_t kSSTableMagicNumber = 0x54495441; // "TITA"

// Compression type enumeration
enum class CompressionType : uint8_t {
    kNone = 0x00,
    kLZ4  = 0x01
};

// Internal block trailer layout
struct BlockTrailer {
    uint32_t crc32;
    CompressionType compression;
    uint32_t uncompressed_size;
} __attribute__((packed));

// Sparse index entry mapping a block's first key to its absolute disk offset
struct IndexEntry {
    std::string first_key;
    uint64_t offset;
    uint32_t size;
};

// Footer layout stored at the absolute end of the SSTable file
struct SSTableFooter {
    uint64_t index_offset;
    uint64_t index_size;
    uint64_t bloom_offset;
    uint64_t bloom_size;
    uint32_t magic_number;
} __attribute__((packed));

// Simple high-performance CRC32 implementation using standard polynomial 0xEDB88320
class CRC32 {
public:
    static uint32_t Calculate(const uint8_t* data, size_t length) noexcept {
        uint32_t crc = 0xFFFFFFFF;
        for (size_t i = 0; i < length; ++i) {
            crc = table_[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
        }
        return ~crc;
    }

    static uint32_t Calculate(std::string_view data) noexcept {
        return Calculate(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    }

private:
    class TableInitializer {
    public:
        TableInitializer() {
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int j = 0; j < 8; ++j) {
                    c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
                }
                table_[i] = c;
            }
        }
    };

    static inline uint32_t table_[256];
    static inline TableInitializer initializer_;
};

// Optimal Bloom Filter implementation
class BloomFilter {
public:
    BloomFilter() = default;
    
    explicit BloomFilter(size_t num_keys, double false_positive_rate = 0.01) {
        if (num_keys == 0) {
            num_keys = 1;
        }
        // m = - (n * ln(p)) / (ln(2)^2)
        double ln2 = 0.6931471805599453;
        double ln2_sq = ln2 * ln2;
        size_t bits = static_cast<size_t>(-static_cast<double>(num_keys) * std::log(false_positive_rate) / ln2_sq);
        bit_count_ = ((bits + 63) / 64) * 64; // align to 64-bit words
        bytes_.resize(bit_count_ / 8, 0);

        // k = (m / n) * ln(2)
        double k_val = (static_cast<double>(bit_count_) / static_cast<double>(num_keys)) * ln2;
        num_hashes_ = std::max(1U, static_cast<uint32_t>(k_val));
    }

    void Add(std::string_view key) noexcept {
        if (bytes_.empty()) return;
        uint64_t h1 = Hash1(key);
        uint64_t h2 = Hash2(key);
        for (uint32_t i = 0; i < num_hashes_; ++i) {
            uint64_t combined = h1 + i * h2;
            uint64_t bit_idx = combined % bit_count_;
            bytes_[bit_idx / 8] |= (1ULL << (bit_idx % 8));
        }
    }

    bool MayContain(std::string_view key) const noexcept {
        if (bytes_.empty()) return true;
        uint64_t h1 = Hash1(key);
        uint64_t h2 = Hash2(key);
        for (uint32_t i = 0; i < num_hashes_; ++i) {
            uint64_t combined = h1 + i * h2;
            uint64_t bit_idx = combined % bit_count_;
            if ((bytes_[bit_idx / 8] & (1ULL << (bit_idx % 8))) == 0) {
                return false;
            }
        }
        return true;
    }

    void Serialize(std::vector<uint8_t>& dest) const {
        uint32_t hash_count = num_hashes_;
        uint64_t b_count = bit_count_;
        uint64_t vec_size = bytes_.size();

        const size_t header_size = sizeof(hash_count) + sizeof(b_count) + sizeof(vec_size);
        dest.resize(header_size + bytes_.size());

        uint8_t* ptr = dest.data();
        std::memcpy(ptr, &hash_count, sizeof(hash_count)); ptr += sizeof(hash_count);
        std::memcpy(ptr, &b_count, sizeof(b_count)); ptr += sizeof(b_count);
        std::memcpy(ptr, &vec_size, sizeof(vec_size)); ptr += sizeof(vec_size);
        std::memcpy(ptr, bytes_.data(), bytes_.size());
    }

    static std::optional<BloomFilter> Deserialize(std::span<const uint8_t> src) {
        if (src.size() < sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint64_t)) {
            return std::nullopt;
        }
        BloomFilter bf;
        const uint8_t* ptr = src.data();
        
        std::memcpy(&bf.num_hashes_, ptr, sizeof(bf.num_hashes_)); ptr += sizeof(bf.num_hashes_);
        std::memcpy(&bf.bit_count_, ptr, sizeof(bf.bit_count_)); ptr += sizeof(bf.bit_count_);
        
        uint64_t vec_size = 0;
        std::memcpy(&vec_size, ptr, sizeof(vec_size)); ptr += sizeof(vec_size);

        if (src.size() < (ptr - src.data()) + vec_size) {
            return std::nullopt;
        }

        bf.bytes_.resize(vec_size);
        std::memcpy(bf.bytes_.data(), ptr, vec_size);
        return bf;
    }

private:
    static uint64_t Hash1(std::string_view key) noexcept {
        // MurmurHash3 64-bit mix variant
        uint64_t h = 0xc70f6907saturate(key.size());
        for (char c : key) {
            h ^= static_cast<uint64_t>(c);
            h *= 0xff51afd7ed558ccdULL;
            h ^= h >> 32;
        }
        return h;
    }

    static uint64_t Hash2(std::string_view key) noexcept {
        uint64_t h = 0x123456789abcdef0ULL;
        for (char c : key) {
            h += static_cast<uint64_t>(c);
            h = (h << 13) | (h >> 51);
            h *= 0xc4ceb9fe1a85ec53ULL;
        }
        return h;
    }

    static constexpr uint64_t saturate(size_t sz) noexcept { return static_cast<uint64_t>(sz); }

    uint32_t num_hashes_{0};
    uint64_t bit_count_{0};
    std::vector<uint8_t> bytes_;
};

// Builder class to construct an SSTable file sequentially
class SSTableBuilder {
public:
    explicit SSTableBuilder(std::filesystem::path filepath, uint32_t block_size = kDefaultBlockSize)
        : filepath_(std::move(filepath)), block_size_(block_size) {
        file_.open(filepath_, std::ios::out | std::ios::binary | std::ios::trunc);
        current_block_buffer_.reserve(block_size_);
    }

    ~SSTableBuilder() {
        if (file_.is_open()) {
            file_.close();
        }
    }

    SSTableBuilder(const SSTableBuilder&) = delete;
    SSTableBuilder& operator=(const SSTableBuilder&) = delete;
    SSTableBuilder(SSTableBuilder&&) noexcept = default;
    SSTableBuilder& operator=(SSTableBuilder&&) noexcept = default;

    std::error_code Append(std::string_view key, std::string_view value) {
        if (key.empty()) {
            return SSTableError::kInvalidArgument;
        }

        if (current_block_buffer_.empty()) {
            current_block_first_key_ = std::string(key);
        }

        // Key length (4 bytes) + Value length (4 bytes) + Key + Value
        uint32_t k_len = static_cast<uint32_t>(key.size());
        uint32_t v_len = static_cast<uint32_t>(value.size());

        if (current_block_buffer_.size() + k_len + v_len + 8 > block_size_ && !current_block_buffer_.empty()) {
            auto ec = FlushCurrentBlock();
            if (ec) return ec;
            current_block_first_key_ = std::string(key);
        }

        const uint8_t* k_ptr = reinterpret_cast<const uint8_t*>(key.data());
        const uint8_t* v_ptr = reinterpret_cast<const uint8_t*>(value.data());

        current_block_buffer_.insert(current_block_buffer_.end(),
                                     reinterpret_cast<const uint8_t*>(&k_len),
                                     reinterpret_cast<const uint8_t*>(&k_len) + 4);
        current_block_buffer_.insert(current_block_buffer_.end(),
                                     reinterpret_cast<const uint8_t*>(&v_len),
                                     reinterpret_cast<const uint8_t*>(&v_len) + 4);
        current_block_buffer_.insert(current_block_buffer_.end(), k_ptr, k_ptr + k_len);
        current_block_buffer_.insert(current_block_buffer_.end(), v_ptr, v_ptr + v_len);

        keys_count_++;
        bloom_keys_.emplace_back(key);

        return SSTableError::kSuccess;
    }

    std::error_code Finalize() {
        if (!current_block_buffer_.empty()) {
            auto ec = FlushCurrentBlock();
            if (ec) return ec;
        }

        // Build Bloom Filter
        BloomFilter bf(keys_count_, 0.01);
        for (const auto& k : bloom_keys_) {
            bf.Add(k);
        }
        std::vector<uint8_t> bloom_serialized;
        bf.Serialize(bloom_serialized);

        uint64_t bloom_offset = static_cast<uint64_t>(file_.tellp());
        if (!file_.write(reinterpret_cast<const char*>(bloom_serialized.data()), bloom_serialized.size())) {
            return SSTableError::kFileWriteError;
        }
        uint64_t bloom_size = bloom_serialized.size();

        // Build Sparse Index
        std::vector<uint8_t> index_serialized;
        uint32_t entries_count = static_cast<uint32_t>(index_entries_.size());
        index_serialized.insert(index_serialized.end(),
                                reinterpret_cast<const uint8_t*>(&entries_count),
                                reinterpret_cast<const uint8_t*>(&entries_count) + 4);

        for (const auto& entry : index_entries_) {
            uint32_t k_len = static_cast<uint32_t>(entry.first_key.size());
            index_serialized.insert(index_serialized.end(),
                                    reinterpret_cast<const uint8_t*>(&k_len),
                                    reinterpret_cast<const uint8_t*>(&k_len) + 4);
            index_serialized.insert(index_serialized.end(),
                                    reinterpret_cast<const uint8_t*>(entry.first_key.data()),
                                    reinterpret_cast<const uint8_t*>(entry.first_key.data()) + k_len);
            index_serialized.insert(index_serialized.end(),
                                    reinterpret_cast<const uint8_t*>(&entry.offset),
                                    reinterpret_cast<const uint8_t*>(&entry.offset) + sizeof(entry.offset));
            index_serialized.insert(index_serialized.end(),
                                    reinterpret_cast<const uint8_t*>(&entry.size),
                                    reinterpret_cast<const uint8_t*>(&entry.size) + sizeof(entry.size));
        }

        uint64_t index_offset = static_cast<uint64_t>(file_.tellp());
        if (!file_.write(reinterpret_cast<const char*>(index_serialized.data()), index_serialized.size())) {
            return SSTableError::kFileWriteError;
        }
        uint64_t index_size = index_serialized.size();

        // Write Footer
        SSTable