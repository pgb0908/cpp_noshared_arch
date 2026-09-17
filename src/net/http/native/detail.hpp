#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include "net/http/types.hpp"
#include "net/types.hpp"

// RequestSerializer/ResponseSerializer가 공유하는 작은 헬퍼들.
namespace net::http::native::detail {

inline bool iequals(const std::string& a, const char* b) {
    return a.size() == std::strlen(b) &&
           std::equal(a.begin(), a.end(), b, [](char x, char y) { return std::tolower(x) == std::tolower(y); });
}

inline bool icontains(const std::string& haystack, const char* needle) {
    std::string h = haystack;
    std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return std::tolower(c); });
    return h.find(needle) != std::string::npos;
}

// header 목록 중 "Transfer-Encoding: chunked"가 있으면 true.
inline bool has_chunked_encoding(const std::vector<Header>& headers) {
    for (const auto& h : headers) {
        if (iequals(h.name, "Transfer-Encoding") && icontains(h.value, "chunked")) {
            return true;
        }
    }
    return false;
}

// chunked body 조각 하나를 "<hex-size>\r\n<data>\r\n" 형태로 append.
// is_last면 뒤이어 종료 프레임("0\r\n\r\n")까지 append.
inline void append_chunk(std::string& out, net::ConstBuffer chunk, bool is_last) {
    if (chunk.size > 0) {
        char size_hex[32];
        const int n = std::snprintf(size_hex, sizeof(size_hex), "%zx\r\n", chunk.size);
        out.append(size_hex, static_cast<std::size_t>(n));
        out.append(chunk.data, chunk.size);
        out.append("\r\n");
    }
    if (is_last) {
        out.append("0\r\n\r\n");
    }
}

}  // namespace net::http::native::detail
