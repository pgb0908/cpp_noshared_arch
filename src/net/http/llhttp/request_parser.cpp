#include "net/http/llhttp/request_parser.hpp"

#include <cstring>

namespace net::http::llhttp_backend {

LlhttpRequestParser::LlhttpRequestParser() {
    ::llhttp_settings_init(&settings_);
    settings_.on_message_begin = &LlhttpRequestParser::on_message_begin;
    settings_.on_url = &LlhttpRequestParser::on_url;
    settings_.on_header_field = &LlhttpRequestParser::on_header_field;
    settings_.on_header_field_complete = &LlhttpRequestParser::on_header_field_complete;
    settings_.on_header_value = &LlhttpRequestParser::on_header_value;
    settings_.on_header_value_complete = &LlhttpRequestParser::on_header_value_complete;
    settings_.on_headers_complete = &LlhttpRequestParser::on_headers_complete;
    settings_.on_body = &LlhttpRequestParser::on_body;
    settings_.on_message_complete = &LlhttpRequestParser::on_message_complete;

    ::llhttp_init(&parser_, HTTP_REQUEST, &settings_);
    parser_.data = this;
}

std::size_t LlhttpRequestParser::feed(net::ConstBuffer raw) {
    if (has_error_ || message_done_) {
        return 0;
    }

    const ::llhttp_errno_t err = ::llhttp_execute(&parser_, raw.data, raw.size);

    if (err == HPE_OK) {
        return raw.size;  // 전부 소비, 일시정지 없이 끝까지 진행됨
    }

    const char* pos = ::llhttp_get_error_pos(&parser_);
    const std::size_t consumed = pos ? static_cast<std::size_t>(pos - raw.data) : 0;

    if (err == HPE_PAUSED) {
        // on_body가 scratch 버퍼가 꽉 차서 llhttp_pause()로 멈춘 것 --
        // 에러가 아니라 "지금까지 소비한 만큼만 받아들이고, caller가
        // read_body()로 비운 뒤 나머지를 다시 feed()해야 한다"는 신호.
        ::llhttp_resume(&parser_);
        return consumed;
    }

    has_error_ = true;
    error_ = net::Error{static_cast<int>(err), ::llhttp_errno_name(err)};
    return consumed;
}

std::size_t LlhttpRequestParser::read_body(net::MutableBuffer out) {
    const std::size_t n = std::min(out.size, scratch_.size());
    if (n > 0) {
        std::memcpy(out.data, scratch_.data(), n);
        scratch_.erase(0, n);
    }
    return n;
}

int LlhttpRequestParser::on_message_begin(::llhttp_t* p) {
    auto& s = self(p);
    s.head_ = net::http::RequestHead{};
    s.pending_field_.clear();
    s.pending_value_.clear();
    s.header_done_ = false;
    s.message_done_ = false;
    return HPE_OK;
}

int LlhttpRequestParser::on_url(::llhttp_t* p, const char* at, std::size_t len) {
    self(p).head_.target.append(at, len);
    return HPE_OK;
}

int LlhttpRequestParser::on_header_field(::llhttp_t* p, const char* at, std::size_t len) {
    self(p).pending_field_.append(at, len);
    return HPE_OK;
}

int LlhttpRequestParser::on_header_field_complete(::llhttp_t*) { return HPE_OK; }

int LlhttpRequestParser::on_header_value(::llhttp_t* p, const char* at, std::size_t len) {
    self(p).pending_value_.append(at, len);
    return HPE_OK;
}

int LlhttpRequestParser::on_header_value_complete(::llhttp_t* p) {
    auto& s = self(p);
    s.head_.headers.push_back(net::http::Header{std::move(s.pending_field_), std::move(s.pending_value_)});
    s.pending_field_.clear();
    s.pending_value_.clear();
    return HPE_OK;
}

int LlhttpRequestParser::on_headers_complete(::llhttp_t* p) {
    auto& s = self(p);
    s.head_.method = ::llhttp_method_name(static_cast<::llhttp_method_t>(p->method));
    s.head_.version = (p->http_major == 1 && p->http_minor == 0) ? 10 : 11;
    s.header_done_ = true;
    return HPE_OK;
}

int LlhttpRequestParser::on_body(::llhttp_t* p, const char* at, std::size_t len) {
    // on_body는 HPE_PAUSED를 지원하지 않는다 (net/http/llhttp/request_parser.hpp의
    // scratch_ 주석 참고) -- 그냥 다 받아서 쌓아둔다. caller가 feed()를
    // 호출하는 raw 청크 크기만큼만 쌓였다가 바로 read_body()로 빠지므로
    // 무한정 커지지 않는다.
    self(p).scratch_.append(at, len);
    return HPE_OK;
}

int LlhttpRequestParser::on_message_complete(::llhttp_t* p) {
    auto& s = self(p);
    // parser_.flags가 아직 유효한 마지막 시점 -- request_parser.hpp의
    // should_keep_alive_ 주석 참고.
    s.should_keep_alive_ = ::llhttp_should_keep_alive(p) != 0;
    s.message_done_ = true;
    return HPE_OK;
}

}  // namespace net::http::llhttp_backend
