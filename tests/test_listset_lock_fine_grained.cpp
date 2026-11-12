#include <atomic>
#include <cassert>
#include <climits>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#include "listset_lock_fine_grained.hpp"

using namespace std;

// ============================================================================
// Large Movable Object (2 KB inline payload + dynamic heap vector)
// ============================================================================

struct LargeMovableObject {
  static constexpr size_t BUFFER_WORDS = 256; // 2 KB inline buffer
  uint64_t buffer[BUFFER_WORDS]{};
  vector<uint64_t> heap_vec;
  int key = 0;

  LargeMovableObject() = default;

  explicit LargeMovableObject(int k) : heap_vec(32), key(k) {
    for (size_t i = 0; i < BUFFER_WORDS; ++i) {
      buffer[i] = (static_cast<uint64_t>(k) << 32) ^ (i * 2654435761ULL);
    }
    for (size_t i = 0; i < 32; ++i) {
      heap_vec[i] = buffer[i] ^ 0x5a5a5a5a5a5a5a5aULL;
    }
  }

  LargeMovableObject(LargeMovableObject const &) = default;
  LargeMovableObject &operator=(LargeMovableObject const &) = default;

  LargeMovableObject(LargeMovableObject &&other) noexcept
      : heap_vec(std::move(other.heap_vec)), key(other.key) {
    for (size_t i = 0; i < BUFFER_WORDS; ++i) {
      buffer[i] = other.buffer[i];
      other.buffer[i] = 0;
    }
    other.key = -1;
  }

  LargeMovableObject &operator=(LargeMovableObject &&other) noexcept {
    if (this != &other) {
      heap_vec = std::move(other.heap_vec);
      key = other.key;
      for (size_t i = 0; i < BUFFER_WORDS; ++i) {
        buffer[i] = other.buffer[i];
        other.buffer[i] = 0;
      }
      other.key = -1;
    }
    return *this;
  }

  bool verify(int expected_k) const {
    if (key != expected_k || heap_vec.size() != 32) {
      return false;
    }
    for (size_t i = 0; i < BUFFER_WORDS; ++i) {
      if (buffer[i] != ((static_cast<uint64_t>(expected_k) << 32) ^ (i * 2654435761ULL))) {
        return false;
      }
    }
    return true;
  }

  bool operator==(LargeMovableObject const &o) const { return key == o.key; }
  auto operator<=>(LargeMovableObject const &o) const { return key <=> o.key; }
};

static_assert(std::movable<LargeMovableObject>);
static_assert(std::totally_ordered<LargeMovableObject>);
static_assert(std::default_initializable<LargeMovableObject>);

// ============================================================================
// 1. Single-Threaded Combinations: Empty List, Positions, Duplicates, Extremes
// ============================================================================
void test_sequential_combinations() {
  ListSet<int> l;

  // Operations on empty list
  assert(!l.contains(10));
  assert(!l.remove(10));

  // Insertions: middle, head (smaller key), tail (larger key)
  assert(l.add(20));
  assert(l.add(10)); // new head
  assert(l.add(30)); // new tail

  // Duplicate rejection across all positions
  assert(!l.add(10));
  assert(!l.add(20));
  assert(!l.add(30));

  // Extreme boundaries and interior misses
  assert(l.add(int(INT_MIN)));
  assert(l.add(int(INT_MAX)));
  assert(!l.contains(15));
  assert(!l.remove(15));

  // Removals: middle, head, tail, and extremes
  assert(l.remove(20));
  assert(l.remove(10));
  assert(l.remove(30));
  assert(l.remove(INT_MIN));
  assert(l.remove(INT_MAX));

  // Double-remove and emptiness check
  assert(!l.remove(20));
  assert(!l.contains(20));
}

// ============================================================================
// 2. Concurrent Shared Contention: Add/Remove Conservation & Linearizability
// ============================================================================
void test_concurrent_shared_key_contention() {
  ListSet<int> l;
  constexpr int NUM_THREADS = 8;
  constexpr int NUM_KEYS = 100;

  for (int round = 0; round < 2; ++round) {
    vector<atomic<int>> add_wins(NUM_KEYS);
    vector<atomic<int>> rem_wins(NUM_KEYS);

    // Threads race to add the same keys: exactly one must win per key
    vector<thread> adders;
    for (int t = 0; t < NUM_THREADS; ++t) {
      adders.emplace_back([&l, &add_wins, t]() {
        for (int i = 0; i < NUM_KEYS; ++i) {
          int k = (i * 17 + t * 7) % NUM_KEYS;
          if (l.add(int(k))) {
            add_wins[k].fetch_add(1, memory_order_relaxed);
          }
          assert(l.contains(k));
        }
      });
    }
    for (auto &th : adders) {
      th.join();
    }

    for (int k = 0; k < NUM_KEYS; ++k) {
      assert(add_wins[k].load(memory_order_relaxed) == 1);
    }

    // Threads race to remove the same keys: exactly one must win per key
    vector<thread> removers;
    for (int t = 0; t < NUM_THREADS; ++t) {
      removers.emplace_back([&l, &rem_wins, t]() {
        for (int i = 0; i < NUM_KEYS; ++i) {
          int k = (i * 31 + t * 11) % NUM_KEYS;
          if (l.remove(k)) {
            rem_wins[k].fetch_add(1, memory_order_relaxed);
          }
        }
      });
    }
    for (auto &th : removers) {
      th.join();
    }

    for (int k = 0; k < NUM_KEYS; ++k) {
      assert(rem_wins[k].load(memory_order_relaxed) == 1);
      assert(!l.contains(k));
    }
  }
}

// ============================================================================
// 3. Concurrent Stress with Large Movable Objects (2 KB payload + heap buffer)
// ============================================================================
void test_concurrent_large_movable_objects_stress() {
  ListSet<LargeMovableObject> l;
  constexpr int NUM_THREADS = 8;
  constexpr int KEY_DOMAIN = 64;
  constexpr int OPS_PER_THREAD = 400;

  // Single-item move semantics verification
  LargeMovableObject initial_obj(999);
  assert(initial_obj.verify(999));
  assert(l.add(std::move(initial_obj)));
  assert(!initial_obj.verify(999)); // Source moved-from and invalidated
  assert(l.contains(LargeMovableObject(999)));
  assert(l.remove(LargeMovableObject(999)));
  assert(!l.contains(LargeMovableObject(999)));

  // Concurrent mixed operations (add via move, contains, remove)
  vector<thread> workers;
  for (int t = 0; t < NUM_THREADS; ++t) {
    workers.emplace_back([&l, t]() {
      for (int iter = 0; iter < OPS_PER_THREAD; ++iter) {
        int k = (iter * 13 + t * 7) % KEY_DOMAIN;
        int op = (iter + t) % 3;

        if (op == 0) {
          LargeMovableObject item(k);
          l.add(std::move(item));
        } else if (op == 1) {
          LargeMovableObject query(k);
          l.contains(query);
        } else {
          LargeMovableObject target(k);
          l.remove(target);
        }
      }
    });
  }
  for (auto &th : workers) {
    th.join();
  }

  // Post-storm consistency check and drain
  for (int k = 0; k < KEY_DOMAIN; ++k) {
    LargeMovableObject target(k);
    assert(l.contains(target) == l.remove(target));
    assert(!l.contains(target));
  }
}

int main() {
  test_sequential_combinations();
  test_concurrent_shared_key_contention();
  test_concurrent_large_movable_objects_stress();
  return 0;
}
