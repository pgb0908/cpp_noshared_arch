#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "net/types.hpp"

namespace net {

class IResolver {
public:
    virtual ~IResolver() = default;

    // Synchronous by design: current usage is a blocking resolve at shard
    // startup and on a periodic refresh timer -- never on the request hot
    // path -- so there is no need for an async_resolve() in this interface.
    virtual std::pair<Error, std::vector<Endpoint>> resolve(const std::string& host, uint16_t port) = 0;
};

}  // namespace net
