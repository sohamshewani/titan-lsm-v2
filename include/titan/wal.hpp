#ifndef TITAN_WAL_HPP
#define TITAN_WAL_HPP

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <fstream>
#include <mutex>
#include <system_error>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace titan {

enum class StatusEnum : uint8_t {
    kOk = 0,
    kCorruption = 1,
    kIOError = 2,
    kInvalidArgument = 3,
    kNotSupported = 4
};

class Status {
public:
    Status() : state_(nullptr) {}
    ~Status() { delete[] state_; }

    Status(StatusEnum code, const std::string& msg) {
        uint32_t len = static_cast<uint32_t>(msg.size());
        state_ = new char[5 + len];
        state_[0] = static_cast<char>(code);
        std::memcpy(state_ + 1, &len, 4);
        std::memcpy(state_ + 5, msg.data(), len);
    }

    Status(const Status& rhs) {
        if (rhs.state_ == nullptr) {
            state_ = nullptr;
        } else {
            uint32_t len;
            std::memcpy(&len, rhs.state_ + 1, 4);
            state_ = new char[5 + len];
            std::memcpy(state_, rhs.state_, 5 + len);
        }
    }

    Status& operator=(const Status& rhs) {
        if (this != &rhs) {
            delete[] state_;
            if (rhs.state_ == nullptr) {
                state_ = nullptr;
            } else {
                uint32_t len;
                std::memcpy(&len, rhs.state_ + 1, 4);
                state_ = new char[5 + len];
                std::memcpy(state_, rhs.state_, 5 + len);
            }
        }
        return *this;
    }

    Status(Status&& rhs) noexcept : state_(rhs.state_) {
        rhs.state_ = nullptr;
    }

    Status& operator=(Status&& rhs) noexcept {
        if (this != &rhs) {
            delete[] state_;
            state_ = rhs.state_;
            rhs.state_ = nullptr;
        }
        return *this;
    }

    bool ok() const { return state_ == nullptr || code() == StatusEnum::kOk; }
    bool IsCorruption() const { return code() == StatusEnum::kCorruption; }
    bool IsIOError() const { return code() == StatusEnum::kIOError; }
    bool IsInvalidArgument() const { return code() == StatusEnum::kInvalidArgument; }

    StatusEnum code() const {
        if (state_ == nullptr) return StatusEnum::kOk;
        return static_cast<StatusEnum>(state_[0]);
    }

    std::string ToString() const {
        if (state_ == nullptr) return "OK";
        uint32_t len;
        std::memcpy(&len, state_ + 1, 4);
        std::string msg(state_ + 5, len);
        std::string prefix;
        switch (code()) {
            case StatusEnum::kOk: prefix = "OK"; break;
            case StatusEnum::kCorruption: prefix = "Corruption: "; break;
            case StatusEnum::kIOError: prefix = "IO error: "; break;
            case StatusEnum::kInvalidArgument: prefix = "Invalid argument: "; break;
            case StatusEnum::kNotSupported: prefix = "Not implemented: "; break;
        }
        return prefix + msg;
    }

    static Status OK() { return Status(); }
    static Status Corruption(const std::string& msg) { return Status(StatusEnum::kCorruption, msg); }
    static Status IOError(const std::string& msg) { return Status(StatusEnum::kIOError, msg); }
    static Status InvalidArgument(const std::string& msg) { return Status(StatusEnum::kInvalidArgument, msg); }

private:
    char* state_;
};

class Slice {
public:
    Slice() : data_(""), size_(0) {}
    Slice(const char* d, size_t n) : data_(d), size_(n) {}
    Slice(const std::string& s) : data_(s.data()), size_(s.size()) {}

    const char* data() const { return data_; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    char operator[](size_t n) const { return data_[n]; }

    void clear() {
        data_ = "";
        size_ = 0;
    }

    void remove_prefix(size_t n) {
        data_ += n;
        size_ -= n;
    }

    std::string ToString() const { return std::string(data_, size_); }

private:
    const char* data_;
    size_t size_;
};

inline uint32_t CRC32(const char* buf, size_t size) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; ++i) {
        crc ^= static_cast<uint8_t>(buf[i]);
        for (int j = 0; j < 8; ++j) {
            uint32_t mask = -(crc & 1);
            crc = (crc >> 1) ^ (0xEDB88320 & mask);
        }
    }
    return ~crc;
}

