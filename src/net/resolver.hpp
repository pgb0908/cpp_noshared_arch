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

    // 의도적으로 동기 방식: 현재 사용처는 shard 시작 시 1회 blocking
    // resolve와 주기적 refresh timer뿐 -- request hot path에서는 절대
    // 쓰이지 않으므로 이 인터페이스에 async_resolve()는 필요 없다.
    virtual std::pair<Error, std::vector<Endpoint>> resolve(const std::string& host, uint16_t port) = 0;
};

}  // namespace net
