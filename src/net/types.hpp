#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// Library-agnostic networking types. Nothing in this namespace may depend
// on Boost (or any other specific async I/O library) -- that is the whole
// point of the net/ abstraction layer. Concrete implementations live under
// net/boost/ (or a future net/<other-library>/).
namespace net {

struct Error {
    int code = 0;
    std::string message;

    bool ok() const noexcept { return code == 0; }
    static Error none() { return Error{0, {}}; }
};

// A resolved address: host is always a numeric IP string (post-DNS), never
// a hostname -- resolution happens once, in IResolver.
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

using VoidCallback = std::function<void()>;
using ErrorCallback = std::function<void(const Error&)>;
using IoCallback = std::function<void(const Error&, std::size_t)>;

}  // namespace net