enum RecordType : uint8_t {
    kZeroType = 0,
    kFullType = 1,
    kFirstType = 2,
    kMiddleType = 3,
    kLastType = 4
};

class WritableFile {
public:
    explicit WritableFile(std::string filename) : filename_(std::move(filename)), fd_(-1) {}
    
    ~WritableFile() {
        if (fd_ != -1) {
            ::close(fd_);
        }
    }

    Status Open() {
        fd_ = ::open(filename_.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd_ == -1) {
            return Status::IOError("Failed to open file: " + filename_ + " error: " + std::strerror(errno));
        }
        return Status::OK();
    }

    Status Append(const Slice& data) {
        size_t left = data.size();
        const char* p = data.data();
        while (left > 0) {
            ssize_t written = ::write(fd_, p, left);
            if (written <= 0) {
                if (errno == EINTR) continue;
                return Status::IOError("Write error on " + filename_ + ": " + std::strerror(errno));
            }
            left -= written;
            p += written;
        }
        return Status::OK();
    }

    Status Flush() {
        return Status::OK();
    }

    Status Sync() {
        if (::fsync(fd_) == -1) {
            return Status::IOError("Fsync error on " + filename_ + ": " + std::strerror(errno));
        }
        return Status::OK();
    }

private:
    std::string filename_;
    int fd_;
};

class SequentialFile {
public:
    explicit SequentialFile(std::string filename) : filename_(std::move(filename)), fd_(-1) {}

    ~SequentialFile() {
        if (fd_ != -1) {
            ::close(fd_);
        }
    }

    Status Open() {
        fd_ = ::open(filename_.c_str(), O_RDONLY);
        if (fd_ == -1) {
            return Status::IOError("Failed to open sequential file: " + filename_ + " error: " + std::strerror(errno));
        }
        return Status::OK();
    }

    Status Read(size_t n, Slice* result, char* scratch) {
        size_t read_bytes = 0;
        while (read_bytes < n) {
            ssize_t nread = ::read(fd_, scratch + read_bytes, n - read_bytes);
            if (nread < 0) {
                if (errno == EINTR) continue;
                return Status::IOError("Read error on " + filename_ + ": " + std::strerror(errno));
            }
            if (nread == 0) {
                break;
            }
            read_bytes += nread;
        }
        *result = Slice(scratch, read_bytes);
        return Status::OK();
    }

    Status Skip(uint64_t n) {
        if (::lseek(fd_, n, SEEK_CUR) == static_cast<off_t>(-1)) {
            return Status::IOError("Skip error on " + filename_ + ": " + std::strerror(errno));
        }
        return Status::OK();
    }

private:
    std::string filename_;
    int fd_;
};

class LogWriter {
public:
    explicit LogWriter(std::unique_ptr<WritableFile> dest)
        : dest_(std::move(dest)), block_offset_(0) {}

    ~LogWriter() = default;

    Status AddRecord(const Slice& slice) {
        const char* ptr = slice.data();
        size_t left = slice.size();

        Status s;
        bool begin = true;
        do {
            const int kBlockSize = 32768;
            int leftover = kBlockSize - block_offset_;
            if (leftover < kHeaderSize) {
                if (leftover > 0) {
                    char zeros[7] = {0};
                    s = dest_->Append(Slice(zeros, leftover));
                    if (!s.ok()) break;
                }
                block_offset_ = 0;
                leftover = kBlockSize;
            }

            size_t avail = leftover - kHeaderSize;
            size_t fragment_length = (left < avail) ? left : avail;

            RecordType type;
            bool end = (left == fragment_length);
            if (begin && end) {
                type = kFullType;
            } else if (begin) {
                type = kFirstType;
            } else if (end) {
                type = kLastType;
            } else {
                type = kMiddleType;
            }

            s = EmitPhysicalRecord(type, ptr, fragment_length);
            ptr += fragment_length;
            left -= fragment_length;
            begin = false;
        } while (s.ok() && left > 0);

        if (s.ok()) {
            s = dest_->Flush();
        }
        return s;
    }

