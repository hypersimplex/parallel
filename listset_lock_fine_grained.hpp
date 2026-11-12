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

#include <concepts>
#include <functional>
#include <memory>
#include <utility>

#include "common.hpp"

template <class Ty>
  requires std::movable<Ty> && std::totally_ordered<Ty> &&
           std::default_initializable<Ty>
class ListSet {
  class Node;

  using ProtectedNode = MutexWrapper<Node>;

  class Node {
  public:
    Node(auto &&...arg)
        : next(nullptr), val(std::forward<decltype(arg)>(arg)...) {}
    std::shared_ptr<ProtectedNode> next;
    Ty val;
  };

public:
  ListSet() {
    // sentinil node; requires Ty to be default constructible
    head = std::make_shared<ProtectedNode>();
  }

  // make this simple and explicit for safety
  ListSet(ListSet const &) = delete;
  ListSet &operator=(ListSet const &) = delete;
  ListSet(ListSet &&) = delete;
  ListSet &operator=(ListSet &&) = delete;

  // return true if added
  // return false if it exists and no modification is done
  bool add(Ty &&item) {
    return find(item, [&item](auto prev, auto cur) {
      auto [shared_ptr_raw, raw_ptr_prev] = prev;
      auto [shared_ptr_cur, raw_ptr_cur] = cur;

      // we have locks on both prev (possibly null) and cur

      if (raw_ptr_cur && raw_ptr_cur->val == item) {
        // already exists
        return false;
      }

      // does not exist; add new node in between prev and cur (possibly null for
      // tail)

      auto new_node = std::make_shared<ProtectedNode>(std::move(item));
      auto guard = new_node->lock();

      raw_ptr_prev->next = new_node;
      guard->next = shared_ptr_cur;

      return true;
    });
  }
  // return true if an existing matching item found
  // return false if it doesn't exist
  bool contains(Ty const &item) {
    return find(item, [&item](auto prev, auto cur) {
      auto [shared_ptr_raw, raw_ptr_prev] = prev;
      auto [shared_ptr_cur, raw_ptr_cur] = cur;
      // we have locks on both prev and cur
      return raw_ptr_cur && raw_ptr_cur->val == item;
    });
  }
  // return true if removed an existing matching item
  // return false if it doesn't exist
  bool remove(Ty const &item) {
    return find(item, [&item](auto prev, auto cur) {
      auto [shared_ptr_raw, raw_ptr_prev] = prev;
      auto [shared_ptr_cur, raw_ptr_cur] = cur;

      // we have locks on both prev and cur

      // empty case
      if (!raw_ptr_cur) {
        return false;
      }

      // non-empty

      if (raw_ptr_cur->val != item) {
        // non-empty but not a match
        return false;
      }

      // non-empty and is a match

      // unlink current node from list
      raw_ptr_prev->next = raw_ptr_cur->next;

      return true;
    });
  }

private:
  // find the first node in the list that is >= than the key
  // return (prev_node, node)
  bool find(
      Ty const &item,
      std::function<bool(std::pair<std::shared_ptr<ProtectedNode>, Node *> prev,
                         std::pair<std::shared_ptr<ProtectedNode>, Node *> cur)>
          &&f) {
    // keep 2 adjacent locks as we move through the linked list
    auto prev = head;
    auto guard_prev = prev->lock();
    Node *prev_node = guard_prev.operator->();

    auto cur = guard_prev->next;

    while (cur) {
      // obtain next node's lock
      auto guard_cur = cur->lock();

      Node *cur_node = guard_cur.operator->();

      if (cur_node->val >= item) {
        return f({prev, prev_node}, {cur, cur_node});
      }
      // keep searching

      prev = cur;
      prev_node = cur_node;

      cur = guard_cur->next;

      // drop previous guard but keep current guard by moving to guard_prev
      guard_prev = std::move(guard_cur);
    }
    // not found: pass in empty for cur
    return f({prev, prev_node}, {{}, nullptr});
  }

  // internally keep an ordering (<=) wrt. Ty
  std::shared_ptr<ProtectedNode> head;
};
