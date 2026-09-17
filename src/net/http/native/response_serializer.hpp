#pragma once

#include <string>

#include "net/http/serializer.hpp"

// RequestSerializer와 대칭 -- 설계 배경은 그쪽 주석 참고.
namespace net::http::native {

class ResponseSerializer : public IResponseSerializer {
public:
    void start(const ResponseHead& head) override;
    void provide_body(net::ConstBuffer chunk, bool is_last) override;
    std::size_t pull(net::MutableBuffer out) override;
    bool done() const override;

private:
    enum class Stage { kHead, kBody, kDone };

    std::string head_bytes_;
    std::size_t head_sent_ = 0;

    bool chunked_ = false;
    std::string pending_bytes_;
    std::size_t pending_sent_ = 0;
    bool is_last_provided_ = false;

    Stage stage_ = Stage::kHead;
};

}  // namespace net::http::native
