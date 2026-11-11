#pragma once

#include <memory>
#include <mutex>
#include <optional>

template <class TyInner> class MutexWrapper;

/// a type to indicate that we have ownership of the wrapped item
/// and it automatically releases ownership when done;
///
/// use dereference to accessing the wrapped item
template <class TyInner> class LockGuard {
public:
  // invariant on construction: assumed that bucket's mtx has been acquired with
  // lk_
  LockGuard(std::shared_ptr<MutexWrapper<TyInner>> wrapper,
            std::unique_lock<std::mutex> &&lk_)
      : mtx_wrapper(std::move(wrapper)), lk(std::move(lk_)) {}

  LockGuard(LockGuard const &) = delete;
  LockGuard &operator=(LockGuard const &) = delete;

  // moves are totally valid
  LockGuard(LockGuard &&) = default;
  LockGuard &operator=(LockGuard &&) = default;

  ~LockGuard() = default;

  // allow calling methods and accessing members via arrow
  // note: don't provide TyInner& since it's hard to restrict moving it in C++
  // if TyInner originally does allow it
  TyInner *operator->() noexcept { return &mtx_wrapper->inner; }
  TyInner const *operator->() const noexcept { return &mtx_wrapper->inner; }

private:
  std::shared_ptr<MutexWrapper<TyInner>> mtx_wrapper;

  std::unique_lock<std::mutex> lk;
};

template <class TyInner>
class MutexWrapper
    : public std::enable_shared_from_this<MutexWrapper<TyInner>> {
public:
  // not movable and not copyable; this is required since it is composed of a
  // mutex
  MutexWrapper(MutexWrapper const &) = delete;
  MutexWrapper &operator=(MutexWrapper const &) = delete;
  MutexWrapper(MutexWrapper &&) = delete;
  MutexWrapper &operator=(MutexWrapper &&) = delete;

  // perfect forwarding for inner wrapped type
  MutexWrapper(auto &&...args) : inner(std::forward<decltype(args)>(args)...) {}

  ~MutexWrapper() {}

  LockGuard<TyInner> lock() {
    std::unique_lock<std::mutex> lk(mtx);
    return LockGuard(this->shared_from_this(), std::move(lk));
  }

  std::optional<LockGuard<TyInner>> try_lock() {
    std::unique_lock<std::mutex> lk(mtx, std::defer_lock);
    if (lk.try_lock()) {
      return LockGuard(this->shared_from_this(), std::move(lk));
    }
    return std::nullopt;
  }

private:
  std::mutex mtx;

  TyInner inner;

  friend class LockGuard<TyInner>;
};
