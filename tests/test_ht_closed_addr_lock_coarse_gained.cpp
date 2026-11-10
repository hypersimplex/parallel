#include <vector>
#include <map>
#include <memory>
#include <cassert>
#include <utility>
#include <cstdint>
#include <climits>
#include <atomic>
#include <thread>

#include "ht_closed_addr_lock_coarse_gained.hpp"

using namespace std;

// ============================================================================
// Large Value Payloads for Stress Testing Move/Copy Semantics & Memory Safety
// ============================================================================

struct LargeMoveOnlyStruct {
  static constexpr size_t NUM_WORDS = 512; // 4 KB inline payload
  uint64_t words[NUM_WORDS]{};
  unique_ptr<uint64_t[]> heap_buf;
  int owner_key = -1;
  uint64_t generation = 0;

  LargeMoveOnlyStruct() = default;

  LargeMoveOnlyStruct(int k, uint64_t gen)
    : heap_buf(make_unique<uint64_t[]>(64)), owner_key(k), generation(gen) {
    for(size_t i = 0; i < NUM_WORDS; ++i){
      words[i] = (static_cast<uint64_t>(k) << 32) ^ (gen * 1315423911ULL + i);
    }
    for(size_t i = 0; i < 64; ++i){
      heap_buf[i] = words[i] ^ 0xa5a5a5a5a5a5a5a5ULL;
    }
  }

  LargeMoveOnlyStruct(LargeMoveOnlyStruct const &) = delete;
  LargeMoveOnlyStruct & operator=(LargeMoveOnlyStruct const &) = delete;

  LargeMoveOnlyStruct(LargeMoveOnlyStruct && other) noexcept
    : heap_buf(std::move(other.heap_buf)), owner_key(other.owner_key), generation(other.generation) {
    for(size_t i = 0; i < NUM_WORDS; ++i){
      words[i] = other.words[i];
      other.words[i] = 0;
    }
    other.owner_key = -999;
    other.generation = 0;
  }

  LargeMoveOnlyStruct & operator=(LargeMoveOnlyStruct && other) noexcept {
    if(this != &other){
      heap_buf = std::move(other.heap_buf);
      owner_key = other.owner_key;
      generation = other.generation;
      for(size_t i = 0; i < NUM_WORDS; ++i){
        words[i] = other.words[i];
        other.words[i] = 0;
      }
      other.owner_key = -999;
      other.generation = 0;
    }
    return *this;
  }

  bool verify(int expected_key) const {
    if(owner_key != expected_key || !heap_buf || generation == 0){
      return false;
    }
    for(size_t i = 0; i < NUM_WORDS; ++i){
      uint64_t expected_w = (static_cast<uint64_t>(expected_key) << 32) ^ (generation * 1315423911ULL + i);
      if(words[i] != expected_w){
        return false;
      }
    }
    for(size_t i = 0; i < 64; ++i){
      if(heap_buf[i] != (words[i] ^ 0xa5a5a5a5a5a5a5a5ULL)){
        return false;
      }
    }
    return true;
  }
};

struct LargeMovableCopyableStruct {
  static constexpr size_t NUM_WORDS = 512; // 4 KB inline payload
  uint64_t words[NUM_WORDS]{};
  vector<uint64_t> heap_vec;
  int owner_key = -1;
  uint64_t generation = 0;

  LargeMovableCopyableStruct() = default;

  LargeMovableCopyableStruct(int k, uint64_t gen)
    : heap_vec(64), owner_key(k), generation(gen) {
    for(size_t i = 0; i < NUM_WORDS; ++i){
      words[i] = (static_cast<uint64_t>(k) << 32) ^ (gen * 2654435761ULL + i);
    }
    for(size_t i = 0; i < 64; ++i){
      heap_vec[i] = words[i] ^ 0x5a5a5a5a5a5a5a5aULL;
    }
  }

  LargeMovableCopyableStruct(LargeMovableCopyableStruct const &) = default;
  LargeMovableCopyableStruct & operator=(LargeMovableCopyableStruct const &) = default;

