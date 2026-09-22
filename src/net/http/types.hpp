#pragma once

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

// HTTP 메시지의 라이브러리 독립적인 표현. net:: 최상위 네임스페이스와
// 마찬가지로, 이 안의 어떤 것도 Beast(혹은 다른 HTTP 라이브러리)를
// 알아서는 안 된다. 구체 구현은 net/boost/http/ 아래.
namespace net::http {

struct Header {
    std::string name;
    std::string value;
};

// 요청의 header 부분만 (start-line + headers). body는 별도로 스트리밍
// relay되므로 여기 포함하지 않는다.
struct RequestHead {
    std::string method;
    std::string target;
    unsigned version = 11;  // 11 = HTTP/1.1, 10 = HTTP/1.0
    std::vector<Header> headers;
};

struct ResponseHead {
    unsigned status = 0;
    std::string reason;
    unsigned version = 11;
    std::vector<Header> headers;
};

namespace detail {
inline bool header_name_iequals(const std::string& a, const char* b) {
    return a.size() == std::strlen(b) &&
           std::equal(a.begin(), a.end(), b, [](char x, char y) { return std::tolower(x) == std::tolower(y); });
}
}  // namespace detail

// name과 대소문자 무관 일치하는 기존 헤더를 전부 제거하고 새 값 하나를
// append한다. keep-alive 도입 시 HttpSession이 Connection 헤더를
// (클라이언트/upstream이 뭐라 보냈든) 자신이 결정한 값으로 덮어쓰는 데
// 사용 -- hop-by-hop 헤더는 프록시가 직접 통제해야 한다는 원칙
// (RFC 7230 section 6.1) 그대로.
inline void set_header(std::vector<Header>& headers, const char* name, std::string value) {
    headers.erase(std::remove_if(headers.begin(), headers.end(),
                                  [name](const Header& h) { return detail::header_name_iequals(h.name, name); }),
                  headers.end());
    headers.push_back(Header{name, std::move(value)});
}

inline bool has_header(const std::vector<Header>& headers, const char* name) {
    return std::any_of(headers.begin(), headers.end(),
                        [name](const Header& h) { return detail::header_name_iequals(h.name, name); });
}

}  // namespace net::http
