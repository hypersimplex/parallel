#include <vector>
#include <deque>
#include <map>
#include <memory>
#include <cassert>
#include <utility>
#include <iostream>
#include <format>
#include <cstring>
#include <algorithm>
#include <random>
#include <cstdlib>
#include <cstdint>
#include <climits>
#include <atomic>
#include <mutex>
#include <thread>

#include "ht_closed_addr_lock_coarse_gained.hpp"

using namespace std;

void test_single_thread_crud_and_chain_collisions(){
  HashTable<int> tbl(64);

  // Keep > 6 anchor keys present so load factor stays > 0.1 (7/64 > 0.1) during single-bucket tests
  for(int i = 1; i <= 10; ++i){
    tbl.add(i, i * 10);
  }
  for(int i = 1; i <= 10; ++i){
    assert(tbl.contains(i));
    auto val = tbl.get(i);
    assert(val.has_value());
    assert(val.value() == i * 10);
  }
  assert(!tbl.contains(999));
  assert(!tbl.get(999).has_value());

  // Colliding keys in bucket 0 (multiples of 64: 64, 128, 192, 256, 320)
  vector<int> chain_keys = {64, 128, 192, 256, 320};
  for(int k : chain_keys){
    tbl.add(k, k + 1);
  }
  for(int k : chain_keys){
    assert(tbl.contains(k));
    auto val = tbl.get(k);
    assert(val.has_value());
    assert(val.value() == k + 1);
  }

  // Update existing keys at head, middle, and tail of the bucket chain
  for(int k : chain_keys){
    tbl.add(k, k * 2);
  }
  for(int k : chain_keys){
    auto val = tbl.get(k);
    assert(val.has_value());
    assert(val.value() == k * 2);
  }

  // Remove middle node (192), head of chain (320, inserted last), and tail of chain (64, inserted first)
  auto rem_192 = tbl.remove(192);
  assert(rem_192.has_value() && rem_192.value() == 384);
  assert(!tbl.contains(192));
  assert(!tbl.get(192).has_value());
  assert(!tbl.remove(192).has_value()); // idempotent remove of already-removed key

  auto rem_320 = tbl.remove(320);
  assert(rem_320.has_value() && rem_320.value() == 640);
  assert(!tbl.contains(320));
  assert(!tbl.get(320).has_value());

  auto rem_64 = tbl.remove(64);
  assert(rem_64.has_value() && rem_64.value() == 128);
  assert(!tbl.contains(64));
  assert(!tbl.get(64).has_value());

  // Remaining chain nodes (128, 256) must still be reachable
  assert(tbl.get(128).has_value() && tbl.get(128).value() == 256);
  assert(tbl.get(256).has_value() && tbl.get(256).value() == 512);

  // Re-insert removed keys and verify
  tbl.add(64, 640);
  tbl.add(192, 1920);
  assert(tbl.get(64).has_value() && tbl.get(64).value() == 640);
  assert(tbl.get(192).has_value() && tbl.get(192).value() == 1920);
}

void test_resize_up_and_down_cycles(){
  HashTable<int> tbl(32);
  map<int, int> expected;

  // Insert 700 items to force multiple expansions:
  // 32 -> 64 (at 160), 64 -> 128 (at 320), 128 -> 256 (at 640)
  for(int i = 0; i < 700; ++i){
    int val = i * 10 + 1;
    tbl.add(i, std::move(val));
    expected[i] = i * 10 + 1;
  }

  // Overwrite every even key across the expanded table
  for(int i = 0; i < 700; i += 2){
    int val = i * 100 + 2;
    tbl.add(i, std::move(val));
    expected[i] = i * 100 + 2;
  }

  for(auto const & [k, v] : expected){
    auto res = tbl.get(k);
    assert(res.has_value());
    assert(res.value() == v);
  }

  // Remove 695 of 700 items to force multiple contractions:
  // 256 -> 128 (at 25), 128 -> 64 (at 12), 64 -> 32 (at 6)
  for(int i = 0; i < 695; ++i){
    tbl.remove(i);
    expected.erase(i);
    assert(!tbl.get(i).has_value());
  }

  for(auto const & [k, v] : expected){
    auto res = tbl.get(k);
    assert(res.has_value());
    assert(res.value() == v);
  }

  // Remove remaining 5 items at minimum capacity (32)
  for(int i = 695; i < 700; ++i){
    tbl.remove(i);
    expected.erase(i);
    assert(!tbl.get(i).has_value());
  }

  // Re-populate after full drain to verify table remains usable after shrinking to min capacity
  for(int i = 1000; i < 1200; ++i){
    int val = i * 5;
    tbl.add(i, std::move(val));
    expected[i] = i * 5;
  }
  for(auto const & [k, v] : expected){
    auto res = tbl.get(k);
    assert(res.has_value());
    assert(res.value() == v);
  }
}

