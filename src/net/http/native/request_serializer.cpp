#include "net/http/native/request_serializer.hpp"

#include <cstring>

#include "net/http/native/detail.hpp"

namespace net::http::native {

void RequestSerializer::start(const RequestHead& head) {
    head_bytes_.clear();
    head_bytes_ += head.method;
    head_bytes_ += ' ';
    head_bytes_ += head.target;
    head_bytes_ += ' ';
    head_bytes_ += (head.version == 10) ? "HTTP/1.0" : "HTTP/1.1";
    head_bytes_ += "\r\n";
    for (const auto& h : head.headers) {
        head_bytes_ += h.name;
        head_bytes_ += ": ";
        head_bytes_ += h.value;
        head_bytes_ += "\r\n";
    }
    head_bytes_ += "\r\n";

    chunked_ = detail::has_chunked_encoding(head.headers);
    head_sent_ = 0;
    pending_bytes_.clear();
    pending_sent_ = 0;
    is_last_provided_ = false;
    stage_ = Stage::kHead;
}

void RequestSerializer::provide_body(net::ConstBuffer chunk, bool is_last) {
    if (chunked_) {
        detail::append_chunk(pending_bytes_, chunk, is_last);
    } else {
        pending_bytes_.append(chunk.data, chunk.size);
    }
    if (is_last) {
        is_last_provided_ = true;
    }
}

std::size_t RequestSerializer::pull(net::MutableBuffer out) {
    std::size_t total = 0;

    if (stage_ == Stage::kHead) {
        const std::size_t n = std::min(out.size, head_bytes_.size() - head_sent_);
        std::memcpy(out.data, head_bytes_.data() + head_sent_, n);
        head_sent_ += n;
        total += n;
        if (head_sent_ == head_bytes_.size()) {
            stage_ = Stage::kBody;
        }
        if (total == out.size) {
            return total;
        }
    }

    if (stage_ == Stage::kBody) {
        const std::size_t n = std::min(out.size - total, pending_bytes_.size() - pending_sent_);
        std::memcpy(out.data + total, pending_bytes_.data() + pending_sent_, n);
        pending_sent_ += n;
        total += n;
        if (pending_sent_ == pending_bytes_.size()) {
            pending_bytes_.clear();
            pending_sent_ = 0;
            if (is_last_provided_) {
                stage_ = Stage::kDone;
            }
        }
    }

    return total;
}

bool RequestSerializer::done() const { return stage_ == Stage::kDone; }

}  // namespace net::http::native
