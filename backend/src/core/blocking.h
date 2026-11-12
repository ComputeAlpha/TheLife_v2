#pragma once

#include <drogon/utils/coroutine.h>
#include <trantor/net/EventLoop.h>
#include <trantor/utils/ConcurrentTaskQueue.h>

#include <coroutine>
#include <exception>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

namespace thelife {

// Shared pool for CPU-heavy or blocking work (argon2id, file IO) so event-loop threads stay responsive.
trantor::ConcurrentTaskQueue& blockingPool();

namespace detail {
template <typename T>
struct BlockingAwaiter : drogon::CallbackAwaiter<T> {
  std::function<T()> fn;
  explicit BlockingAwaiter(std::function<T()> f) : fn(std::move(f)) {}
  void await_suspend(std::coroutine_handle<> h) {
    // Resume on the event loop that awaited, if there is one.
    auto* loop = trantor::EventLoop::getEventLoopOfCurrentThread();
    blockingPool().runTaskInQueue([this, h, loop] {
      try {
        if constexpr (std::is_void_v<T>) {
          fn();
        } else {
          this->setValue(fn());
        }
      } catch (...) {
        this->setException(std::current_exception());
      }
      if (loop) {
        loop->queueInLoop([h] { h.resume(); });
      } else {
        h.resume();
      }
    });
  }
};
}  // namespace detail

// co_await runBlocking([]{ return expensive(); });  — runs fn on blockingPool(), resumes on the caller's loop.
template <typename F>
auto runBlocking(F&& fn) {
  using T = std::invoke_result_t<F>;
  return detail::BlockingAwaiter<T>(std::function<T()>(std::forward<F>(fn)));
}

}  // namespace thelife