void test_concurrent_disjoint_and_overlapping_writers_readers(){
  // Large initial bucket count (256) so 8 threads * 120 keys = 960 < 256 * 5 (no resize)
  HashTable<int> tbl(256);
  constexpr int NUM_THREADS = 8;
  constexpr int KEYS_PER_THREAD = 120;

  // Phase 1: Disjoint writers + concurrent readers
  vector<thread> writers;
  vector<thread> readers;
  atomic<bool> stop_readers{false};

  for(int t = 0; t < 4; ++t){
    readers.emplace_back([&tbl, &stop_readers, t](){
      while(!stop_readers.load(std::memory_order_relaxed)){
        for(int i = 0; i < KEYS_PER_THREAD; ++i){
          int k = t * 1000 + i;
          auto res = tbl.get(k);
          if(res.has_value()){
            int v = res.value();
            assert(v == k + 1 || v == k + 2);
          }
        }
      }
    });
  }

  for(int t = 0; t < NUM_THREADS; ++t){
    writers.emplace_back([&tbl, t](){
      int base = t * 1000;
      for(int i = 0; i < KEYS_PER_THREAD; ++i){
        int k = base + i;
        tbl.add(k, k + 1);
      }
      for(int i = 0; i < KEYS_PER_THREAD; ++i){
        int k = base + i;
        tbl.add(k, k + 2);
      }
    });
  }

  for(auto & w : writers){
    w.join();
  }
  stop_readers.store(true, std::memory_order_relaxed);
  for(auto & r : readers){
    r.join();
  }

  for(int t = 0; t < NUM_THREADS; ++t){
    int base = t * 1000;
    for(int i = 0; i < KEYS_PER_THREAD; ++i){
      int k = base + i;
      auto res = tbl.get(k);
      assert(res.has_value());
      assert(res.value() == k + 2);
    }
  }

  // Phase 2: Overlapping hot-spot contention on the same 16 colliding keys
  vector<thread> hot_threads;
  for(int t = 0; t < NUM_THREADS; ++t){
    hot_threads.emplace_back([&tbl, t](){
      for(int iter = 0; iter < 200; ++iter){
        int k = (iter % 16) * 256; // all hash to bucket 0
        tbl.add(k, t * 10000 + iter);
        auto res = tbl.get(k);
        assert(res.has_value());
      }
    });
  }
  for(auto & th : hot_threads){
    th.join();
  }

  for(int j = 0; j < 16; ++j){
    int k = j * 256;
    auto res = tbl.get(k);
    assert(res.has_value());
    int v = res.value();
    int tid = v / 10000;
    int iter = v % 10000;
    assert(tid >= 0 && tid < NUM_THREADS);
    assert(iter >= 0 && iter < 200);
  }
}

