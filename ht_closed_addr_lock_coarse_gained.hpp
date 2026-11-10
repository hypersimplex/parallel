//! coarse grained closed address hash table with a lock per bucket
//! during resizing, the table is blocked until new table is allocated and items
//! rehashed
//!
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <utility>
#include <vector>

using namespace std;

/// lists of nodes in a bucket
template <class TyV>
  requires std::movable<TyV>
struct Node {
  uint64_t key;
  TyV val;
  Node *prev = nullptr;
  Node *next = nullptr;

  static Node *list_find(Node *node, uint64_t const k) {
    while (node) {
      if (k == node->key) {
        return node;
      }
      node = node->next;
    }
    return nullptr;
  }

  static void list_insert_after(Node *insertion_point, Node *node) {
    if (insertion_point->next) {
      insertion_point->next->prev = node;
    }
    node->next = insertion_point->next;
    node->prev = insertion_point;

    insertion_point->next = node;
  }

  // assumes node->prev always exists
  static void list_unlink(Node *node) {
    node->prev->next = node->next;
    if (node->next) {
      node->next->prev = node->prev;
    }
  }
};

template <class TyV>
  requires std::movable<TyV>
class Bucket;

/// a type to indicate that we have ownership of the bucket
/// and it automatically releases ownership when done;
/// it also provides interface for accessing bucket operations
template <class TyV>
  requires std::movable<TyV>
class LockGuard {
public:
  // invariant on construction: assumed that bucket's mtx has been acquired with
  // lk_
  LockGuard(shared_ptr<Bucket<TyV>> b, std::unique_lock<std::mutex> &&lk_)
      : bucket(b), lk(std::move(lk_)) {}
  LockGuard() = default;

  LockGuard(LockGuard const &) = delete;
  LockGuard &operator=(LockGuard &) = delete;

  LockGuard(LockGuard &&) = default;
  LockGuard &operator=(LockGuard &&) = default;

  ~LockGuard() = default;

  // return true if new item is added
  // return false if existing item is modified or no bucket exists
  bool insert(uint64_t k, TyV &&v) {
    if (!bucket) {
      return false;
    }
    auto *node = find(k);
    if (node) {
      node->val = std::move(v);
      return false;
    }
    auto new_node = new Node<TyV>;
    new_node->val = std::move(v);
    new_node->key = k;
    Node<TyV>::list_insert_after(get_head(), new_node);
    return true;
  }

  // return erased value if it exists
  optional<TyV> erase(uint64_t k) {
    if (!bucket) {
      return nullopt;
    }
    auto *node = find(k);
    if (!node) {
      return nullopt;
    }
    Node<TyV>::list_unlink(node);
    auto ret = std::move(node->val);
    delete node;
    return optional<TyV>{std::in_place, std::move(ret)};
  }

  Node<TyV> *find(uint64_t k) {
    if (!bucket) {
      return nullptr;
    }
    return Node<TyV>::list_find(get_head()->next, k);
  }

  Node<TyV> *get_head() { return bucket->head; }

  void set_migrated() {
    if (!bucket) {
      return;
    }
    bucket->migrated = true;
  }

  bool is_migrated() const {
    if (!bucket) {
      return false;
    }
    return bucket->migrated;
  }

  shared_ptr<Bucket<TyV>> bucket;
  std::unique_lock<std::mutex> lk;
};

template <class TyV>
  requires std::movable<TyV>
class Bucket : public std::enable_shared_from_this<Bucket<TyV>> {
public:
  // not movable and not copyable; this is required since it is composed of a
  // mutex
  Bucket(Bucket const &) = delete;
  Bucket &operator=(Bucket &) = delete;
  Bucket(Bucket &&) = delete;
  Bucket &operator=(Bucket &&) = delete;

  Bucket() {
    migrated = false;
    head = new Node<TyV>;
  }

  ~Bucket() {
    // todo: ensure anything else is visiting the chain linked with head
    auto node = head;
    while (node) {
      auto next = node->next;
      delete node;
      node = next;
    }
  }