  LargeMovableCopyableStruct(LargeMovableCopyableStruct && other) noexcept
    : heap_vec(std::move(other.heap_vec)), owner_key(other.owner_key), generation(other.generation) {
    for(size_t i = 0; i < NUM_WORDS; ++i){
      words[i] = other.words[i];
      other.words[i] = 0;
    }
    other.owner_key = -999;
    other.generation = 0;
  }

  LargeMovableCopyableStruct & operator=(LargeMovableCopyableStruct && other) noexcept {
    if(this != &other){
      heap_vec = std::move(other.heap_vec);
      owner_key = other.owner_key;
      generation = other.generation;
      for(size_t i = 0; i < NUM_WORDS; ++i){
        words[i] = other.words[i];
        other.words[i] = 0;
      }
      other.owner_key = -999;
      other.generation = 0;
    }
    return *this;
  }

  bool verify(int expected_key) const {
    if(owner_key != expected_key || heap_vec.size() != 64 || generation == 0){
      return false;
    }
    for(size_t i = 0; i < NUM_WORDS; ++i){
      uint64_t expected_w = (static_cast<uint64_t>(expected_key) << 32) ^ (generation * 2654435761ULL + i);
      if(words[i] != expected_w){
        return false;
      }
    }
    for(size_t i = 0; i < 64; ++i){
      if(heap_vec[i] != (words[i] ^ 0x5a5a5a5a5a5a5a5aULL)){
        return false;
      }
    }
    return true;
  }
};

// ============================================================================
// 1. Single-Threaded CRUD, Chain Unlinking, Boundary Keys & Multi-Stage Resize
// ============================================================================
void test_single_thread_crud_collisions_and_boundary_keys(){
  HashTable<int> tbl(32);
  map<int, int> expected;

  // 1a. Boundary and negative keys (exercises sign-extension to uint64_t key)
  vector<int> boundary_keys = {
    INT_MIN, INT_MIN + 1, INT_MIN + 2,
    -1000000, -65536, -1024, -64, -32, -2, -1,
    0, 1, 2, 32, 64, 1024, 65536, 1000000,
    INT_MAX - 2, INT_MAX - 1, INT_MAX
  };
  for(int k : boundary_keys){
    int v = (k == INT_MIN) ? -42 : (k ^ 0x5a5a5a5a);
    tbl.add(k, int(v));
    expected[k] = v;
  }
  assert(!tbl.contains(9999999));
  assert(!tbl.get(9999999).has_value());
  assert(!tbl.remove(9999999).has_value());

  // 1b. Explicit 5-node collision chain in bucket 0 (multiples of 32: 320, 352, 384, 416, 448)
  // Exercises removing middle (384), head (448, inserted last), and tail (320, inserted first)
  vector<int> chain_keys = {320, 352, 384, 416, 448};
  for(int k : chain_keys){
    tbl.add(k, k * 10);
  }
  // Overwrite in-place at head, middle, and tail
  for(int k : chain_keys){
    tbl.add(k, k * 20);
  }
  auto rem_mid = tbl.remove(384);
  assert(rem_mid.has_value() && rem_mid.value() == 384 * 20);
  assert(!tbl.remove(384).has_value()); // idempotent remove

  auto rem_head = tbl.remove(448);
  assert(rem_head.has_value() && rem_head.value() == 448 * 20);

  auto rem_tail = tbl.remove(320);
  assert(rem_tail.has_value() && rem_tail.value() == 320 * 20);

  assert(tbl.get(352).value() == 352 * 20);
  assert(tbl.get(416).value() == 416 * 20);
  expected[352] = 352 * 20;
  expected[416] = 416 * 20;

  // 1c. Multi-stage expansion: insert 700 negative & positive keys (32 -> 64 -> 128 -> 256)
  for(int i = -350; i < 350; ++i){
    int v = i * 10 + 7;
    tbl.add(i, int(v));
    expected[i] = v;
  }
  // Overwrite every even key across the expanded table
  for(int i = -350; i < 350; i += 2){
    int v = i * 100 + 3;
    tbl.add(i, int(v));
    expected[i] = v;
  }

  for(auto const & [k, v] : expected){
    assert(tbl.contains(k));
    auto res = tbl.get(k);
    assert(res.has_value() && res.value() == v);
  }

  // 1d. Multi-stage contraction: remove all [-350, 350) keys (256 -> 128 -> 64 -> 32)
  for(int i = -350; i < 350; ++i){
    auto rem = tbl.remove(i);
    assert(rem.has_value() && rem.value() == expected[i]);
    expected.erase(i);
    assert(!tbl.contains(i));
  }

  // Verify surviving boundary keys after shrink, then drain completely and repopulate
  for(auto const & [k, v] : expected){
    auto rem = tbl.remove(k);
    assert(rem.has_value() && rem.value() == v);
    assert(!tbl.contains(k));
  }
  expected.clear();

  for(int i = 1000; i < 1200; ++i){
    tbl.add(i, i * 5);
    assert(tbl.get(i).value() == i * 5);
  }
}

