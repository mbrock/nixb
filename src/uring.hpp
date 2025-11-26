#pragma once

#include <bit>
#include <cerrno>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include <exec/linux/io_uring_context.hpp>
#include <exec/linux/safe_file_descriptor.hpp>
#include <exec/task.hpp>
#include <fcntl.h>
#include <fmt/format.h>
#include <poll.h>
#include <stdexec/execution.hpp>
#include <sys/eventfd.h>
#include <sys/uio.h>

namespace nxb::uring
{

struct file_data
{
  std::filesystem::path path;
  std::string buffer;
};

namespace detail
{
using exec::__io_uring::__context;
using exec::__io_uring::__stoppable_op_base;
using exec::__io_uring::__stoppable_task_facade_t;

template <class Receiver> struct read_file_impl : __stoppable_op_base<Receiver>
{
  exec::safe_file_descriptor fd_;
  std::filesystem::path path_;
  std::string buffer_;
#ifndef STDEXEC_HAS_IORING_OP_READ
  ::iovec iov_{};
#endif
  bool open_failed_{ false };
  std::exception_ptr open_error_{};

  read_file_impl (__context &ctx, std::filesystem::path &&path,
                  std::size_t buffer_bytes, Receiver &&receiver)
      : __stoppable_op_base<Receiver>{ ctx,
                                       static_cast<Receiver &&> (receiver) },
        path_{ std::move (path) }, buffer_ (buffer_bytes, '\0')
  {
    int raw_fd = ::open (path_.c_str (), O_RDONLY | O_CLOEXEC);
    if (raw_fd < 0)
      {
        open_failed_ = true;
        open_error_ = std::make_exception_ptr (std::system_error (
            errno, std::system_category (),
            fmt::format ("failed to open {}", path_.string ())));
      }
    else
      {
        fd_.reset (raw_fd);
#ifndef STDEXEC_HAS_IORING_OP_READ
        iov_.iov_base = buffer_.data ();
        iov_.iov_len = buffer_.size ();
#endif
      }
  }

  auto
  ready () const noexcept -> bool
  {
    return open_failed_;
  }

  void
  submit (::io_uring_sqe &sqe) noexcept
  {
    if (open_failed_)
      {
        sqe = ::io_uring_sqe{};
        return;
      }
    ::io_uring_sqe local{};
    local.fd = fd_.native_handle ();
    local.off = 0;
#ifdef STDEXEC_HAS_IORING_OP_READ
    local.opcode = IORING_OP_READ;
    local.addr = std::bit_cast<__u64> (buffer_.data ());
    local.len = static_cast<__u32> (buffer_.size ());
#else
    iov_.iov_base = buffer_.data ();
    iov_.iov_len = buffer_.size ();
    local.opcode = IORING_OP_READV;
    local.addr = std::bit_cast<__u64> (&iov_);
    local.len = 1;
#endif
    sqe = local;
  }

  void
  complete (const ::io_uring_cqe &cqe) noexcept
  {
    auto &receiver = this->receiver ();
    if (open_failed_)
      {
        stdexec::set_error (static_cast<Receiver &&> (receiver), open_error_);
        return;
      }
    if (cqe.res >= 0)
      {
        buffer_.resize (static_cast<std::size_t> (cqe.res));
        try
          {
            file_data payload{ std::move (path_), std::move (buffer_) };
            stdexec::set_value (static_cast<Receiver &&> (receiver),
                                std::move (payload));
          }
        catch (...)
          {
            stdexec::set_error (static_cast<Receiver &&> (receiver),
                                std::current_exception ());
          }
      }
    else
      {
        auto err = std::make_exception_ptr (
            std::system_error (-cqe.res, std::system_category ()));
        stdexec::set_error (static_cast<Receiver &&> (receiver), err);
      }
  }
};

template <class Receiver>
using read_file_operation
    = __stoppable_task_facade_t<read_file_impl<Receiver>>;

class io_uring_file_sender
{
public:
  using sender_concept = stdexec::sender_t;
  using completion_signatures = stdexec::completion_signatures<
      stdexec::set_value_t (file_data),
      stdexec::set_error_t (std::exception_ptr), stdexec::set_stopped_t ()>;

  io_uring_file_sender (exec::io_uring_scheduler scheduler,
                        std::filesystem::path path, std::size_t buffer_bytes)
      : scheduler_{ scheduler }, path_{ std::move (path) },
        buffer_bytes_{ buffer_bytes }
  {
  }

