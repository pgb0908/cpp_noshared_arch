#pragma once

#include <llhttp.h>

#include "net/http/parser.hpp"

namespace net::llhttp_backend {

// llhttp(https://github.com/nodejs/llhttp, MIT, third_party/llhttp/에
// pre-generated C 소스 vendoring)로 구현한 net::http::IRequestParser.
// llhttp는 파싱만 하는 라이브러리라(직렬화는 없음) 순수 파싱 엔진으로
// 쓰기에 net::http:: 인터페이스와 잘 맞는다. Content-Length/chunked
// body framing 모두 llhttp가 자체 처리.
class LlhttpRequestParser : public net::http::IRequestParser {
public:
    LlhttpRequestParser();

    std::size_t feed(net::ConstBuffer raw) override;

    bool header_done() const override { return header_done_; }
    const net::http::RequestHead& head() const override { return head_; }

    std::size_t read_body(net::MutableBuffer out) override;

    bool message_done() const override { return message_done_; }
    bool has_error() const override { return has_error_; }
    const net::Error& error() const override { return error_; }

private:
    // llhttp 콜백들 (전부 static, parser->data에 담긴 this로 디스패치).
    static int on_message_begin(::llhttp_t* p);
    static int on_url(::llhttp_t* p, const char* at, std::size_t len);
    static int on_header_field(::llhttp_t* p, const char* at, std::size_t len);
    static int on_header_field_complete(::llhttp_t* p);
    static int on_header_value(::llhttp_t* p, const char* at, std::size_t len);
    static int on_header_value_complete(::llhttp_t* p);
    static int on_headers_complete(::llhttp_t* p);
    static int on_body(::llhttp_t* p, const char* at, std::size_t len);
    static int on_message_complete(::llhttp_t* p);

    static LlhttpRequestParser& self(::llhttp_t* p) { return *static_cast<LlhttpRequestParser*>(p->data); }

    static constexpr std::size_t kScratchSize = 16 * 1024;

    ::llhttp_t parser_{};
    ::llhttp_settings_t settings_{};

    bool header_done_ = false;
    bool message_done_ = false;
    net::http::RequestHead head_;

    // on_header_field/value 콜백은 하나의 헤더 이름/값이 여러 조각으로
    // 나뉘어 호출될 수 있어서 *_complete 콜백이 올 때까지 누적해둔다.
    std::string pending_field_;
    std::string pending_value_;

    char scratch_[kScratchSize];
    std::size_t scratch_used_ = 0;
    bool scratch_full_ = false;  // on_body 콜백이 scratch 꽉 차서 일시 정지시켰는지

    bool has_error_ = false;
    net::Error error_;
};

}  // namespace net::llhttp_backend