    Status Sync() {
        return dest_->Sync();
    }

private:
    Status EmitPhysicalRecord(RecordType t, const char* ptr, size_t length) {
        assert(length <= 0xffff);
        assert(block_offset_ + kHeaderSize + length <= 32768);

        char buf[kHeaderSize];
        buf[4] = static_cast<char>(length & 0xff);
        buf[5] = static_cast<char>(length >> 8);
        buf[6] = static_cast<char>(t);

        uint32_t crc = CRC32(buf + 6, 1);
        crc = CRC32_Combine(crc, ptr, length);
        
        buf[0] = static_cast<char>(crc & 0xff);
        buf[1] = static_cast<char>((crc >> 8) & 0xff);
        buf[2] = static_cast<char>((crc >> 16) & 0xff);
        buf[3] = static_cast<char>((crc >> 24) & 0xff);

        Status s = dest_->Append(Slice(buf, kHeaderSize));
        if (s.ok()) {
            s = dest_->Append(Slice(ptr, length));
            if (s.ok()) {
                block_offset_ += kHeaderSize + length;
            }
        }
        return s;
    }

    uint32_t CRC32_Combine(uint32_t crc, const char* ptr, size_t length) {
        uint32_t c = crc;
        for (size_t i = 0; i < length; ++i) {
            c ^= static_cast<uint8_t>(ptr[i]);
            for (int j = 0; j < 8; ++j) {
                uint32_t mask = -(c & 1);
                c = (c >> 1) ^ (0xEDB88320 & mask);
            }
        }
        return ~c;
    }

    std::unique_ptr<WritableFile> dest_;
    int block_offset_;
    static const int kHeaderSize = 7;
};

class LogReader {
public:
    class SequentialFileWrapper {
    public:
        explicit SequentialFileWrapper(std::unique_ptr<SequentialFile> file) : file_(std::move(file)) {}
        Status Open() { return file_->Open(); }
        Status Read(size_t n, Slice* result, char* scratch) { return file_->Read(n, result, scratch); }
        Status Skip(uint64_t n) { return file_->Skip(n); }
    private:
        std::unique_ptr<SequentialFile> file_;
    };

    LogReader(std::unique_ptr<SequentialFile> file, bool checksum, uint64_t initial_offset)
        : file_(std::move(file)),
          checksum_(checksum),
          backing_store_(new char[32768]),
          buffer_(backing_store_, 0),
          eof_(false),
          last_record_offset_(0),
          end_of_buffer_offset_(0),
          initial_offset_(initial_offset) {}

    ~LogReader() {
        delete[] backing_store_;
    }

    bool ReadRecord(Slice* record, std::string* scratch) {
        scratch->clear();
        record->clear();
        bool in_fragmented_record = false;
        uint64_t prospective_record_offset = 0;

        Slice fragment;
        while (true) {
            const int physical_record_type = ReadPhysicalRecord(&fragment);
            uint64_t physical_record_offset = end_of_buffer_offset_ - buffer_.size() - kHeaderSize - fragment.size();

            if (initial_offset_ > 0) {
                if (physical_record_offset < initial_offset_) {
                    continue;
                } else {
                    initial_offset_ = 0;
                }
            }

            switch (physical_record_type) {
                case kFullType:
                    if (in_fragmented_record) {
                        return false;
                    }
                    last_record_offset_ = physical_record_offset;
                    *record = fragment;
                    return true;

                case kFirstType:
                    if (in_fragmented_record) {
                        return false;
                    }
                    prospective_record_offset = physical_record_offset;
                    scratch->assign(fragment.data(), fragment.size());
                    in_fragmented_record = true;
                    break;

                case kMiddleType:
                    if (!in_fragmented_record) {
                        return false;
                    }
                    scratch->append(fragment.data(), fragment.size());
                    break;

                case kLastType:
                    if (!in_fragmented_record) {
                        return false;
                    }
                    scratch->append(fragment.data(), fragment.size());
                    *record = Slice(*scratch);
                    last_record_offset_ = prospective_record_offset;
                    return true;

                case kEof:
                    if (in_fragmented_record) {
                        scratch->clear();
                    }
                    return false;

                case kBadRecord:
                    if (in_fragmented_record) {
                        in_fragmented_record = false;
                        scratch->clear();
                    }
                    break;

                default: {
                    char buf[40];
                    std::snprintf(buf, sizeof(buf), "unknown record type %d", physical_record_type);
                    return false;
                }
            }
        }
    }

private:
    enum {
        kEof =