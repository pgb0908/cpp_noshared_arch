#pragma once

#include <memory>

#include "net/types.hpp"

namespace net {

class ISocket {
public:
    virtual ~ISocket() = default;

    virtual void async_connect(const Endpoint& endpoint, ErrorCallback cb) = 0;
    virtual void async_read_some(MutableBuffer buffer, IoCallback cb) = 0;
    // Writes the entire buffer (matches boost::asio::async_write semantics,
    // not a raw single async_write_some).
    virtual void async_write(ConstBuffer buffer, IoCallback cb) = 0;

    virtual void shutdown() = 0;  // both directions
    virtual void close() = 0;
    virtual bool is_open() const = 0;
    virtual void cancel() = 0;    // cancel any pending async operation

    // Releases and returns the platform-native socket handle (POSIX fd),
    // relinquishing this object's ownership of it. Used only by
    // IEventLoop::adopt_socket() to move a socket accepted on one event
    // loop onto another -- see net/event_loop.hpp. Calling any other method
    // on this object afterward is undefined behavior.
    virtual int release_native_handle() = 0;
};

using SocketCallback = std::function<void(const Error&, std::unique_ptr<ISocket>)>;

}  // namespace net