void test_concurrent_mixed_add_get_remove_churn(){
  HashTable<int> tbl(128);

  // Insert 64 permanent anchor keys (keeps load factor > 0.1 so bucket chains are stressed in-place)
  for(int i = 0; i < 64; ++i){
    tbl.add(i, i * 100);
  }

  constexpr int NUM_THREADS = 8;
  constexpr int CHURN_KEYS_PER_THREAD = 50;
  vector<thread> workers;

  for(int t = 0; t < NUM_THREADS; ++t){
    workers.emplace_back([&tbl, t](){
      int base = 1000 + t * 1000;
      for(int round = 0; round < 5; ++round){
        for(int i = 0; i < CHURN_KEYS_PER_THREAD; ++i){
          int k = base + i;
          tbl.add(k, round * 1000 + i);
          auto r1 = tbl.get(k);
          assert(r1.has_value() && r1.value() == round * 1000 + i);

          // Update in-place before removing
          tbl.add(k, round * 2000 + i);
          auto r2 = tbl.get(k);
          assert(r2.has_value() && r2.value() == round * 2000 + i);

          // Verify an anchor key in the middle of churn
          int anchor = (t * 8 + (i % 8)) % 64;
          auto ra = tbl.get(anchor);
          assert(ra.has_value() && ra.value() == anchor * 100);

          tbl.remove(k);
          assert(!tbl.get(k).has_value());
        }
      }
    });
  }

  for(auto & w : workers){
    w.join();
  }

  // Verify all anchor keys survived intact and all churn keys were removed
  for(int i = 0; i < 64; ++i){
    auto ra = tbl.get(i);
    assert(ra.has_value() && ra.value() == i * 100);
  }
  for(int t = 0; t < NUM_THREADS; ++t){
    int base = 1000 + t * 1000;
    for(int i = 0; i < CHURN_KEYS_PER_THREAD; ++i){
      assert(!tbl.get(base + i).has_value());
    }
  }
}

void test_concurrent_resize_up_and_down_stress(){
  HashTable<int> tbl(32);
  constexpr int NUM_THREADS = 8;
  constexpr int ITEMS_PER_THREAD = 200; // 8 * 200 = 1600 items -> expands 32 -> 64 -> 128 -> 256 -> 512

  atomic<bool> phase_done{false};
  // Background reader thread continuously querying keys during concurrent resizes
  thread reader([&tbl, &phase_done](){
    while(!phase_done.load(std::memory_order_relaxed)){
      for(int t = 0; t < NUM_THREADS; ++t){
        int k = t * 1000 + (t * 17) % ITEMS_PER_THREAD;
        auto res = tbl.get(k);
        if(res.has_value()){
          assert(res.value() == k * 3 || res.value() == k * 7);
        }
      }
    }
  });

  // Wave 1: Concurrent expansion + updates
  vector<thread> expand_threads;
  for(int t = 0; t < NUM_THREADS; ++t){
    expand_threads.emplace_back([&tbl, t](){
      int base = t * 1000;
      for(int i = 0; i < ITEMS_PER_THREAD; ++i){
        int k = base + i;
        tbl.add(k, k * 3);
      }
      for(int i = 0; i < ITEMS_PER_THREAD; ++i){
        int k = base + i;
        tbl.add(k, k * 7);
      }
    });
  }
  for(auto & th : expand_threads){
    th.join();
  }

  for(int t = 0; t < NUM_THREADS; ++t){
    int base = t * 1000;
    for(int i = 0; i < ITEMS_PER_THREAD; ++i){
      int k = base + i;
      auto res = tbl.get(k);
      assert(res.has_value());
      assert(res.value() == k * 7);
    }
  }

  // Wave 2: Concurrent contraction down to 16 keys, then full drain
  vector<thread> shrink_threads;
  for(int t = 0; t < NUM_THREADS; ++t){
    shrink_threads.emplace_back([&tbl, t](){
      int base = t * 1000;
      for(int i = 2; i < ITEMS_PER_THREAD; ++i){
        tbl.remove(base + i);
      }
    });
  }
  for(auto & th : shrink_threads){
    th.join();
  }

  // Remove the last 16 keys concurrently (shrinks down to min cap 32 and triggers try_resize_down at cap 32)
  vector<thread> drain_threads;
  for(int t = 0; t < NUM_THREADS; ++t){
    drain_threads.emplace_back([&tbl, t](){
      int base = t * 1000;
      tbl.remove(base);
      tbl.remove(base + 1);
    });
  }
  for(auto & th : drain_threads){
    th.join();
  }

  phase_done.store(true, std::memory_order_relaxed);
  reader.join();

  for(int t = 0; t < NUM_THREADS; ++t){
    int base = t * 1000;
    for(int i = 0; i < ITEMS_PER_THREAD; ++i){
      assert(!tbl.get(base + i).has_value());
    }
  }
}

