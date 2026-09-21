#include "net/http/llhttp/response_parser.hpp"

#include <cstring>

namespace net::http::llhttp_backend {

LlhttpResponseParser::LlhttpResponseParser() {
    ::llhttp_settings_init(&settings_);
    settings_.on_message_begin = &LlhttpResponseParser::on_message_begin;
    settings_.on_status = &LlhttpResponseParser::on_status;
    settings_.on_header_field = &LlhttpResponseParser::on_header_field;
    settings_.on_header_field_complete = &LlhttpResponseParser::on_header_field_complete;
    settings_.on_header_value = &LlhttpResponseParser::on_header_value;
    settings_.on_header_value_complete = &LlhttpResponseParser::on_header_value_complete;
    settings_.on_headers_complete = &LlhttpResponseParser::on_headers_complete;
    settings_.on_body = &LlhttpResponseParser::on_body;
    settings_.on_message_complete = &LlhttpResponseParser::on_message_complete;

    ::llhttp_init(&parser_, HTTP_RESPONSE, &settings_);
    parser_.data = this;
}

std::size_t LlhttpResponseParser::feed(net::ConstBuffer raw) {
    if (has_error_ || message_done_) {
        return 0;
    }

    const ::llhttp_errno_t err = ::llhttp_execute(&parser_, raw.data, raw.size);

    if (err == HPE_OK) {
        return raw.size;
    }

    const char* pos = ::llhttp_get_error_pos(&parser_);
    const std::size_t consumed = pos ? static_cast<std::size_t>(pos - raw.data) : 0;

    if (err == HPE_PAUSED) {
        ::llhttp_resume(&parser_);
        return consumed;
    }

    has_error_ = true;
    error_ = net::Error{static_cast<int>(err), ::llhttp_errno_name(err)};
    return consumed;
}

std::size_t LlhttpResponseParser::read_body(net::MutableBuffer out) {
    const std::size_t n = std::min(out.size, scratch_.size());
    if (n > 0) {
        std::memcpy(out.data, scratch_.data(), n);
        scratch_.erase(0, n);
    }
    return n;
}

int LlhttpResponseParser::on_message_begin(::llhttp_t* p) {
    auto& s = self(p);
    s.head_ = net::http::ResponseHead{};
    s.pending_field_.clear();
    s.pending_value_.clear();
    s.header_done_ = false;
    s.message_done_ = false;
    return HPE_OK;
}

int LlhttpResponseParser::on_status(::llhttp_t* p, const char* at, std::size_t len) {
    self(p).head_.reason.append(at, len);
    return HPE_OK;
}

int LlhttpResponseParser::on_header_field(::llhttp_t* p, const char* at, std::size_t len) {
    self(p).pending_field_.append(at, len);
    return HPE_OK;
}

int LlhttpResponseParser::on_header_field_complete(::llhttp_t*) { return HPE_OK; }

int LlhttpResponseParser::on_header_value(::llhttp_t* p, const char* at, std::size_t len) {
    self(p).pending_value_.append(at, len);
    return HPE_OK;
}

int LlhttpResponseParser::on_header_value_complete(::llhttp_t* p) {
    auto& s = self(p);
    s.head_.headers.push_back(net::http::Header{std::move(s.pending_field_), std::move(s.pending_value_)});
    s.pending_field_.clear();
    s.pending_value_.clear();
    return HPE_OK;
}

int LlhttpResponseParser::on_headers_complete(::llhttp_t* p) {
    auto& s = self(p);
    s.head_.status = p->status_code;
    s.head_.version = (p->http_major == 1 && p->http_minor == 0) ? 10 : 11;
    s.header_done_ = true;
    return HPE_OK;
}

int LlhttpResponseParser::on_body(::llhttp_t* p, const char* at, std::size_t len) {
    // on_body는 HPE_PAUSED를 지원하지 않는다 (request_parser.hpp의
    // scratch_ 주석 참고) -- 그냥 다 받아서 쌓아둔다.
    self(p).scratch_.append(at, len);
    return HPE_OK;
}

int LlhttpResponseParser::on_message_complete(::llhttp_t* p) {
    self(p).message_done_ = true;
    return HPE_OK;
}

}  // namespace net::http::llhttp_backend
