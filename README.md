# titan-lsm
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT) [![CI](https://github.com/sohamshewani/titan-lsm/actions/workflows/ci.yml/badge.svg)](https://github.com/sohamshewani/titan-lsm/actions) [![Security: Hardened](https://img.shields.io/badge/Security-Hardened-blue.svg)](https://github.com/sohamshewani/titan-lsm) [![Stars](https://img.shields.io/github/stars/sohamshewani/titan-lsm?style=flat)](https://github.com/sohamshewani/titan-lsm)

## Executive Summary & Problem Space

`titan-lsm` is an enterprise-grade, high-throughput Log-Structured Merge-tree (LSM-tree) storage engine engineered from the ground up in modern C++ (C++20). Designed for hyperscale distributed transactions and low-latency transactional workloads, `titan-lsm` resolves fundamental storage bottlenecks inherent in traditional B-Trees and naive LSM architectures, such as write stalls, read amplification during point lookups, and memory fragmentation under high allocation churn.

Traditional storage engines often suffer from severe latency spikes caused by global mutex contention during memtable flushes and unbuffered I/O operations that saturate kernel page caches. `titan-lsm` circumvents these bottlenecks through lock-free concurrent skip-list memtables, zero-copy memory arenas, deterministic Write-Ahead Logging (WAL) with group commit primitives, and predictable background compaction pipelines. By maintaining strict memory-ordering semantics and utilizing cache-conscious data layouts, `titan-lsm` achieves predictable microsecond-level latencies while maximizing disk bandwidth utilization.

---

## Structural ASCII Architecture / Dataflow Diagram

The following architectural diagram illustrates the complete dataflow lifecycle within `titan-lsm`, tracing execution paths from client write/read operations down to persistent storage tiers.

```
 +-----------------------------------------------------------------------+
 |                             Client Request                            |
 +-----------------------------------+-----------------------------------+
                                     |
                  +------------------+------------------+
                  |                                     |
                  v (Write: Put/Delete)                 v (Read: Get)
+------------------------------------+   +------------------------------------+
| Write-Ahead Log (WAL)              |   | Memtable (Active SkipList)         |
| [include/titan/wal.hpp]            |   | [include/titan/skiplist.hpp]       |
+-----------------+------------------+   +-----------------+------------------+
                  |                                     |
                  v (Sequential Disk Sync)              | (Miss)
+------------------------------------+                  v
| Persistent Journal File            |   +------------------------------------+
+------------------------------------+   | Immutable Memtable (Flush Queue)   |
                                         +-----------------+------------------+
                                                           |
                                                           v (Background Thread)
                                         +------------------------------------+
                                         | SSTable Builder                    |
                                         | [include/titan/sstable.hpp]        |
                                         +-----------------+------------------+
                                                           |
                                                           v (Sequential Write)
+----------------------------------------------------------+--------------------+
| Persistent Storage Tier (Level-0 -> Level-N SSTables)                         |
| Managed via Compaction Engine [include/titan/compaction.hpp]                   |
+-------------------------------------------------------------------------------+
```

---

## Concurrency Model & Memory Invariants

`titan-lsm` enforces a rigorous concurrency model designed to eliminate lock contention on hot code paths and guarantee memory safety without relying on garbage collection.

- **Lock-Free Concurrent SkipLists:** The active memtable (`include/titan/skiplist.hpp`) implements a lock-free multi-producer, multi-consumer skip-list utilizing atomic pointer CAS (Compare-And-Swap) operations and hazard pointers/epoch-based reclamation. This allows concurrent client threads to insert mutations without acquiring global latches.
- **Arena Allocator (`include/titan/arena.hpp`):** To prevent heap fragmentation and minimize allocation overhead during high-velocity writes, node allocations within the skip-list are serviced by a thread-safe bump-pointer arena. Memory blocks are pre-allocated in large chunks, ensuring high cache locality and deterministic destruction upon memtable retirement.
- **Memory Ownership & RAII:** All system resources—including file descriptors, compaction worker handles, and memory mappings—adhere strictly to RAII (Resource Acquisition Is Initialization) semantics. Unique and shared ownership models are explicitly defined using `std::unique_ptr` and custom deleters, eliminating dangling pointers and resource leaks.
- **Atomicity & Synchronization:** State transitions (e.g., active-to-immutable memtable swaps) are guarded by lightweight read-write spinlocks and atomic sequence counters (`std::atomic<uint64_t>`), ensuring serializability of writes while allowing concurrent non-blocking reads.

---

## Performance Characteristics & Algorithmic Complexity Table

The following table details the theoretical algorithmic complexity and empirical latency profiles of core operations within `titan-lsm`.

| Operation | Algorithmic Complexity | Typical Latency | Bottleneck / Mitigation |
| :--- | :--- | :--- | :--- |
| **Point Lookup (`Get`)** | $O(\log N + L \cdot \log M)$ | $10\mu s - 50\mu s$ | Bloom filter checks mitigate SSTable disk reads. |
| **Write Insertion (`Put`)** | $O(1)$ amortized | $5\mu s - 15\mu s$ | Group commit WAL sync; lock-free skip-list insertion. |
| **Range Scan (`Iterator`)** | $O(\log N + K)$ | $2\mu s$ / key | Prefetched block caches and iterator merging heaps. |
| **Compaction (`Merge`)** | $O(N \log N)$ background | N/A (Asynchronous) | Throttled I/O bandwidth to prevent tail latency spikes. |

---

## Security Architecture & Threat Model

`titan-lsm` is engineered to operate reliably in untrusted environments and multi-tenant cloud infrastructures.

- **Input Boundaries & Sanitization:** All incoming keys, values, and configuration payloads undergo strict length and format validation at the public API boundary (`include/titan/types.hpp`) to prevent buffer over-reads and injection vectors.
- **Memory Corruption Protections:** Data structures utilize strict type safety, avoiding dangerous casts or pointer arithmetic outside encapsulated memory arenas. Integral bounds checks are enforced across all SSTable block decoders.
- **Sanitizer Integration:** The build infrastructure is hardened to support AddressSanitizer (ASan), UndefinedBehaviorSanitizer (UBSan), and ThreadSanitizer (TSan), ensuring zero tolerance for memory leaks, data races, or undefined arithmetic.
- **Threat Vectors Mitigated:**
  - *Denial of Service (Resource Exhaustion):* Bounded memtable sizing and strict write-stalling mechanisms prevent out-of-memory crashes during compaction backpressure.
  - *Data Corruption:* Checksums (CRC32C) are embedded within WAL entries and SSTable data blocks to detect silent disk corruption or bit rot.

---

## Build, Verification & Benchmarking Recipes

To compile the engine, execute the invariant test suite, and run performance benchmarks, use the following exact terminal commands from the repository root.

### Prerequisites
- A modern C++20 compliant compiler (GCC 11+ or Clang 13+)
- GNU Make 4.0+

### 1. Build the Engine and Test Suite
```bash
make clean
make all
```

### 2. Execute Invariant Tests (Verification Suite)
Runs rigorous multi-threaded correctness checks, crash-recovery simulations, and structural invariant assertions.
```bash
make test
# Or execute the compiled test binary directly:
./tests/test_invariants
```

### 3. Run Performance Benchmarks
Executes high-concurrency throughput and latency benchmarks for random writes, sequential scans, and point lookups.
```bash
make bench
# Or execute the benchmark binary directly:
./benchmarks/bench_engine