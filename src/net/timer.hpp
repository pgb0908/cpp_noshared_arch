#pragma once

#include <chrono>

#include "net/types.hpp"

namespace net {

class ITimer {
public:
    virtual ~ITimer() = default;

    virtual void expires_after(std::chrono::seconds duration) = 0;
    virtual void async_wait(ErrorCallback cb) = 0;
    virtual void cancel() = 0;
};

}  // namespace net
