#pragma once

#include <string>

#include "net/http/serializer.hpp"

// llhttp는 파싱만 하는 라이브러리라 시리얼라이저는 없다 -- 직렬화는
// 우리가 emit하는 바이트를 전부 통제하는 입장이라(malformed input을
// 받아들여야 하는 파싱과 달리) 직접 짜는 쪽이 오히려 더 간단하고 위험도
// 낮다. 외부 라이브러리 의존성 없음.
//
// 구현은 provide_body()가 호출될 때마다 (필요하면 chunked framing을
// 입혀서) pending_bytes_에 이어붙이고, pull()은 그걸 그냥 순서대로
// 흘려보내는 단순한 2단계(head -> body) 상태 기계다. body chunk를 한 번
// 더 복사하는 비용이 있지만(진짜 zero-copy는 아님), 상태 기계가 훨씬
// 단순해져서 첫 HTTP 릴레이 구현으로는 이 트레이드오프가 낫다고 판단함.
namespace net::http::native {

class RequestSerializer : public IRequestSerializer {
public:
    void start(const RequestHead& head) override;
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