// ============================================================================
// 2. Pathological Single-Bucket Collision Chain Across Resizes
// ============================================================================
void test_pathological_single_bucket_chain_across_resizes(){
  HashTable<int> tbl(32);

  // All multiples of 1024 hash to bucket 0 across bucket counts 32, 64, 128, 256, 512, 1024.
  // Inserting 350 keys forces expansions (32 -> 64 -> 128) where all 350 nodes stay in bucket 0
  // and reverse their chain order on every migration.
  constexpr int NUM_COLLIDING = 350;
  for(int i = 0; i < NUM_COLLIDING; ++i){
    tbl.add(i * 1024, i + 100);
  }

  // Update every 3rd node in the 350-node single-bucket chain
  for(int i = 0; i < NUM_COLLIDING; i += 3){
    tbl.add(i * 1024, i + 5000);
  }

  // Remove all even-indexed nodes from the single-bucket chain
  for(int i = 0; i < NUM_COLLIDING; i += 2){
    int k = i * 1024;
    int expected_val = (i % 3 == 0) ? (i + 5000) : (i + 100);
    auto rem = tbl.remove(k);
    assert(rem.has_value() && rem.value() == expected_val);
    assert(!tbl.contains(k));
  }

  // Drain the remaining odd-indexed nodes to force shrink resizes (128 -> 64 -> 32) on bucket 0
  for(int i = 1; i < NUM_COLLIDING; i += 2){
    int k = i * 1024;
    int expected_val = (i % 3 == 0) ? (i + 5000) : (i + 100);
    assert(tbl.contains(k));
    auto rem = tbl.remove(k);
    assert(rem.has_value() && rem.value() == expected_val);
    assert(!tbl.contains(k));
  }
}

// ============================================================================
// 3. Sub-32 Initial Bucket Count & Rapid Empty-Table (0 <-> 1) Churn
// ============================================================================
void test_small_initial_bucket_count_and_rapid_empty_churn(){
  // Starts with num_buckets = 4 (< 32 minimum shrink floor)
  HashTable<int> tbl(4);

  for(int i = 0; i < 100; ++i){
    tbl.add(i, i * 10);
  }
  for(int i = 0; i < 100; ++i){
    auto rem = tbl.remove(i);
    assert(rem.has_value() && rem.value() == i * 10);
  }

  // Rapid concurrent add/remove on a nearly empty table (exercises sz_total 0 <-> 1 boundary)
  constexpr int NUM_THREADS = 8;
  vector<thread> churners;
  for(int t = 0; t < NUM_THREADS; ++t){
    churners.emplace_back([&tbl, t](){
      int k = t % 2;
      for(int iter = 0; iter < 1000; ++iter){
        tbl.add(k, iter * 10 + t);
        auto rem = tbl.remove(k);
        if(rem.has_value()){
          assert(rem.value() >= 0);
        }
      }
    });
  }
  for(auto & th : churners){
    th.join();
  }

  tbl.remove(0);
  tbl.remove(1);
  assert(!tbl.contains(0));
  assert(!tbl.contains(1));
}