void test_boundary_and_negative_keys_across_resizes(){
  HashTable<int> tbl(32);
  map<int, int> expected;

  vector<int> boundary_keys = {
    INT_MIN, INT_MIN + 1, INT_MIN + 2,
    -1000000, -65536, -1024, -64, -32, -2, -1,
    0, 1, 2, 32, 64, 1024, 65536, 1000000,
    INT_MAX - 2, INT_MAX - 1, INT_MAX
  };

  for(int k : boundary_keys){
    int v = (k == INT_MIN) ? -42 : (k ^ 0x5a5a5a5a);
    int v_copy = v;
    tbl.add(k, std::move(v_copy));
    expected[k] = v;
  }

  // Also insert 200 negative sequential keys to trigger resize up (32 -> 64) with sign-extended uint64_t keys
  for(int i = -500; i < -300; ++i){
    int v = i * 3;
    tbl.add(i, std::move(v));
    expected[i] = i * 3;
  }

  for(auto const & [k, v] : expected){
    assert(tbl.contains(k));
    auto res = tbl.get(k);
    assert(res.has_value());
    assert(res.value() == v);
  }

  // Overwrite boundary keys after expansion
  for(int k : boundary_keys){
    int v = (k == INT_MAX) ? 99 : (k ^ 0x12345678);
    int v_copy = v;
    tbl.add(k, std::move(v_copy));
    expected[k] = v;
  }

  // Remove the 200 negative sequential keys to trigger resize down (64 -> 32) while keeping boundary keys
  for(int i = -500; i < -300; ++i){
    auto rem = tbl.remove(i);
    assert(rem.has_value());
    assert(rem.value() == expected[i]);
    expected.erase(i);
    assert(!tbl.contains(i));
    assert(!tbl.get(i).has_value());
  }

  // Verify all boundary keys survived resize up and resize down intact
  for(int k : boundary_keys){
    assert(tbl.contains(k));
    auto res = tbl.get(k);
    assert(res.has_value());
    assert(res.value() == expected[k]);

    auto rem = tbl.remove(k);
    assert(rem.has_value());
    assert(rem.value() == expected[k]);
    assert(!tbl.contains(k));
    assert(!tbl.remove(k).has_value());
  }
}

