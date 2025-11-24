#include "uring.hpp"

namespace nxb::uring
{

auto
async_read_file (exec::io_uring_scheduler scheduler,
                 std::filesystem::path path, std::size_t buffer_bytes)
    -> detail::io_uring_file_sender
{
  return detail::io_uring_file_sender (scheduler, std::move (path),
                                       buffer_bytes);
}

io_uring_runtime::io_uring_runtime ()
    : context_ (), worker_ ([this] { context_.run_until_stopped (); })
{
}

io_uring_runtime::~io_uring_runtime () { stop (); }

exec::io_uring_scheduler
io_uring_runtime::scheduler ()
{
  return context_.get_scheduler ();
}

void
io_uring_runtime::stop ()
{
  if (worker_.joinable ())
    {
      context_.request_stop ();
      worker_.join ();
    }
}

} // namespace nxb::uring