// ============================================================================
// 4. Concurrent Resize Threshold Oscillation & Migration Retry Loop
// ============================================================================
void test_concurrent_resize_threshold_oscillation_and_migration_retry(){
  // Repeatedly crosses the expand (32 -> 64 at >160 items) and shrink (64 -> 32 at <7 items)
  // thresholds for 20 waves while background threads continuously hammer keys,
  // maximizing contention on `buckets.load()` and `maybe_guard.value().is_migrated()` retries.
  HashTable<int> tbl(32);
  atomic<bool> stop_bg{false};

  vector<thread> bg_threads;
  for(int t = 0; t < 4; ++t){
    bg_threads.emplace_back([&tbl, &stop_bg, t](){
      int k = 10000 + t;
      int iter = 0;
      while(!stop_bg.load(std::memory_order_relaxed)){
        tbl.add(k, int(iter));
        auto got = tbl.get(k);
        assert(got.has_value() && got.value() == iter);
        assert(tbl.contains(k));
        auto rem = tbl.remove(k);
        assert(rem.has_value() && rem.value() == iter);
        ++iter;
      }
    });
  }

  for(int wave = 0; wave < 20; ++wave){
    vector<thread> wave_threads;
    for(int t = 0; t < 4; ++t){
      wave_threads.emplace_back([&tbl, wave, t](){
        int base = t * 100;
        for(int i = 0; i < 45; ++i){
          int k = base + i;
          tbl.add(k, wave * 1000 + k);
        }
      });
    }
    for(auto & th : wave_threads){
      th.join();
    }

    vector<thread> drain_threads;
    for(int t = 0; t < 4; ++t){
      drain_threads.emplace_back([&tbl, wave, t](){
        int base = t * 100;
        for(int i = 0; i < 45; ++i){
          int k = base + i;
          auto rem = tbl.remove(k);
          assert(rem.has_value() && rem.value() == wave * 1000 + k);
        }
      });
    }
    for(auto & th : drain_threads){
      th.join();
    }
  }

  stop_bg.store(true, std::memory_order_relaxed);
  for(auto & bg : bg_threads){
    bg.join();
  }

  for(int t = 0; t < 4; ++t){
    assert(!tbl.contains(10000 + t));
  }
}

// ============================================================================
// 5. Large-Scale Move-Only Struct Stress Test (High-Volume Resize + Shared-Key Conservation)
// ============================================================================
void test_large_move_only_struct_high_scale_resize_and_contention(){
  HashTable<LargeMoveOnlyStruct> tbl(32);

  // Phase 1: High-scale expansion (32 -> 64 -> 128 -> 256 -> 512) and contraction (512 -> 32)
  // 8 threads * 250 keys = 2,000 move-only 4KB+heap objects (~8.5 MB of node payloads)
  constexpr int NUM_THREADS = 8;
  constexpr int KEYS_PER_THREAD = 250;

  vector<thread> writers;
  for(int t = 0; t < NUM_THREADS; ++t){
    writers.emplace_back([&tbl, t](){
      int base = t * 1000;
      for(int i = 0; i < KEYS_PER_THREAD; ++i){
        int k = base + i;
        tbl.add(k, LargeMoveOnlyStruct(k, 1));
      }
      // In-place move-assignment overwrite of all 2,000 keys with generation 2
      for(int i = 0; i < KEYS_PER_THREAD; ++i){
        int k = base + i;
        tbl.add(k, LargeMoveOnlyStruct(k, 2));
      }
    });
  }
  for(auto & w : writers){
    w.join();
  }

  // Concurrently drain all 2,000 keys (forces 512 -> 256 -> 128 -> 64 -> 32 contractions)
  // and verify every 4KB inline array + 64-word heap buffer upon removal
  vector<thread> removers;
  for(int t = 0; t < NUM_THREADS; ++t){
    removers.emplace_back([&tbl, t](){
      int base = t * 1000;
      for(int i = 0; i < KEYS_PER_THREAD; ++i){
        int k = base + i;
        assert(tbl.contains(k));
        auto rem = tbl.remove(k);
        assert(rem.has_value());
        assert(rem.value().verify(k));
        assert(rem.value().generation == 2);
        assert(!tbl.contains(k));
      }
    });
  }
  for(auto & r : removers){
    r.join();
  }

  // Phase 2: Shared-key contention + exact removal conservation with 4KB Move-Only Structs
  // All 8 threads race to insert/overwrite the same 400 keys (expanding 32 -> 64 -> 128),
  // then all 8 threads race to remove the same 400 keys (shrinking 128 -> 64 -> 32).
  // Verifies no duplicate key nodes and exact 1-to-1 removal conservation of move-only payloads.
  constexpr int SHARED_KEYS = 400;
  for(int round = 1; round <= 3; ++round){
    vector<thread> shared_inserters;
    for(int t = 0; t < NUM_THREADS; ++t){
      shared_inserters.emplace_back([&tbl, t, round](){
        for(int i = 0; i < SHARED_KEYS; ++i){
          int k = (i * 37 + t * 13) % SHARED_KEYS;
          uint64_t gen = static_cast<uint64_t>(round * 100 + t + 1);
          tbl.add(k, LargeMoveOnlyStruct(k, gen));
        }
      });
    }
    for(auto & th : shared_inserters){
      th.join();
    }

    vector<atomic<int>> key_remove_counts(SHARED_KEYS);
    for(int k = 0; k < SHARED_KEYS; ++k){
      key_remove_counts[k].store(0, std::memory_order_relaxed);
    }
    atomic<int> total_removed{0};

    vector<thread> shared_removers;
    for(int t = 0; t < NUM_THREADS; ++t){
      shared_removers.emplace_back([&tbl, &key_remove_counts, &total_removed, t, round](){
        for(int i = 0; i < SHARED_KEYS; ++i){
          int k = (i * 53 + t * 29) % SHARED_KEYS;
          auto rem = tbl.remove(k);
          if(rem.has_value()){
            assert(rem.value().verify(k));
            uint64_t gen = rem.value().generation;
            assert(gen >= static_cast<uint64_t>(round * 100 + 1) &&
                   gen <= static_cast<uint64_t>(round * 100 + NUM_THREADS));
            key_remove_counts[k].fetch_add(1, std::memory_order_relaxed);
            total_removed.fetch_add(1, std::memory_order_relaxed);
          }
        }
      });
    }
    for(auto & th : shared_removers){
      th.join();
    }

    assert(total_removed.load(std::memory_order_relaxed) == SHARED_KEYS);
    for(int k = 0; k < SHARED_KEYS; ++k){
      assert(key_remove_counts[k].load(std::memory_order_relaxed) == 1);
      assert(!tbl.contains(k));
    }
  }
}