  LockGuard<TyV> lock() {
    std::unique_lock<std::mutex> lk(mtx);
    return LockGuard(this->shared_from_this(), std::move(lk));
  }

  optional<LockGuard<TyV>> try_lock() {
    std::unique_lock<std::mutex> lk(mtx, std::defer_lock);
    if (lk.try_lock()) {
      return LockGuard(this->shared_from_this(), std::move(lk));
    }
    return nullopt;
  }

private:
  // sentinil head node always exist for ease of node list manipulation
  Node<TyV> *head;

  std::mutex mtx;

  // a market indicating that threads are looking at an old bucket
  // and that threads need to retry and obtain an more up-to-date
  // bucket
  bool migrated;

  // let this type have special access due to ownership
  friend class LockGuard<TyV>;
};

// open address table with linked list chain for each slot in the hash table
template <class TyV>
  requires std::movable<TyV>
class HashTable {
public:
  HashTable(HashTable const &) = delete;
  HashTable &operator=(HashTable &) = delete;
  HashTable(HashTable &&) = delete;
  HashTable &operator=(HashTable &&) = delete;

  HashTable(uint64_t num_buckets) {
    resizing = false;
    sz_total = 0;
    sz_buckets = num_buckets;
    auto buckets_shared = make_shared<vector<shared_ptr<Bucket<TyV>>>>();
    for (int i = 0; i < sz_buckets; ++i) {
      // initialize bucket with a head node
      buckets_shared->emplace_back(std::make_shared<Bucket<TyV>>());
    }
    buckets.store(buckets_shared);
  }
  void add(int key, TyV &&val) {
    while (true) {
      // we use the pattern of atomically loading the current set
      // of buckets and operating on these
      //
      // if stale bucket is detected, we retry loading
      auto buckets_current = buckets.load();
      auto idx = hash_to_bucket_idx(buckets_current, key);
      if (!idx) {
        continue;
      }
      bool new_insert{false};
      {
        auto maybe_guard = (*buckets_current)[idx.value()]->try_lock();
        if (!maybe_guard) {
          continue;
        }
        if (maybe_guard.value().is_migrated()) {
          continue;
        }
        new_insert = maybe_guard.value().insert(key, std::move(val));
      }
      if (new_insert) {
        sz_total.fetch_add(1, std::memory_order_relaxed);
        try_resize_up();
      }
      return;
    }
  }

  // cloning get, since it's usually dangerous to give back a reference in
  // multi-threaded context
  optional<TyV> get(int const &key) {
    while (true) {
      auto buckets_current = buckets.load();
      auto idx = hash_to_bucket_idx(buckets_current, key);
      if (!idx) {
        continue;
      }
      auto maybe_guard = (*buckets_current)[idx.value()]->try_lock();
      if (!maybe_guard) {
        continue;
      }
      if (maybe_guard.value().is_migrated()) {
        continue;
      }
      auto node = maybe_guard.value().find(key);
      if (!node) {
        return nullopt;
      }
      // just do a copy for now
      return node->val;
    }
  }

  bool contains(int const &key) {
    while (true) {
      auto buckets_current = buckets.load();
      auto idx = hash_to_bucket_idx(buckets_current, key);
      if (!idx) {
        continue;
      }
      auto maybe_guard = (*buckets_current)[idx.value()]->try_lock();
      if (!maybe_guard) {
        continue;
      }
      if (maybe_guard.value().is_migrated()) {
        continue;
      }
      auto node = maybe_guard.value().find(key);
      return node != nullptr;
    }
  }

