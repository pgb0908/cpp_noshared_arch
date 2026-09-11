#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "net/acceptor.hpp"
#include "net/resolver.hpp"
#include "net/socket.hpp"
#include "net/timer.hpp"

namespace net {

// One event loop = one execution context (e.g. one boost::asio::io_context)
// run on exactly one thread. Mirrors the per-core-shard architecture's
// "1 io_context per shard" rule -- each GatewayShard owns exactly one
// IEventLoop. It also acts as the factory for every I/O object bound to it,
// so callers never touch a concrete library type directly.
class IEventLoop {
public:
    virtual ~IEventLoop() = default;

    virtual void run() = 0;   // blocks the calling thread until stop()
    virtual void stop() = 0;
    virtual void post(std::function<void()> task) = 0;

    // True if called from the thread currently running this loop's run().
    // Before run() is first called, always false. Intended for
    // assert()-based verification that shard-owned code actually executes
    // on its shard's thread -- see GatewayShard::dispatch_accept() and
    // UpstreamManager, which both assert this on every entry point that the
    // per-core-shard architecture requires to stay on one thread.
    virtual bool is_current_thread() const noexcept = 0;

    virtual std::unique_ptr<ISocket> create_socket() = 0;
    virtual std::unique_ptr<IAcceptor> create_acceptor(uint16_t port) = 0;
    virtual std::unique_ptr<IResolver> create_resolver() = 0;
    virtual std::unique_ptr<ITimer> create_timer() = 0;

    // Rebinds a socket created on a DIFFERENT event loop onto this one, so
    // all of its subsequent async operations run on this loop's thread.
    //
    // This exists because of a real bug found in this project: a socket
    // accepted via IAcceptor::async_accept() is bound to the ACCEPTOR's
    // event loop (the Listener's), not the shard it gets handed off to.
    // Without this rebind, every Session's actual read/write completions
    // kept running on the Listener thread -- not the owning shard's thread
    // -- silently defeating the whole per-core-shard design. The fix:
    // release the accepted socket's native handle and reconstruct it bound
    // to the target loop, done here so the domain layer (Listener) never
    // has to know the mechanism. See doc/plan.md for the full writeup.
    virtual std::unique_ptr<ISocket> adopt_socket(std::unique_ptr<ISocket> foreign_socket) = 0;
};

}  // namespace net