void test_pathological_single_bucket_chain_across_resizes(){
  HashTable<int> tbl(32);

  // All multiples of 1024 hash to bucket 0 for bucket counts 32, 64, 128, 256, 512, 1024.
  // Inserting 350 such keys forces expansions (32 -> 64 at 161, 64 -> 128 at 321)
  // where all 350 nodes remain in bucket 0's linked list and reverse chain order on each resize!
  constexpr int NUM_COLLIDING = 350;
  for(int i = 0; i < NUM_COLLIDING; ++i){
    int k = i * 1024;
    tbl.add(k, i + 100);
  }

  for(int i = 0; i < NUM_COLLIDING; ++i){
    int k = i * 1024;
    assert(tbl.contains(k));
    auto res = tbl.get(k);
    assert(res.has_value() && res.value() == i + 100);
  }

  // Update every 3rd node in the 350-node single-bucket chain
  for(int i = 0; i < NUM_COLLIDING; i += 3){
    int k = i * 1024;
    tbl.add(k, i + 5000);
  }

  // Remove all even-indexed nodes from the single-bucket chain (exercises head, middle, and tail unlinks)
  for(int i = 0; i < NUM_COLLIDING; i += 2){
    int k = i * 1024;
    int expected_val = (i % 3 == 0) ? (i + 5000) : (i + 100);
    auto rem = tbl.remove(k);
    assert(rem.has_value() && rem.value() == expected_val);
    assert(!tbl.contains(k));
    assert(!tbl.remove(k).has_value());
  }

  // Verify all odd-indexed nodes in the chain are still reachable with exact values
  for(int i = 1; i < NUM_COLLIDING; i += 2){
    int k = i * 1024;
    int expected_val = (i % 3 == 0) ? (i + 5000) : (i + 100);
    assert(tbl.contains(k));
    auto res = tbl.get(k);
    assert(res.has_value() && res.value() == expected_val);
  }

  // Drain the remaining odd-indexed nodes to force shrink resizes (128 -> 64 -> 32) on the single chain
  for(int i = 1; i < NUM_COLLIDING; i += 2){
    int k = i * 1024;
    int expected_val = (i % 3 == 0) ? (i + 5000) : (i + 100);
    auto rem = tbl.remove(k);
    assert(rem.has_value() && rem.value() == expected_val);
    assert(!tbl.contains(k));
  }
}