// ============================================================================
// 6. Large-Scale Copyable+Movable Struct Stress Test (Concurrent Readers + Writers + Resizes)
// ============================================================================
void test_large_movable_copyable_struct_high_scale_readers_writers_resize(){
  // 8 writer threads * 250 keys = 2,000 4KB+heap structs (~8.5 MB) expanding 32 -> 512 and shrinking 512 -> 32
  // while 4 reader threads continuously call get(k) and verify the 4KB+heap checksum.
  // Because LargeMovableCopyableStruct zeroes its 4KB inline array and empties its heap vector on move,
  // any stale-bucket read, lock violation, or mid-write/mid-move race immediately fails verify(k).
  HashTable<LargeMovableCopyableStruct> tbl(32);
  constexpr int NUM_WRITERS = 8;
  constexpr int KEYS_PER_WRITER = 250;

  atomic<bool> stop_readers{false};
  vector<thread> readers;
  for(int r = 0; r < 4; ++r){
    readers.emplace_back([&tbl, &stop_readers, r](){
      while(!stop_readers.load(std::memory_order_relaxed)){
        for(int w = 0; w < NUM_WRITERS; ++w){
          int k = w * 1000 + ((r * 31) % KEYS_PER_WRITER);
          auto got = tbl.get(k);
          if(got.has_value()){
            assert(got.value().verify(k));
            assert(got.value().generation == 10 || got.value().generation == 20);
          }
        }
      }
    });
  }

  vector<thread> writers;
  for(int w = 0; w < NUM_WRITERS; ++w){
    writers.emplace_back([&tbl, w](){
      int base = w * 1000;
      // Insert 250 4KB+heap structs per thread (expands 32 -> 64 -> 128 -> 256 -> 512)
      for(int i = 0; i < KEYS_PER_WRITER; ++i){
        int k = base + i;
        tbl.add(k, LargeMovableCopyableStruct(k, 10));
      }
      // Overwrite all 250 structs per thread with generation 20
      for(int i = 0; i < KEYS_PER_WRITER; ++i){
        int k = base + i;
        tbl.add(k, LargeMovableCopyableStruct(k, 20));
      }
      // Remove the second half of each writer's keys while readers are active
      for(int i = KEYS_PER_WRITER / 2; i < KEYS_PER_WRITER; ++i){
        int k = base + i;
        auto rem = tbl.remove(k);
        assert(rem.has_value());
        assert(rem.value().verify(k));
        assert(rem.value().generation == 20);
      }
    });
  }

  for(auto & w : writers){
    w.join();
  }

  // Drain the remaining first half across threads (shrinks 512 -> 256 -> 128 -> 64 -> 32)
  vector<thread> drainers;
  for(int w = 0; w < NUM_WRITERS; ++w){
    drainers.emplace_back([&tbl, w](){
      int base = w * 1000;
      for(int i = 0; i < KEYS_PER_WRITER / 2; ++i){
        int k = base + i;
        auto got = tbl.get(k);
        assert(got.has_value() && got.value().verify(k) && got.value().generation == 20);
        auto rem = tbl.remove(k);
        assert(rem.has_value() && rem.value().verify(k) && rem.value().generation == 20);
        assert(!tbl.contains(k));
      }
    });
  }
  for(auto & d : drainers){
    d.join();
  }

  stop_readers.store(true, std::memory_order_relaxed);
  for(auto & r : readers){
    r.join();
  }
}

