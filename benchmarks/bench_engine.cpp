#include <iostream>
#include <chrono>
#include <vector>
#include <string>
#include <numeric>
#include <algorithm>
#include <iomanip>
#include <cstdint>
#include <random>

// Minimal mock/simulated headers for titan-lsm to make this a self-contained,
// compilable microbenchmark template. In a real environment, replace these
// with: #include "titan/db.h"
namespace titan {
    struct Options {
        std::string db_path = "/tmp/titan_lsm_bench";
        size_t write_buffer_size = 64 * 1024 * 1024;
    };
    
    struct Status {
        bool ok() const { return true; }
        std::string ToString() const { return "OK"; }
    };

    class DB {
    public:
        static Status Open(const Options& options, DB** dbptr) {
            *dbptr = new DB();
            return Status();
        }

        Status Put(int /*flags*/, const std::string& key, const std::string& value) {
            // Simulate LSM write amplification and IO overhead
            volatile uint64_t sum = 0;
            for (size_t i = 0; i < 150; ++i) sum += i;
            return Status();
        }

        Status Get(int /*flags*/, const std::string& key, std::string* value) {
            // Simulate LSM read amplification / block lookup overhead
            volatile uint64_t sum = 0;
            for (size_t i = 0; i < 300; ++i) sum += i;
            *value = "val_" + key;
            return Status();
        }

        ~DB() {}
    };
}

int main() {
    constexpr int kNumOperations = 500000;
    constexpr size_t kKeySize = 16;
    constexpr size_t kValueSize = 100;

    titan::Options options;
    titan::DB* db = nullptr;
    titan::Status status = titan::DB::Open(options, &db);
    if (!status.ok()) {
        std::cerr << "Failed to open titan-lsm database.\n";
        return 1;
    }

    // Pre-generate random keys and values to isolate IO/Engine latency from generation overhead
    std::vector<std::string> keys(kNumOperations);
    std::vector<std::string> values(kNumOperations);
    
    std::mt19937_64 rng(1337);
    std::uniform_int_distribution<char> dist('a', 'z');

    for (int i = 0; i < kNumOperations; ++i) {
        std::string k(kKeySize, ' ');
        std::string v(kValueSize, ' ');
        for (size_t j = 0; j < kKeySize; ++j) k[j] = dist(rng);
        for (size_t j = 0; j < kValueSize; ++j) v[j] = dist(rng);
        keys[i] = k;
        values[i] = v;
    }

    std::vector<double> put_latencies_us(kNumOperations);
    std::vector<double> get_latencies_us(kNumOperations);

    // --- PUT BENCHMARK ---
    auto total_start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < kNumOperations; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        db->Put(0, keys[i], values[i]);
        auto end = std::chrono::high_resolution_clock::now();
        
        std::chrono::duration<double, std::micro> elapsed = end - start;
        put_latencies_us[i] = elapsed.count();
    }

    auto total_put_end = std::chrono::high_resolution_clock::now();

    // --- GET BENCHMARK ---
    std::string val_out;
    for (int i = 0; i < kNumOperations; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        db->Get(0, keys[i], &val_out);
        auto end = std::chrono::high_resolution_clock::now();
        
        std::chrono::duration<double, std::micro> elapsed = end - start;
        get_latencies_us[i] = elapsed.count();
    }

    auto total_get_end = std::chrono::high_resolution_clock::now();

    // --- METRICS CALCULATION ---
    auto compute_metrics = [](std::vector<double>& lats, double total_seconds) {
        std::sort(lats.begin(), lats.end());
        double p50 = lats[lats.size() * 0.50];
        double p99 = lats[lats.size() * 0.99];
        double p999 = lats[lats.size() * 0.999];
        double ops_sec = static_cast<double>(lats.size()) / total_seconds;
        return std::make_tuple(ops_sec, p50, p99, p999);
    };

    std::chrono::duration<double> put_seconds = total_put_end - total_start;
    std::chrono::duration<double> get_seconds = total_get_end - total_put_end;

    auto [put_ops, put_p50, put_p99, put_p999] = compute_metrics(put_latencies_us, put_seconds.count());
    auto [get_ops, get_p50, get_p99, get_p999] = compute_metrics(get_latencies_us, get_seconds.count());

    // --- FORMATTED OUTPUT ---
    std::cout << "\n";
    std::cout << "=================================================================================\n";
    std::cout << "                 TITAN-LSM PERFORMANCE MICROBENCHMARK REPORT                     \n";
    std::cout << "=================================================================================\n";
    std::cout << " Operations : " << kNumOperations << " per workload\n";
    std::cout << " Key Size   : " << kKeySize << " bytes\n";
    std::cout << " Value Size : " << kValueSize << " bytes\n";
    std::cout << "---------------------------------------------------------------------------------\n";
    std::cout << std::left << std::setw(12) << "Operation" 
              << std::right << std::setw(15) << "Throughput" 
              << std::setw(15) << "p50 Latency" 
              << std::setw(15) << "p99 Latency" 
              << std::setw(15) << "p99.9 Latency" << "\n";
    std::cout << "---------------------------------------------------------------------------------\n";
    
    std::cout << std::left << std::setw(12) << "PUT" 
              << std::right << std::setw(12) << static_cast<int>(put_ops) << " ops/s"
              << std::setw(11) << put_p50 << " us"
              << std::setw(11) << put_p99 << " us"
              << std::setw(13) << put_p999 << " us" << "\n";

    std::cout << std::left << std::setw(12) << "GET" 
              << std::right << std::setw(12) << static_cast<int>(get_ops) << " ops/s"
              << std::setw(11) << get_p50 << " us"
              << std::setw(11) << get_p99 << " us"
              << std::setw(13) << get_p999 << " us" << "\n";

    std::cout << "=================================================================================\n\n";

    delete db;
    return 0;
}