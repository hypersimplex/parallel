#include <atomic>
#include <cassert>
#include <climits>
#include <thread>
#include <vector>

#include "listset_lockfree.hpp"

using namespace std;

// TODO: remove once deferred node reclamation is implemented in listset_lockfree.hpp
extern "C" int __lsan_is_turned_off() { return 1; }

struct MoveOnly {
  int key = 0;

  MoveOnly() = default;
  explicit MoveOnly(int k) : key(k) {}
  MoveOnly(MoveOnly const &) = delete;
  MoveOnly &operator=(MoveOnly const &) = delete;
  MoveOnly(MoveOnly &&) = default;
  MoveOnly &operator=(MoveOnly &&) = default;

  auto operator<=>(MoveOnly const &) const = default;
};

void test_sequential() {
  ListSet<int> l;

  assert(!l.contains(0));
  assert(!l.remove(0));

  assert(l.add(10));
  assert(l.add(0));
  assert(l.add(20));
  assert(l.add(int(INT_MIN)));
  assert(l.add(int(INT_MAX)));

  assert(l.contains(0));
  assert(l.contains(10));
  assert(l.contains(20));
  assert(!l.add(0));
  assert(!l.add(10));
  assert(!l.add(int(INT_MAX)));
  assert(!l.contains(15));
  assert(!l.remove(15));

  assert(l.remove(10));
  assert(l.remove(INT_MIN));
  assert(l.remove(INT_MAX));
  assert(l.remove(0));
  assert(l.remove(20));
  assert(!l.remove(10));
  assert(!l.contains(10));

  assert(l.add(10));
  assert(l.contains(10));
  assert(l.remove(10));
  assert(!l.contains(10));
}

void test_move_only() {
  ListSet<MoveOnly> l;

  assert(l.add(MoveOnly(2)));
  assert(l.add(MoveOnly(1)));
  assert(!l.add(MoveOnly(2)));
  assert(l.contains(MoveOnly(1)));
  assert(l.remove(MoveOnly(1)));
  assert(!l.contains(MoveOnly(1)));
}

void test_concurrent_shared_keys() {
  ListSet<int> l;
  constexpr int NUM_THREADS = 8;
  constexpr int NUM_KEYS = 100;

  vector<atomic<int>> add_wins(NUM_KEYS);
  vector<atomic<int>> rem_wins(NUM_KEYS);

  vector<thread> threads;
  for (int t = 0; t < NUM_THREADS; ++t) {
    threads.emplace_back([&l, &add_wins, t]() {
      for (int i = 0; i < NUM_KEYS; ++i) {
        int k = (t % 2 == 0) ? i : (NUM_KEYS - 1 - i);
        if (l.add(int(k))) {
          add_wins[k].fetch_add(1, memory_order_relaxed);
        }
        assert(l.contains(k));
      }
    });
  }
  for (auto &th : threads) {
    th.join();
  }
  for (int k = 0; k < NUM_KEYS; ++k) {
    assert(add_wins[k].load(memory_order_relaxed) == 1);
  }

  threads.clear();
  for (int t = 0; t < NUM_THREADS; ++t) {
    threads.emplace_back([&l, &rem_wins, t]() {
      for (int i = 0; i < NUM_KEYS; ++i) {
        int k = (t % 2 == 0) ? i : (NUM_KEYS - 1 - i);
        if (l.remove(k)) {
          rem_wins[k].fetch_add(1, memory_order_relaxed);
        }
      }
    });
  }
  for (auto &th : threads) {
    th.join();
  }
  for (int k = 0; k < NUM_KEYS; ++k) {
    assert(rem_wins[k].load(memory_order_relaxed) == 1);
    assert(!l.contains(k));
  }
}

void test_concurrent_disjoint_keys() {
  ListSet<int> l;
  constexpr int NUM_THREADS = 8;
  constexpr int KEYS_PER_THREAD = 100;
  constexpr int ROUNDS = 2;

  vector<thread> threads;
  for (int t = 0; t < NUM_THREADS; ++t) {
    threads.emplace_back([&l, t]() {
      for (int r = 0; r < ROUNDS; ++r) {
        for (int i = 0; i < KEYS_PER_THREAD; ++i) {
          int k = i * NUM_THREADS + t;
          assert(l.add(int(k)));
          assert(l.contains(k));
        }
        for (int i = 0; i < KEYS_PER_THREAD; ++i) {
          int k = i * NUM_THREADS + t;
          assert(l.contains(k));
          assert(l.remove(k));
          assert(!l.contains(k));
        }
      }
    });
  }
  for (auto &th : threads) {
    th.join();
  }
  for (int k = 0; k < NUM_THREADS * KEYS_PER_THREAD; ++k) {
    assert(!l.contains(k));
  }
}

int main() {
  test_sequential();
  test_move_only();
  test_concurrent_shared_keys();
  test_concurrent_disjoint_keys();
  return 0;
}
