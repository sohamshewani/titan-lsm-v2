#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <cassert>
#include <filesystem>
#include <random>
#include <chrono>
#include <algorithm>
#include <cstring>

// -----------------------------------------------------------------------------
// Mock / Minimal Titan-LSM Engine Implementation for Self-Contained Compilation
// In a real environment, this would include "titan/db.h"
// -----------------------------------------------------------------------------
namespace titan {
    struct Status {
        enum Code { OK, NotFound, Corruption, IOError, Busy } code;
        std::string msg;
        bool ok() const { return code == OK; }
        static Status OK_Status() { return {OK, ""}; }
        static Status NotFound_Status(const std::string& m = "") { return {NotFound, m}; }
        static Status Corruption_Status(const std::string& m = "") { return {Corruption, m}; }
        static Status IOError_Status(const std::string& m = "") { return {IOError, m}; }
    };

    struct Options {
        std::string path;
        size_t write_buffer_size = 4 * 1024 * 1024;
        size_t max_background_jobs = 4;
        bool paranoid_checks = true;
    };

    class DB {
    public:
        static Status Open(const Options& options, DB** dbptr) {
            *dbptr = new DB(options);
            return Status::OK_Status();
        }

        virtual ~DB() {
            if (simulated_crash_) return;
            // Clean up files if needed
        }

        virtual Status Put(const std::string& key, const std::string& value) {
            if (simulated_crash_) return Status::IOError_Status("Simulated crash state");
            std::lock_guard<std::mutex> lock(mu_);
            if (key.empty()) return Status::Corruption_Status("Empty key not allowed");
            store_[key] = value;
            return Status::OK_Status();
        }

        virtual Status Get(const std::string& key, std::string* value) {
            if (simulated_crash_) return Status::IOError_Status("Simulated crash state");
            std::lock_guard<std::mutex> lock(mu_);
            auto it = store_.find(key);
            if (it == store_.end()) {
                return Status::NotFound_Status();
            }
            *value = it->second;
            return Status::OK_Status();
        }

        virtual Status Delete(const std::string& key) {
            if (simulated_crash_) return Status::IOError_Status("Simulated crash state");
            std::lock_guard<std::mutex> lock(mu_);
            store_.erase(key);
            return Status::OK_Status();
        }

        virtual Status CompactRange() {
            if (simulated_crash_) return Status::IOError_Status("Simulated crash state");
            // Simulate LSM compaction pass
            std::lock_guard<std::mutex> lock(mu_);
            compaction_count_++;
            return Status::OK_Status();
        }

        // Fault injection hooks
        void SimulateCrash() {
            std::lock_guard<std::mutex> lock(mu_);
            simulated_crash_ = true;
        }

        void Recover() {
            std::lock_guard<std::mutex> lock(mu_);
            simulated_crash_ = false;
        }

        size_t GetCompactionCount() const {
            std::lock_guard<std::mutex> lock(mu_);
            return compaction_count_;
        }

    protected:
        DB(const Options& opt) : options_(opt), simulated_crash_(false), compaction_count_(0) {}
        Options options_;
        mutable std::mutex mu_;
        std::map<std::string, std::string> store_;
        bool simulated_crash_;
        size_t compaction_count_;
    };
}

// -----------------------------------------------------------------------------
// Test Framework and Utilities
// -----------------------------------------------------------------------------
#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        std::cerr << "\n[FAIL] " << __FILE__ << ":" << __LINE__ \
                  << " - Assertion failed: " << #cond << std::endl; \
        std::exit(1); \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    auto _a = (a); \
    auto _b = (b); \
    if (_a != _b) { \
        std::cerr << "\n[FAIL] " << __FILE__ << ":" << __LINE__ \
                  << " - Assertion failed: " << #a << " (" << _a << ") == " \
                  << #b << " (" << _b << ")" << std::endl; \
        std::exit(1); \
    } \
} while(0)

const std::string kTestDir = "./titan_lsm_test_db";

void CleanupTestDir() {
    std::error_code ec;
    std::filesystem::remove_all(kTestDir, ec);
}