  template <stdexec::receiver_of<completion_signatures> Receiver>
  friend auto
  tag_invoke (stdexec::connect_t, io_uring_file_sender &&self,
              Receiver &&receiver) -> read_file_operation<Receiver>
  {
    auto *ctx = self.scheduler_.__context_;
    STDEXEC_ASSERT (ctx != nullptr);
    return read_file_operation<Receiver> (
        std::in_place, *ctx, std::move (self.path_), self.buffer_bytes_,
        static_cast<Receiver &&> (receiver));
  }

private:
  exec::io_uring_scheduler scheduler_;
  std::filesystem::path path_;
  std::size_t buffer_bytes_;
};

template <class Receiver> struct poll_impl : __stoppable_op_base<Receiver>
{
  int fd_;
  short events_;

  poll_impl (__context &ctx, int fd, short events, Receiver &&receiver)
      : __stoppable_op_base<Receiver>{ ctx,
                                       static_cast<Receiver &&> (receiver) },
        fd_{ fd }, events_{ events }
  {
  }

  auto
  ready () const noexcept -> bool
  {
    return false;
  }

  void
  submit (::io_uring_sqe &sqe) noexcept
  {
    sqe = {};
    sqe.opcode = IORING_OP_POLL_ADD;
    sqe.fd = fd_;
    sqe.poll_events = static_cast<__poll_t> (events_);
  }

  void
  complete (const ::io_uring_cqe &cqe) noexcept
  {
    auto &receiver = this->receiver ();
    if (cqe.res >= 0)
      {
        stdexec::set_value (static_cast<Receiver &&> (receiver),
                            static_cast<short> (cqe.res));
      }
    else
      {
        auto err = std::make_exception_ptr (
            std::system_error (-cqe.res, std::system_category (), "poll"));
        stdexec::set_error (static_cast<Receiver &&> (receiver), err);
      }
  }
};

template <class Receiver>
using poll_operation = __stoppable_task_facade_t<poll_impl<Receiver>>;

class poll_sender
{
public:
  using sender_concept = stdexec::sender_t;
  using completion_signatures
      = stdexec::completion_signatures<stdexec::set_value_t (short),
                                       stdexec::set_error_t (std::exception_ptr),
                                       stdexec::set_stopped_t ()>;

  poll_sender (exec::io_uring_scheduler scheduler, int fd, short events)
      : scheduler_{ scheduler }, fd_{ fd }, events_{ events }
  {
  }

  template <stdexec::receiver_of<completion_signatures> Receiver>
  friend auto
  tag_invoke (stdexec::connect_t, poll_sender &&self, Receiver &&receiver)
      -> poll_operation<Receiver>
  {
    auto *ctx = self.scheduler_.__context_;
    STDEXEC_ASSERT (ctx != nullptr);
    return poll_operation<Receiver> (std::in_place, *ctx, self.fd_,
                                     self.events_,
                                     static_cast<Receiver &&> (receiver));
  }

private:
  exec::io_uring_scheduler scheduler_;
  int fd_;
  short events_;
};

} // namespace detail

auto async_read_file (exec::io_uring_scheduler scheduler,
                      std::filesystem::path path,
                      std::size_t buffer_bytes = 64 * 1024)
    -> detail::io_uring_file_sender;

/// Poll an fd for readability. Completes with the poll revents.
inline auto
async_poll (exec::io_uring_scheduler scheduler, int fd,
            short events = POLLIN) -> detail::poll_sender
{
  return detail::poll_sender{ scheduler, fd, events };
}

/// An async event that can be signaled and awaited via io_uring.
/// Uses eventfd under the hood.
class async_event
{
public:
  async_event ()
  {
    int fd = ::eventfd (0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0)
      throw std::system_error (errno, std::system_category (), "eventfd");
    fd_.reset (fd);
  }

  /// Signal the event (wake up any waiters).
  void
  set () noexcept
  {
    std::uint64_t val = 1;
    ::write (fd_.native_handle (), &val, sizeof (val));
  }

  /// Reset the event (drain the eventfd).
  void
  reset () noexcept
  {
    std::uint64_t val;
    ::read (fd_.native_handle (), &val, sizeof (val));
  }

  /// Returns a sender that completes when the event is signaled.
  auto
  wait (exec::io_uring_scheduler scheduler)
  {
    namespace sx = stdexec;
    return async_poll (scheduler, fd_.native_handle ())
           | sx::then ([this] (short) { reset (); });
  }

  int
  fd () const noexcept
  {
    return fd_.native_handle ();
  }

private:
  exec::safe_file_descriptor fd_;
};

class io_uring_runtime
{
public:
  io_uring_runtime ();
  ~io_uring_runtime ();

  exec::io_uring_scheduler scheduler ();
  void stop ();

private:
  exec::io_uring_context context_;
  std::thread worker_;
};

} // namespace nxb::uring
