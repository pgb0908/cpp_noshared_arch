#pragma once

#include <memory>

#include "net/socket.hpp"
#include "net/types.hpp"

namespace net {

using AcceptCallback = MoveOnlyFunction<void(const Error&, std::unique_ptr<ISocket>)>;

class IAcceptor {
public:
    virtual ~IAcceptor() = default;
    virtual void async_accept(AcceptCallback cb) = 0;
    virtual void close() = 0;
};

}  // namespace net