// -----------------------------------------------------------------------------
// 1. Boundary and Edge-Case Tests
// -----------------------------------------------------------------------------
void TestBoundariesAndEdgeCases() {
    std::cout << "[RUN ] TestBoundariesAndEdgeCases..." << std::endl;
    CleanupTestDir();

    titan::Options options;
    options.path = kTestDir;
    titan::DB* db = nullptr;
    ASSERT_TRUE(titan::DB::Open(options, &db).ok());

    // Edge Case: Empty Key
    {
        titan::Status s = db->Put("", "value_for_empty_key");
        // Depending on LSM rules, empty keys might be rejected or accepted. 
        // Our mock explicitly rejects empty keys.
        ASSERT_TRUE(!s.ok());
    }

    // Edge Case: Empty Value (Deletions vs Empty strings)
    {
        std::string key = "empty_val_key";
        ASSERT_TRUE(db->Put(key, "").ok());
        std::string val;
        ASSERT_TRUE(db->Get(key, &val).ok());
        ASSERT_EQ(val, "");
    }

    // Edge Case: Max-size / Large Keys and Values (1MB)
    {
        std::string large_key(1024, 'k');
        std::string large_val(1024 * 1024, 'v');
        ASSERT_TRUE(db->Put(large_key, large_val).ok());
        std::string retrieved_val;
        ASSERT_TRUE(db->Get(large_key, &retrieved_val).ok());
        ASSERT_EQ(retrieved_val.size(), large_val.size());
        ASSERT_EQ(retrieved_val, large_val);
    }

    // Edge Case: Special Characters & Binary Data
    {
        std::string binary_key = "\x00\x01\x02\xff\xfe";
        std::string binary_val = "data\0with\0nulls\xff", 15;
        ASSERT_TRUE(db->Put(binary_key, binary_val).ok());
        std::string out_val;
        ASSERT_TRUE(db->Get(binary_key, &out_val).ok());
        ASSERT_EQ(out_val, binary_val);
    }

    // Edge Case: Rapid Overwrites of Same Key
    {
        std::string key = "churn_key";
        for (int i = 0; i < 1000; ++i) {
            ASSERT_TRUE(db->Put(key, std::to_string(i)).ok());
        }
        std::string val;
        ASSERT_TRUE(db->Get(key, &val).ok());
        ASSERT_EQ(val, "999");
    }

    delete db;
    CleanupTestDir();
    std::cout << "[PASS] TestBoundariesAndEdgeCases" << std::endl;
}

// -----------------------------------------------------------------------------
// 2. Multi-Threaded Contention and Concurrency Tests (Race Conditions)
// -----------------------------------------------------------------------------
void TestConcurrentContention() {
    std::cout << "[RUN ] TestConcurrentContention..." << std::endl;
    CleanupTestDir();

    titan::Options options;
    options.path = kTestDir;
    titan::DB* db = nullptr;
    ASSERT_TRUE(titan::DB::Open(options, &db).ok());

    constexpr int kNumThreads = 8;
    constexpr int kOpsPerThread = 500;
    std::atomic<bool> start_flag{false};
    std::vector<std::thread> threads;

    // Concurrent Writers writing disjoint keys
    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([db, t, kOpsPerThread, &start_flag]() {
            while (!start_flag) { std::this_thread::yield(); }
            for (int i = 0; i < kOpsPerThread; ++i) {
                std::string key = "t_" + std::to_string(t) + "_k_" + std::to_string(i);
                std::string val = "val_" + std::to_string(i);
                titan::Status s = db->Put(key, val);
                ASSERT_TRUE(s.ok());
            }
        });
    }

    start_flag = true;
    for (auto& th : threads) {
        th.join();
    }
    threads.clear();

    // Verify written data concurrently
    start_flag = false;
    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([db, t, kOpsPerThread, &start_flag]() {
            while (!start_flag) { std::this_thread::yield(); }
            for (int i = 0; i < kOpsPerThread; ++i) {
                std::string key = "t_" + std::to_string(t) + "_k_" + std::to_string(i);
                std::string expected_val = "val_" + std::to_string(i);
                std::string val;
                titan::Status s = db->Get(key, &val);
                ASSERT_TRUE(s.ok());
                ASSERT_EQ(val, expected_val);
            }
        });
    }

    start_flag = true;
    for (auto& th : threads) {
        th.join();
    }
    threads.clear();

    // Concurrent Mixed Workload: Readers, Writers, Deletions, and Compactions
    std::atomic<bool> stop_flag{false};
    std::vector<std::thread> mixed_threads;

    // Background compactor thread
    mixed_threads.emplace_back([db, &stop_flag]() {
        while (!stop_flag) {
            db->CompactRange();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    // Mutator/Reader threads
    for (int t = 0; t < 4; ++t) {
        mixed_threads.emplace_back([db, t, &stop_flag]() {
            std::mt19937 rng(t + 1337);
            std::uniform_int_distribution<int> dist(0, 499);
            while (!stop_flag) {
                int idx = dist(rng);
                std::string key = "t_0_k_" + std::to_string(idx);
                std::string val;
                db->Get(key, &val);
                if (idx % 2 == 0) {
                    db->Put(key, "updated_mixed");
                } else {
                    db->Delete(key);
                }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    stop_flag = true;
    for (auto& th : mixed_threads) {
        th.join();
    }

    delete db;
    CleanupTestDir();
    std::cout << "[PASS] TestConcurrentContention" << std::endl;
}

// -----------------------------------------------------------------------------
// 3. Failure, Crash Injection, and State Recovery Tests
// -----------------------------------------------------------------------------
void TestFailureAndRecovery() {
    std::cout << "[RUN ] TestFailureAndRecovery..." << std::endl;