// ============================================================================
// 7. Large Struct Shared-Key Tug-of-War & Hot-Bucket Collision Storm
// ============================================================================
void test_large_struct_shared_key_tug_of_war_and_hot_collisions(){
  // 8 threads concurrently hammer the same 64 shared keys (half of which are multiples of 256
  // that collide in bucket 0) with add, remove, get, and contains using 4KB+heap structs,
  // while also validating post-storm state consistency.
  HashTable<LargeMovableCopyableStruct> tbl(32);
  constexpr int NUM_THREADS = 8;
  constexpr int SHARED_KEYS = 64;
  constexpr int OPS_PER_THREAD = 600;

  auto index_to_key = [](int idx) -> int {
    // Even indices collide in bucket 0 (multiples of 256); odd indices spread across buckets
    return (idx % 2 == 0) ? (idx * 256) : idx;
  };

  vector<thread> threads;
  for(int t = 0; t < NUM_THREADS; ++t){
    threads.emplace_back([&tbl, &index_to_key, t](){
      for(int iter = 1; iter <= OPS_PER_THREAD; ++iter){
        int idx = (iter + t * 7) % SHARED_KEYS;
        int k = index_to_key(idx);
        int op = (iter + t) % 4;
        if(op == 0){
          uint64_t gen = static_cast<uint64_t>(t * 10000 + iter);
          tbl.add(k, LargeMovableCopyableStruct(k, gen));
        } else if(op == 1){
          auto rem = tbl.remove(k);
          if(rem.has_value()){
            assert(rem.value().verify(k));
          }
        } else if(op == 2){
          auto got = tbl.get(k);
          if(got.has_value()){
            assert(got.value().verify(k));
          }
        } else {
          bool present = tbl.contains(k);
          (void)present;
        }
      }
    });
  }

  for(auto & th : threads){
    th.join();
  }

  // Post-storm consistency check: contains(), get(), and remove() must agree on every key
  for(int idx = 0; idx < SHARED_KEYS; ++idx){
    int k = index_to_key(idx);
    bool has = tbl.contains(k);
    auto got = tbl.get(k);
    assert(has == got.has_value());
    if(has){
      assert(got.value().verify(k));
      auto rem = tbl.remove(k);
      assert(rem.has_value());
      assert(rem.value().verify(k));
      assert(rem.value().generation == got.value().generation);
      assert(!tbl.contains(k));
      assert(!tbl.remove(k).has_value());
    } else {
      assert(!tbl.remove(k).has_value());
    }
  }
}

int main(){
  test_single_thread_crud_collisions_and_boundary_keys();
  test_pathological_single_bucket_chain_across_resizes();
  test_small_initial_bucket_count_and_rapid_empty_churn();
  test_concurrent_resize_threshold_oscillation_and_migration_retry();
  test_large_move_only_struct_high_scale_resize_and_contention();
  test_large_movable_copyable_struct_high_scale_readers_writers_resize();
  test_large_struct_shared_key_tug_of_war_and_hot_collisions();
  return 0;
}
