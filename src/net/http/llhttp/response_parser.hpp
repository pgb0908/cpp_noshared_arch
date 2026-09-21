#pragma once

#include <llhttp.h>

#include "net/http/parser.hpp"

namespace net::http::llhttp_backend {

// LlhttpRequestParser와 대칭 -- 자세한 설계 배경은 그쪽 주석 참고.
class LlhttpResponseParser : public net::http::IResponseParser {
public:
    LlhttpResponseParser();

    std::size_t feed(net::ConstBuffer raw) override;

    bool header_done() const override { return header_done_; }
    const net::http::ResponseHead& head() const override { return head_; }

    std::size_t read_body(net::MutableBuffer out) override;

    bool message_done() const override { return message_done_; }
    bool has_error() const override { return has_error_; }
    const net::Error& error() const override { return error_; }

private:
    static int on_message_begin(::llhttp_t* p);
    static int on_status(::llhttp_t* p, const char* at, std::size_t len);
    static int on_header_field(::llhttp_t* p, const char* at, std::size_t len);
    static int on_header_field_complete(::llhttp_t* p);
    static int on_header_value(::llhttp_t* p, const char* at, std::size_t len);
    static int on_header_value_complete(::llhttp_t* p);
    static int on_headers_complete(::llhttp_t* p);
    static int on_body(::llhttp_t* p, const char* at, std::size_t len);
    static int on_message_complete(::llhttp_t* p);

    static LlhttpResponseParser& self(::llhttp_t* p) { return *static_cast<LlhttpResponseParser*>(p->data); }

    ::llhttp_t parser_{};
    ::llhttp_settings_t settings_{};

    bool header_done_ = false;
    bool message_done_ = false;
    net::http::ResponseHead head_;

    std::string pending_field_;
    std::string pending_value_;

    // 고정 크기가 아니라 std::string인 이유는 LlhttpRequestParser의
    // scratch_ 주석 참고 -- on_body는 HPE_PAUSED를 지원하지 않는다.
    std::string scratch_;

    bool has_error_ = false;
    net::Error error_;
};

}  // namespace net::http::llhttp_backend