  // returns removed item if it exists
  optional<TyV> remove(int const &key) {
    while (true) {
      auto buckets_current = buckets.load();
      auto idx = hash_to_bucket_idx(buckets_current, key);
      if (!idx) {
        continue;
      }
      optional<TyV> ret;
      {
        auto maybe_guard = (*buckets_current)[idx.value()]->try_lock();
        if (!maybe_guard) {
          continue;
        }
        if (maybe_guard.value().is_migrated()) {
          continue;
        }
        ret = std::move(maybe_guard.value().erase(key));
      }
      if (ret) {
        sz_total.fetch_sub(1, std::memory_order_relaxed);
        try_resize_down();
      }
      return ret;
    }
  }

private:
  void resize() {
    bool expected = false;

    if (resizing.compare_exchange_strong(expected, true,
                                         std::memory_order_acq_rel,
                                         std::memory_order_relaxed)) {

      uint64_t sz_buckets_new;

      float cur_sz = sz_total.load(std::memory_order_relaxed);
      float cur_sz_buckets = sz_buckets.load(std::memory_order_relaxed);
      float avg_per_bucket = cur_sz / cur_sz_buckets;
      if (avg_per_bucket > THRES_EXPAND_AVG_BUCKET_LEN) {
        sz_buckets_new = sz_buckets * 2;
      } else if (avg_per_bucket < THRES_SHRINK_AVG_BUCKET_LEN) {
        sz_buckets_new = max(sz_buckets / 2, static_cast<uint64_t>(32));
      } else {
        resizing.store(false, std::memory_order_release);
        return;
      }

      if (sz_buckets == sz_buckets_new) {
        resizing.store(false, std::memory_order_release);
        return;
      }

      // resize

      vector<shared_ptr<Bucket<TyV>>> new_buckets;
      for (int i = 0; i < sz_buckets_new; ++i) {
        new_buckets.push_back(make_shared<Bucket<TyV>>());
      }

      {
        std::vector<LockGuard<TyV>> locked_guards;
        // obtain locks
        auto cur_buckets = buckets.load();
        for (auto &bucket : *cur_buckets) {
          locked_guards.push_back(std::move(bucket->lock()));
        }

        for (auto &guard : locked_guards) {
          auto node = guard.get_head()->next;
          while (node) {
            std::hash<uint64_t> h{};
            uint64_t idx = h(node->key) % sz_buckets_new;
            {
              new_buckets[idx]->lock().insert(node->key, std::move(node->val));
            }
            node = node->next;
          }
        }

        buckets.store(
            make_shared<decltype(new_buckets)>(std::move(new_buckets)));

        sz_buckets = sz_buckets_new;

        // signal to threads working on old buckets to retry
        for (auto &guard : locked_guards) {
          guard.set_migrated();
        }

        // unlocked when locked_guards are dropped
      }
      resizing.store(false, std::memory_order_release);
    }
  }
  void try_resize_up() {
    float cur_sz = sz_total.load(std::memory_order_relaxed);
    float cur_sz_buckets = sz_buckets.load(std::memory_order_relaxed);
    float avg_per_bucket = cur_sz / cur_sz_buckets;
    if (avg_per_bucket < THRES_EXPAND_AVG_BUCKET_LEN) {
      return;
    }
    resize();
  }

  void try_resize_down() {
    float cur_sz = sz_total.load(std::memory_order_relaxed);
    float cur_sz_buckets = sz_buckets.load(std::memory_order_relaxed);
    float avg_per_bucket = cur_sz / cur_sz_buckets;
    if (avg_per_bucket > THRES_SHRINK_AVG_BUCKET_LEN) {
      return;
    }
    resize();
  }

  std::optional<size_t>
  hash_to_bucket_idx(shared_ptr<vector<shared_ptr<Bucket<TyV>>>> cur_buckets,
                     uint64_t const &key) {
    if (resizing.load(std::memory_order_acquire)) {
      return nullopt;
    }
    std::hash<uint64_t> h{};
    return h(key) % cur_buckets->size();
  }

  std::atomic<bool> resizing;

  static constexpr float THRES_EXPAND_AVG_BUCKET_LEN = 5.0f;
  static constexpr float THRES_SHRINK_AVG_BUCKET_LEN = 0.1f;

  std::atomic<uint64_t> sz_buckets;

  // use signed to avoid temporary (under/over)flow
  std::atomic<int64_t> sz_total;

  std::atomic<shared_ptr<vector<shared_ptr<Bucket<TyV>>>>> buckets;
};
