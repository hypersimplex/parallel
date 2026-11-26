//! construct to provide a set of item via a linked list
//! that is safe for multi-threaded contexts
//!
//! it requires the type:
//!   - have a total ordering
//!   - default initializable
//!   - movable
//!
//! the container provides the following functionalities:
//!   - add (non-overwriting)
//!   - remove
//!   - contains

#pragma once

#include <atomic>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#include "common.hpp"

template <class Ty>
  requires std::movable<Ty> && std::totally_ordered<Ty> &&
           std::default_initializable<Ty>
class ListSet {
public:
  // marked = not active / not alive
  // unmarked = active/alive
  static constexpr uintptr_t MASK_MARK = (1 << 3) - 1;
  static constexpr uintptr_t MASK_PTR = ~MASK_MARK;
  struct alignas(8) Node {
    std::atomic<Node *> next = nullptr;
    Ty item{};
  };

  ListSet() { head = new Node; }

  ~ListSet() {
    // this assumes threads are all done
    auto cur = head.load();
    while (cur) {
      auto next_aug = cur->next.load();
      auto [next, mark] = get_ptr_and_mark(next_aug);
      delete cur;
      cur = next;
    }
  }

  // attempt to add item
  // return true if added
  // return false if it already exists
  bool add(Ty &&item) {
    //   if cur == item, then abort
    //   else, add item between prev and cur

    auto new_node = new Node;
    new_node->item = std::move(item);

    while (true) {
      auto [prev, cur] = find(new_node->item);

      if (cur && cur->item == new_node->item) {
        // exists, early exit
        delete new_node;
        return false;
      }

      Node *cur_aug = set_ptr_mark_node_active(cur);

      new_node->next = cur_aug;

      Node *new_node_aug = set_ptr_mark_node_active(new_node);

      Node *expected = cur_aug;
      if (prev->next.compare_exchange_strong(expected, new_node_aug,
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
        // success
        return true;
      }
      // failed, retry
    }
    std::unreachable();
  }

  bool remove(Ty const &item) {
    while (true) {
      auto [prev, cur] = find(item);
      // check if cur == item, if so remove cur
      // else, not found => return false
      if (!cur || cur->item != item) {
        // not exist or not match
        return false;
      }
      // cur exists and match, so remove cur by logically setting a flag

      Node *next_aug = cur->next.load(std::memory_order_acquire);
      auto [next, mark] = get_ptr_and_mark(next_aug);
      if (mark) {
        // retry
        continue;
      }
      Node *desired = set_ptr_mark_node_inactive(next);
      if (cur->next.compare_exchange_strong(next_aug, desired)) {
        // success
        return true;
      }
      // retry
    }
    std::unreachable();
  }

  bool contains(Ty const &item) {
    auto [prev, cur] = find(item);
    return cur && (cur->item == item);
  }

  // find (prev, cur) ptrs such that cur is the first in the list that
  // is >= item
  //
  // if not found, return (prev, nullptr)
  std::pair<Node *, Node *> find(Ty const &item) {
    while (true) {
      bool retry{false};

      Node *prev = head.load(std::memory_order::acquire);

      Node *cur = prev->next.load(std::memory_order_acquire);
      {
        auto [next, mark] = get_ptr_and_mark(cur);
        cur = next;
      }

      while (cur) {
        auto next_aug = cur->next.load(std::memory_order_acquire);
        auto [next, mark] = get_ptr_and_mark(next_aug);

        if (mark) {
          // current node is not alive, so try removing it via a CAS

          Node *expected = set_ptr_mark_node_active(cur);
          Node *desired = set_ptr_mark_node_active(next);
          if (prev->next.compare_exchange_strong(expected, desired,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_relaxed)) {
            // success
            // [wip/todo]: defer physical deletion of cur until it is safe;
            // eg: need to add cur to a container and use quiescent state
            // reclaimation to recycle memory
            cur = next;
            continue;
          } else {
            // retry from start
            retry = true;
            break;
          }
        }
        // current node is alive

        if (cur->item >= item) {
          // found
          return {prev, cur};
        }

        // keep searching
        prev = cur;
        cur = next;
      }
      if (retry) {
        continue;
      }
      // reached end of list
      return {prev, nullptr};
    }
  }

private:
  // store active flag in the lowest bit of aligned pointer

  static Node *set_ptr_mark_node_inactive(Node *node) {
    return reinterpret_cast<Node *>(
        (reinterpret_cast<uintptr_t>(node) & MASK_PTR) | 0b1);
  }

  static Node *set_ptr_mark_node_active(Node *node) {
    return reinterpret_cast<Node *>(reinterpret_cast<uintptr_t>(node) &
                                    MASK_PTR);
  }

  static std::pair<Node *, bool> get_ptr_and_mark(Node *node_augmented) {
    return {reinterpret_cast<Node *>(
                reinterpret_cast<uintptr_t>(node_augmented) & MASK_PTR),
            (reinterpret_cast<uintptr_t>(node_augmented) & MASK_MARK) != 0};
  }

  std::atomic<Node *> head;

  std::vector<Node *> defer_deallocation;
};
