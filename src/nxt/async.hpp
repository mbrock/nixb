#pragma once

#include <coro/coro.hpp>
#include <coro/event.hpp>
#include <coro/io_scheduler.hpp>
#include <coro/queue.hpp>
#include <coro/task.hpp>

namespace nxb
{

  template <typename T = void> using task = coro::task<T>;

  template <typename T> using queue = coro::queue<T>;

  using event = coro::event;

  using io_scheduler = coro::io_scheduler;

  using poll_op = coro::poll_op;

  inline auto
  sync_wait (auto &&awaitable)
  {
    return coro::sync_wait (
      std::forward<decltype (awaitable)> (awaitable));
  }

  inline auto
  when_all (auto &&tasks)
  {
    return coro::when_all (std::forward<decltype (tasks)> (tasks));
  }

  /// Schedule a task to run on the given scheduler.
  /// Use this instead of co_await scheduler.schedule() at the top
  /// of coroutines.
  inline auto
  start (io_scheduler &sched, task<> t)
  {
    return sched.schedule (std::move (t));
  }

  /// A group of tasks that run on a scheduler.
  /// Add tasks with start(), then run_all() to execute them.
  class task_group
  {
  public:
    explicit task_group (io_scheduler &sched) : sched_ (sched) {}

    template <typename... Tasks>
    task_group (io_scheduler &sched, Tasks &&...tasks)
        : sched_ (sched)
    {
      (tasks_.push_back (
         nxb::start (sched_, std::forward<Tasks> (tasks))),
       ...);
    }

    void
    start (task<> t)
    {
      tasks_.push_back (nxb::start (sched_, std::move (t)));
    }

    task_group &
    operator<< (task<> t)
    {
      start (std::move (t));
      return *this;
    }

    auto
    run_all ()
    {
      return nxb::when_all (std::move (tasks_));
    }

    template <typename... Tasks>
    static auto
    run (io_scheduler &sched, Tasks &&...tasks)
    {
      auto tg
        = nxb::task_group (sched, std::forward<Tasks> (tasks)...);
      return tg.run_all ();
    }

  private:
    io_scheduler &sched_;
    std::vector<task<>> tasks_;
  };

} // namespace nxb
