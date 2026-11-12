//! coarse grained closed address hash table with a lock per bucket
//! during resizing, the table is blocked until new table is allocated and items
//! rehashed
//!

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <utility>
#include <vector>

#include "common.hpp"

// open address table with linked list chain for each slot in the hash table
// along with a lock per bucket
template <class TyV>
  requires std::movable<TyV>
class HashTable {

  // internal constructs

  /// node type for keeping track of items in a bucket
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

  class OwnedBucket {
  public:
    // disallow copying or moving for simplicity
    OwnedBucket(OwnedBucket const &) = delete;
    OwnedBucket &operator=(OwnedBucket const &) = delete;
    OwnedBucket(OwnedBucket &&other) = delete;
    OwnedBucket &operator=(OwnedBucket &&other) = delete;

    // signal there exists a more up-to-date bucket than the current bucket
    void set_migrated() { migrated = true; }

    bool is_migrated() const { return migrated; }

    // return true if new item is added
    // return false if existing item is modified or no bucket exists
    bool insert(uint64_t k, TyV &&v) {
      auto *node = find(k);
      if (node) {
        node->val = std::move(v);
        return false;
      }
      auto new_node = new Node;
      new_node->val = std::move(v);
      new_node->key = k;
      Node::list_insert_after(head, new_node);
      return true;
    }

    // return erased value if it exists
    std::optional<TyV> erase(uint64_t k) {
      auto *node = find(k);
      if (!node) {
        return std::nullopt;
      }
      Node::list_unlink(node);
      auto ret = std::move(node->val);
      delete node;
      return std::optional<TyV>{std::in_place, std::move(ret)};
    }

    Node *find(uint64_t k) { return Node::list_find(head->next, k); }

    Node *get_head() { return head; }

    OwnedBucket() {
      migrated = false;
      head = new Node;
    }

    ~OwnedBucket() {
      if (!head) {
        return;
      }
      auto node = head;
      while (node) {
        auto next = node->next;
        delete node;
        node = next;
      }
    }

  private:
    // sentinil head node always exist for ease of node list manipulation
    Node *head;

    // a mark indicating that threads are looking at an old bucket
    // and that threads need to retry and obtain an more up-to-date
    // bucket
    bool migrated;
  };

public:
  HashTable(HashTable const &) = delete;
  HashTable &operator=(HashTable const &) = delete;
  HashTable(HashTable &&) = delete;
  HashTable &operator=(HashTable &&) = delete;

  HashTable(uint64_t num_buckets) {
    resizing = false;
    sz_total = 0;
    sz_buckets = num_buckets;
    auto buckets_shared = std::make_shared<
        std::vector<std::shared_ptr<MutexWrapper<OwnedBucket>>>>();
    for (int i = 0; i < sz_buckets; ++i) {
      // initialize bucket with a head node
      buckets_shared->emplace_back(
          std::make_shared<MutexWrapper<OwnedBucket>>());
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
        auto &guard = maybe_guard.value();
        if (guard->is_migrated()) {
          continue;
        }
        new_insert = guard->insert(key, std::move(val));
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
  std::optional<TyV> get(int const &key) {
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
      auto &guard = maybe_guard.value();
      if (guard->is_migrated()) {
        continue;
      }
      auto node = guard->find(key);
      if (!node) {
        return std::nullopt;
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
      auto &guard = maybe_guard.value();
      if (guard->is_migrated()) {
        continue;
      }
      auto node = guard->find(key);
      return node != nullptr;
    }
  }

  // returns removed item if it exists
  std::optional<TyV> remove(int const &key) {
    while (true) {
      auto buckets_current = buckets.load();
      auto idx = hash_to_bucket_idx(buckets_current, key);
      if (!idx) {
        continue;
      }
      std::optional<TyV> ret;
      {
        auto maybe_guard = (*buckets_current)[idx.value()]->try_lock();
        if (!maybe_guard) {
          continue;
        }
        auto &guard = maybe_guard.value();
        if (guard->is_migrated()) {
          continue;
        }
        ret = std::move(guard->erase(key));
      }
      if (ret) {
        sz_total.fetch_sub(1, std::memory_order_relaxed);
        try_resize_down();
      }
      return ret;
    }
  }

private:
  // lock all buckets and rehash existing items onto new table
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
        sz_buckets_new = std::max(sz_buckets / 2, static_cast<uint64_t>(32));
      } else {
        resizing.store(false, std::memory_order_release);
        return;
      }

      if (sz_buckets == sz_buckets_new) {
        resizing.store(false, std::memory_order_release);
        return;
      }

      // resize

      std::vector<std::shared_ptr<MutexWrapper<OwnedBucket>>> new_buckets;
      for (int i = 0; i < sz_buckets_new; ++i) {
        new_buckets.push_back(std::make_shared<MutexWrapper<OwnedBucket>>());
      }

      {
        // obtain locks to all existing buckets
        std::vector<LockGuard<OwnedBucket>> locked_guards;

        auto cur_buckets = buckets.load();
        for (auto &bucket : *cur_buckets) {
          locked_guards.push_back(std::move(bucket->lock()));
        }

        for (auto &guard : locked_guards) {
          auto node = guard->get_head()->next;
          while (node) {
            std::hash<uint64_t> h{};
            uint64_t idx = h(node->key) % sz_buckets_new;
            {
              new_buckets[idx]->lock()->insert(node->key, std::move(node->val));
            }
            node = node->next;
          }
        }

        // make new buckets effective
        buckets.store(
            std::make_shared<decltype(new_buckets)>(std::move(new_buckets)));

        sz_buckets = sz_buckets_new;

        // signal to threads working on old buckets to retry
        for (auto &guard : locked_guards) {
          guard->set_migrated();
        }

        // unlocked existing buckets when locked_guards are dropped
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

  std::optional<size_t> hash_to_bucket_idx(
      std::shared_ptr<std::vector<std::shared_ptr<MutexWrapper<OwnedBucket>>>>
          cur_buckets,
      uint64_t const &key) {
    if (resizing.load(std::memory_order_acquire)) {
      return std::nullopt;
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

  // atomic to make thread observe effects/updates and retry logic
  //
  // in practice, it seems most compiler implementation uses a mutex under the
  // hood
  std::atomic<
      std::shared_ptr<std::vector<std::shared_ptr<MutexWrapper<OwnedBucket>>>>>
      buckets;
};
