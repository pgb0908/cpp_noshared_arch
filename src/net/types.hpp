#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "util/move_only_function.hpp"

// 라이브러리에 독립적인 네트워킹 타입들. 이 네임스페이스 안의 어떤 것도
// Boost(혹은 다른 특정 비동기 I/O 라이브러리)에 의존해서는 안 된다 --
// 그게 바로 net/ 추상화 계층의 존재 이유. 구체적인 구현체는
// net/boost/ 아래(혹은 나중에 net/<다른 라이브러리>/)에 위치한다.
namespace net {

struct Error {
    int code = 0;
    std::string message;

    bool ok() const noexcept { return code == 0; }
    static Error none() { return Error{0, {}}; }
};

// resolve가 끝난 주소: host는 항상 숫자 IP 문자열(DNS 조회 이후)이며
// 절대 hostname이 아님 -- resolve는 IResolver에서 한 번만 일어난다.
struct Endpoint {
    std::string host;
    uint16_t port = 0;
};

struct MutableBuffer {
    char* data = nullptr;
    std::size_t size = 0;
};

struct ConstBuffer {
    const char* data = nullptr;
    std::size_t size = 0;
};

// std::function이 아니라 MoveOnlyFunction을 쓰는 이유는
// util/move_only_function.hpp 주석 참고 -- unique_ptr 캡처를 boxing
// 없이 그대로 담기 위함.
using VoidCallback = MoveOnlyFunction<void()>;
using ErrorCallback = MoveOnlyFunction<void(const Error&)>;
using IoCallback = MoveOnlyFunction<void(const Error&, std::size_t)>;

}  // namespace net