void test_concurrent_shared_key_tug_of_war(){
  // 8 threads concurrently hammer the exact same 32 keys with add, remove, get, and contains
  HashTable<int> tbl(32);
  constexpr int NUM_THREADS = 8;
  constexpr int SHARED_KEYS = 32;
  constexpr int OPS_PER_THREAD = 1500;

  auto is_valid_val_for_key = [](int k, int v) -> bool {
    int encoded_k = v & 0xff;
    int magic = (v >> 8) & 0xff;
    return encoded_k == (k & 0xff) && magic == 0x5a;
  };

  vector<thread> threads;
  for(int t = 0; t < NUM_THREADS; ++t){
    threads.emplace_back([&tbl, &is_valid_val_for_key, t](){
      for(int iter = 0; iter < OPS_PER_THREAD; ++iter){
        int k = (iter + t * 7) % SHARED_KEYS;
        int op = (iter + t) % 4;
        if(op == 0){
          int val = (iter << 16) | (0x5a << 8) | (k & 0xff);
          tbl.add(k, std::move(val));
        } else if(op == 1){
          auto rem = tbl.remove(k);
          if(rem.has_value()){
            assert(is_valid_val_for_key(k, rem.value()));
          }
        } else if(op == 2){
          auto res = tbl.get(k);
          if(res.has_value()){
            assert(is_valid_val_for_key(k, res.value()));
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

  // Post-storm consistency check: for each key, contains(), get(), and remove() must agree
  for(int k = 0; k < SHARED_KEYS; ++k){
    bool has = tbl.contains(k);
    auto got = tbl.get(k);
    assert(has == got.has_value());
    if(has){
      assert(is_valid_val_for_key(k, got.value()));
      auto rem = tbl.remove(k);
      assert(rem.has_value());
      assert(rem.value() == got.value());
      assert(!tbl.contains(k));
      assert(!tbl.remove(k).has_value());
    } else {
      assert(!tbl.remove(k).has_value());
    }
  }
}

void test_concurrent_heap_values_move_integrity_during_resize(){
  // Uses heap-allocated std::vector<int> values so that std::move(node->val) inside resize()
  // empties moved-from vectors in old buckets. Any stale-bucket read or use-after-move will
  // observe an empty vector (size() == 0 instead of 16) and immediately fail the assertion.
  HashTable<vector<int>> tbl(32);
  constexpr int NUM_WRITERS = 6;
  constexpr int KEYS_PER_WRITER = 120; // 720 keys total -> forces 32 -> 64 -> 128 -> 256
  constexpr size_t VEC_LEN = 16;

  auto make_vec = [](int k, int multiplier) -> vector<int> {
    vector<int> v(VEC_LEN);
    for(size_t i = 0; i < VEC_LEN; ++i){
      v[i] = k * multiplier + static_cast<int>(i);
    }
    return v;
  };

  auto check_vec = [](int k, vector<int> const & v) -> bool {
    if(v.size() != VEC_LEN){
      return false;
    }
    for(size_t i = 0; i < VEC_LEN; ++i){
      int expected1 = k * 2 + static_cast<int>(i);
      int expected2 = k * 5 + static_cast<int>(i);
      if(v[i] != expected1 && v[i] != expected2){
        return false;
      }
    }
    return true;
  };

  atomic<bool> stop_readers{false};
  vector<thread> readers;
  for(int r = 0; r < 4; ++r){
    readers.emplace_back([&tbl, &stop_readers, &check_vec, r](){
      while(!stop_readers.load(std::memory_order_relaxed)){
        for(int w = 0; w < NUM_WRITERS; ++w){
          int k = w * 1000 + ((r * 31) % KEYS_PER_WRITER);
          auto res = tbl.get(k);
          if(res.has_value()){
            assert(check_vec(k, res.value()));
          }
        }
      }
    });
  }

  vector<thread> writers;
  for(int w = 0; w < NUM_WRITERS; ++w){
    writers.emplace_back([&tbl, &make_vec, &check_vec, w](){
      int base = w * 1000;
      for(int i = 0; i < KEYS_PER_WRITER; ++i){
        int k = base + i;
        tbl.add(k, make_vec(k, 2));
      }
      for(int i = 0; i < KEYS_PER_WRITER; ++i){
        int k = base + i;
        tbl.add(k, make_vec(k, 5));
      }
      // Remove the second half of each writer's keys to force shrink resizes while readers are active
      for(int i = KEYS_PER_WRITER / 2; i < KEYS_PER_WRITER; ++i){
        int k = base + i;
        auto rem = tbl.remove(k);
        assert(rem.has_value());
        assert(check_vec(k, rem.value()));
      }
    });
  }

  for(auto & w : writers){
    w.join();
  }
  stop_readers.store(true, std::memory_order_relaxed);
  for(auto & r : readers){
    r.join();
  }

  for(int w = 0; w < NUM_WRITERS; ++w){
    int base = w * 1000;
    for(int i = 0; i < KEYS_PER_WRITER / 2; ++i){
      int k = base + i;
      auto res = tbl.get(k);
      assert(res.has_value());
      assert(res.value() == make_vec(k, 5));
    }
    for(int i = KEYS_PER_WRITER / 2; i < KEYS_PER_WRITER; ++i){
      int k = base + i;
      assert(!tbl.contains(k));
      assert(!tbl.get(k).has_value());
    }
  }
}

void test_concurrent_resize_threshold_oscillation_and_migration_retry(){
  // Repeatedly crosses the expand (32 -> 64 at >160 items) and shrink (64 -> 32 at <7 items)
  // thresholds for 20 cycles while background threads continuously hammer buckets,
  // maximizing contention on `buckets.load()` and `maybe_guard.value().is_migrated()` retries.
  HashTable<int> tbl(32);
  atomic<bool> stop_bg{false};

  // 4 background worker threads continuously reading/updating/removing in key range [10000, 10003]
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

  // Oscillate table size between ~0 and 180 items for 20 waves using 4 wave threads
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

void test_small_initial_bucket_count_and_rapid_empty_churn(){
  // Starts with num_buckets = 4 (< 32 minimum shrink floor) and stresses both
  // expansion/contraction from a sub-32 bucket count and rapid concurrent 0 <-> 1
  // item empty-table insert/remove churn on the same key.
  HashTable<int> tbl(4);

  for(int i = 0; i < 100; ++i){
    tbl.add(i, i * 10);
  }
  for(int i = 0; i < 100; ++i){
    assert(tbl.contains(i));
    auto res = tbl.get(i);
    assert(res.has_value() && res.value() == i * 10);
  }
  for(int i = 0; i < 100; ++i){
    auto rem = tbl.remove(i);
    assert(rem.has_value() && rem.value() == i * 10);
    assert(!tbl.contains(i));
  }

  // Rapid concurrent add/remove on an almost-empty table (exercises sz_total 0 <-> 1 boundary)
  constexpr int NUM_THREADS = 8;
  vector<thread> churners;
  for(int t = 0; t < NUM_THREADS; ++t){
    churners.emplace_back([&tbl, t](){
      int k = t % 2; // 4 threads per key on a nearly empty table
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

void test_concurrent_exact_remove_conservation_and_no_duplicate_nodes(){
  // Verifies two strict concurrency invariants across simultaneous resizes:
  // 1. No duplicate nodes for the same key: when 8 threads concurrently add/overwrite
  //    the same shared keys [0, 300) while forcing table expansions, each key must exist
  //    at most once in its bucket chain (a single remove(k) must completely remove it).
  // 2. Exact removal conservation: when 8 threads concurrently race to remove every key
  //    in [0, 300) while forcing table contractions, each key must be successfully
  //    removed bylero-or-one thread, and the total successful remove count across all 8
  //    threads must equal 300 exactly (no lost removes and no double removes).
  HashTable<int> tbl(32);
  constexpr int NUM_THREADS = 8;
  constexpr int SHARED_KEY_COUNT = 300;

  for(int round = 0; round < 5; ++round){
    // Phase 1: All 8 threads race to insert/overwrite the same 300 keys (triggers 32 -> 64)
    vector<thread> inserters;
    for(int t = 0; t < NUM_THREADS; ++t){
      inserters.emplace_back([&tbl, t, round](){
        for(int i = 0; i < SHARED_KEY_COUNT; ++i){
          // Permute traversal order per thread to maximize collision & resize interleaving
          int k = (i * 37 + t * 13) % SHARED_KEY_COUNT;
          tbl.add(k, round * 100000 + t * 1000 + k);
        }
      });
    }
    for(auto & th : inserters){
      th.join();
    }

    // All 300 keys must be present
    for(int k = 0; k < SHARED_KEY_COUNT; ++k){
      assert(tbl.contains(k));
      auto got = tbl.get(k);
      assert(got.has_value());
      assert((got.value() % 1000) == k);
    }

    // Phase 2: All 8 threads race to remove the exact same 300 keys (triggers 64 -> 32)
    vector<atomic<int>> key_remove_counts(SHARED_KEY_COUNT);
    for(int k = 0; k < SHARED_KEY_COUNT; ++k){
      key_remove_counts[k].store(0, std::memory_order_relaxed);
    }
    atomic<int> total_removed{0};

    vector<thread> removers;
    for(int t = 0; t < NUM_THREADS; ++t){
      removers.emplace_back([&tbl, &key_remove_counts, &total_removed, t](){
        for(int i = 0; i < SHARED_KEY_COUNT; ++i){
          int k = (i * 53 + t * 29) % SHARED_KEY_COUNT;
          auto rem = tbl.remove(k);
          if(rem.has_value()){
            assert((rem.value() % 1000) == k);
            key_remove_counts[k].fetch_add(1, std::memory_order_relaxed);
            total_removed.fetch_add(1, std::memory_order_relaxed);
          }
        }
      });
    }
    for(auto & th : removers){
      th.join();
    }

    // Exactly 300 removals must have succeeded across all 8 threads (1 per key, no duplicates)
    assert(total_removed.load(std::memory_order_relaxed) == SHARED_KEY_COUNT);
    for(int k = 0; k < SHARED_KEY_COUNT; ++k){
      assert(key_remove_counts[k].load(std::memory_order_relaxed) == 1);
      assert(!tbl.contains(k));
      assert(!tbl.get(k).has_value());
      assert(!tbl.remove(k).has_value());
    }
  }
}

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
    if(owner_key != expected_key || !heap_buf){
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
    if(owner_key != expected_key || heap_vec.size() != 64){
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

void test_concurrent_large_movable_structs(){
  // Part 1: Strict move-only 4KB+heap struct (copy constructor deleted)
  // Exercises add (insert + overwrite), contains, remove (returns optional by move), and resize (up & down)
  {
    HashTable<LargeMoveOnlyStruct> tbl(32);
    constexpr int NUM_THREADS = 6;
    constexpr int KEYS_PER_THREAD = 80; // 480 total -> expands 32 -> 64 -> 128

    vector<thread> writers;
    for(int t = 0; t < NUM_THREADS; ++t){
      writers.emplace_back([&tbl, t](){
        int base = t * 1000;
        for(int i = 0; i < KEYS_PER_THREAD; ++i){
          int k = base + i;
          tbl.add(k, LargeMoveOnlyStruct(k, 1));
        }
        // Overwrite all keys with generation 2 while resizing
        for(int i = 0; i < KEYS_PER_THREAD; ++i){
          int k = base + i;
          tbl.add(k, LargeMoveOnlyStruct(k, 2));
        }
      });
    }
    for(auto & w : writers){
      w.join();
    }

    // Concurrently remove all keys and verify every 4KB move-only payload + generation == 2
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
  }

  // Part 2: Large 4KB+heap movable & copyable struct with destructive move (zeroes source on move)
  // Exercises concurrent add, get, contains, remove, and resize
  {
    HashTable<LargeMovableCopyableStruct> tbl(32);
    constexpr int NUM_THREADS = 6;
    constexpr int KEYS_PER_THREAD = 80;

    atomic<bool> stop_readers{false};
    vector<thread> readers;
    for(int r = 0; r < 4; ++r){
      readers.emplace_back([&tbl, &stop_readers, r](){
        while(!stop_readers.load(std::memory_order_relaxed)){
          for(int t = 0; t < NUM_THREADS; ++t){
            int k = t * 1000 + ((r * 19) % KEYS_PER_THREAD);
            auto got = tbl.get(k);
            if(got.has_value()){
              // Verifies no tearing and no read of a moved-from (zeroed) struct during resize
              assert(got.value().verify(k));
              assert(got.value().generation == 10 || got.value().generation == 20);
            }
          }
        }
      });
    }

    vector<thread> workers;
    for(int t = 0; t < NUM_THREADS; ++t){
      workers.emplace_back([&tbl, t](){
        int base = t * 1000;
        for(int i = 0; i < KEYS_PER_THREAD; ++i){
          int k = base + i;
          tbl.add(k, LargeMovableCopyableStruct(k, 10));
        }
        for(int i = 0; i < KEYS_PER_THREAD; ++i){
          int k = base + i;
          tbl.add(k, LargeMovableCopyableStruct(k, 20));
        }
        for(int i = 0; i < KEYS_PER_THREAD; ++i){
          int k = base + i;
          auto rem = tbl.remove(k);
          assert(rem.has_value());
          assert(rem.value().verify(k));
          assert(rem.value().generation == 20);
        }
      });
    }

    for(auto & w : workers){
      w.join();
    }
    stop_readers.store(true, std::memory_order_relaxed);
    for(auto & r : readers){
      r.join();
    }
  }
}

int main(){
  test_single_thread_crud_and_chain_collisions();
  test_resize_up_and_down_cycles();
  test_concurrent_disjoint_and_overlapping_writers_readers();
  test_concurrent_mixed_add_get_remove_churn();
  test_concurrent_resize_up_and_down_stress();
  test_boundary_and_negative_keys_across_resizes();
  test_pathological_single_bucket_chain_across_resizes();
  test_concurrent_shared_key_tug_of_war();
  test_concurrent_heap_values_move_integrity_during_resize();
  test_concurrent_resize_threshold_oscillation_and_migration_retry();
  test_small_initial_bucket_count_and_rapid_empty_churn();
  test_concurrent_exact_remove_conservation_and_no_duplicate_nodes();
  test_concurrent_large_movable_structs();
  return 0;
}
