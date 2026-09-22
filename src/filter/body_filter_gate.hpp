#pragma once

#include <iostream>
#include <string>
#include <utility>

#include "filter/filter_chain.hpp"
#include "filter/filter_context.hpp"
#include "filter/filter_data_result.hpp"
#include "net/http/types.hpp"

// HttpSession의 요청/응답 바디 relay 루프와 FilterChain 사이의 정책을
// 담당한다: 청크를 필터에 통과시키고, 그 결과(Continue/거부/버퍼링)를
// HTTP 레벨 결정(그대로 흘려보낼지, 413/502로 거부할지, 아직 더
// 기다릴지)으로 바꾼다. 소켓/시리얼라이저는 전혀 모른다 -- 그래서
// HttpSession 없이 이 클래스만 GTest로 직접 검증할 수 있다.
//
// watermark 정책은 doc/plan.md의 "바디 필터 (스트리밍 + 버퍼링)" 절
// 참고 -- Envoy처럼 high/low 이중 threshold + read pause/resume이
// 아니라, 하드 캡(high watermark) 하나 + 초과 시 즉시 거부만 쓴다
// ("바디 전체가 필요한 필터 + read 일시정지"를 같이 쓰면 데드락이 됨).
class BodyFilterGate {
public:
    enum class Outcome {
        kForward,        // data를 그대로(혹은 필터가 고친 대로) 다음 단계(시리얼라이저)로
        kStillBuffering,  // 필터가 아직 판단 불가 -- 아무것도 하지 말고 다음 청크를 기다려라
        kReject,          // reject_head/reject_body로 즉시 응답하고 relay 중단
    };

    struct Result {
        Outcome outcome = Outcome::kForward;
        std::string data;                        // kForward일 때만 유효
        net::http::ResponseHead reject_head;      // kReject일 때만 유효
        std::string reject_body;
    };

    BodyFilterGate(const FilterChain& chain, FilterContext& ctx, std::size_t high_watermark_bytes)
        : chain_(chain), ctx_(ctx), high_watermark_bytes_(high_watermark_bytes) {}

    Result apply_request(std::string chunk, bool end_stream) {
        return apply(std::move(chunk), end_stream, request_state_, /*is_request=*/true);
    }

    Result apply_response(std::string chunk, bool end_stream) {
        return apply(std::move(chunk), end_stream, response_state_, /*is_request=*/false);
    }

    // keep-alive로 같은 커넥션에서 다음 요청을 처리하기 전에 호출 --
    // 방향별 버퍼링 상태(DataIterationState)를 새 메시지를 위해 비운다.
    void reset() {
        request_state_ = FilterChain::DataIterationState{};
        response_state_ = FilterChain::DataIterationState{};
    }

private:
    Result apply(std::string chunk, bool end_stream, FilterChain::DataIterationState& state, bool is_request) {
        const FilterDataResult result = is_request ? chain_.apply_request_data(chunk, end_stream, state, ctx_)
                                                    : chain_.apply_response_data(chunk, end_stream, state, ctx_);

        if (result.action == FilterDataAction::kRespondDirectly) {
            state = FilterChain::DataIterationState{};
            return Result{Outcome::kReject, {}, result.direct_response.head, result.direct_response.body};
        }

        if (result.action == FilterDataAction::kStopIterationAndBuffer) {
            // watermark 체크가 항상 먼저다 -- end_stream=true인 첫(=마지막)
            // 청크 하나로 메시지가 끝나는 경우(예: 작은 메시지가 소켓 read
            // 한 번에 통째로 옴)엔 "이전 호출들이 이미 체크를 통과해왔다"는
            // 전제가 성립하지 않는다. 이번이 유일한 호출일 수도 있으므로
            // 매번 직접 확인해야 한다 (실측 중 실제로 이 순서를 반대로
            // 했다가 413이 우회되는 버그가 있었음).
            if (state.buffer.size() > high_watermark_bytes_) {
                net::http::ResponseHead resp;
                resp.status = is_request ? 413 : 502;
                resp.reason = is_request ? "Payload Too Large" : "Bad Gateway";
                resp.headers.push_back({"Content-Length", "0"});
                state = FilterChain::DataIterationState{};
                return Result{Outcome::kReject, {}, resp, ""};
            }
            if (end_stream) {
                // 더 기다릴 데이터가 없는데 필터가 계속 버퍼링을 요청함 --
                // 필터 작성자의 실수(계약 위반)이지만, 그런 필터 하나
                // 때문에 서버 프로세스 전체가 죽으면 안 되므로 방어적으로
                // 지금까지 누적된 걸(watermark 이내임이 위에서 이미
                // 확인됨) 그대로 흘려보내고 강제 종결한다.
                std::cerr << "[filter] warning: on_" << (is_request ? "request" : "response")
                          << "_data returned kStopIterationAndBuffer at end_stream -- forcing continue with "
                             "accumulated buffer\n";
                std::string forced = std::move(state.buffer);
                state = FilterChain::DataIterationState{};
                return Result{Outcome::kForward, std::move(forced), {}, ""};
            }
            return Result{Outcome::kStillBuffering, {}, {}, ""};
        }

        // kContinue -- apply_*_data()가 이미 chunk를 최종 데이터로 채워둠
        // (버퍼링 중이었다면 누적본으로 교체됨).
        return Result{Outcome::kForward, std::move(chunk), {}, ""};
    }

    const FilterChain& chain_;
    FilterContext& ctx_;
    std::size_t high_watermark_bytes_;
    FilterChain::DataIterationState request_state_;
    FilterChain::DataIterationState response_state_;
};
