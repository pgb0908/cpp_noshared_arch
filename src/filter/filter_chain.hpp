#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "filter/filter.hpp"
#include "filter/filter_context.hpp"
#include "filter/filter_data_result.hpp"

// 필터를 순서대로 쌓아두고 실행하는 체인. 지금 단계에선 코드에서 직접
// 필터를 등록한다 (default_filters.hpp의 build_default_filter_chain()
// 참고) -- config 기반 동적 구성은 RuntimeConfigManager가 생기면 그때
// 검토.
//
// 요청은 등록 순서대로, 응답은 등록 역순으로 실행한다 (onion 모델 --
// Envoy/Netty와 동일). 예를 들어 A, B, C 순으로 등록하면 요청은
// A -> B -> C, 응답은 C -> B -> A로 흐른다: 가장 바깥에 등록한 필터가
// 요청을 가장 먼저 보고 응답은 가장 나중에(가장 바깥에서) 다시 본다.
class FilterChain {
public:
    void add_filter(std::unique_ptr<IFilter> filter) { filters_.push_back(std::move(filter)); }

    // 등록된 순서대로 실행한다. 하나라도 kRespondDirectly를 반환하면
    // 즉시 멈추고 그 결과를 돌려준다 (뒤 필터는 실행 안 함).
    FilterHeaderResult apply_request(net::http::RequestHead& head, FilterContext& ctx) const {
        for (const auto& filter : filters_) {
            FilterHeaderResult result = filter->on_request(head, ctx);
            if (result.action == FilterHeaderAction::kRespondDirectly) {
                return result;
            }
        }
        return FilterHeaderResult::continue_();
    }

    // 등록 역순으로 실행한다 (onion 모델 -- 클래스 주석 참고).
    FilterHeaderResult apply_response(net::http::ResponseHead& head, FilterContext& ctx) const {
        for (auto it = filters_.rbegin(); it != filters_.rend(); ++it) {
            FilterHeaderResult result = (*it)->on_response(head, ctx);
            if (result.action == FilterHeaderAction::kRespondDirectly) {
                return result;
            }
        }
        return FilterHeaderResult::continue_();
    }

    // 바디 데이터 단계에서 "어느 필터가 막고 있는지 + 그 필터가 누적
    // 중인 바이트"를 담는 세션 스코프 상태. 필터 인스턴스 자체는
    // shard당 싱글턴이라 이 상태를 필터 멤버에 못 두므로, HttpSession이
    // 방향별로(요청/응답) 하나씩 소유하고 apply_request_data()/
    // apply_response_data() 호출마다 넘겨준다.
    struct DataIterationState {
        bool blocked = false;
        std::size_t blocked_filter_index = 0;
        std::string buffer;
    };

    // 등록 순서대로 필터를 실행한다 (apply_data()의 forward 버전).
    // 어떤 필터가 kStopIterationAndBuffer를 반환하면 그 인덱스를 state에
    // 기록하고 즉시 리턴 -- 다음 청크가 오면 그 필터부터(0번부터가
    // 아니라) 누적 버퍼로 이어서 호출한다.
    FilterDataResult apply_request_data(std::string& data, bool end_stream, DataIterationState& state,
                                         FilterContext& ctx) const {
        return apply_data(data, end_stream, state, ctx, /*reverse=*/false, &IFilter::on_request_data);
    }

    // apply_request_data()와 대칭이지만 등록 역순으로 실행한다 (onion
    // 모델, apply_response()와 동일한 순서).
    FilterDataResult apply_response_data(std::string& data, bool end_stream, DataIterationState& state,
                                          FilterContext& ctx) const {
        return apply_data(data, end_stream, state, ctx, /*reverse=*/true, &IFilter::on_response_data);
    }

private:
    std::vector<std::unique_ptr<IFilter>> filters_;

    using DataStep = FilterDataResult (IFilter::*)(std::string&, bool, FilterContext&);

    // apply_request_data()/apply_response_data()의 공통 순회 로직.
    // reverse로 방향을, step(멤버 함수 포인터)으로 on_request_data/
    // on_response_data 중 뭘 호출할지를 받는다 -- 두 메서드가 시그니처가
    // 완전히 같아서(둘 다 std::string&, bool, FilterContext&) 이렇게
    // 하나로 합칠 수 있다. (헤더 쪽 apply_request()/apply_response()는
    // RequestHead/ResponseHead 타입 자체가 달라 이 방식을 그대로 못
    // 쓴다 -- 대신 각 메서드가 8줄 안팎으로 짧아서 굳이 템플릿화하지
    // 않음.)
    FilterDataResult apply_data(std::string& data, bool end_stream, DataIterationState& state, FilterContext& ctx,
                                 bool reverse, DataStep step) const {
        if (filters_.empty()) {
            return FilterDataResult::continue_();
        }

        std::string* current = &data;
        std::size_t start_index = reverse ? filters_.size() - 1 : 0;
        if (state.blocked) {
            state.buffer.append(data);
            current = &state.buffer;
            start_index = state.blocked_filter_index;
        }

        // 필터 하나를 호출한 결과가 순회를 멈춰야 하는 것(거부/버퍼링)이면
        // 그 값을 반환하고, 다음 필터로 넘어가도 되면(kContinue) nullopt를
        // 반환한다.
        auto step_at = [&](std::size_t i) -> std::optional<FilterDataResult> {
            FilterDataResult result = (filters_[i].get()->*step)(*current, end_stream, ctx);
            if (result.action == FilterDataAction::kRespondDirectly) {
                state.blocked = false;
                state.buffer.clear();
                return result;
            }
            if (result.action == FilterDataAction::kStopIterationAndBuffer) {
                if (current != &state.buffer) {
                    state.buffer = std::move(*current);
                }
                state.blocked = true;
                state.blocked_filter_index = i;
                return FilterDataResult::stop_and_buffer();
            }
            return std::nullopt;  // kContinue -- 다음 필터로 진행
        };

        if (!reverse) {
            for (std::size_t i = start_index; i < filters_.size(); ++i) {
                if (auto stop = step_at(i)) {
                    return *stop;
                }
            }
        } else {
            for (std::size_t i = start_index + 1; i-- > 0;) {
                if (auto stop = step_at(i)) {
                    return *stop;
                }
            }
        }

        state.blocked = false;
        if (current == &state.buffer) {
            data = std::move(state.buffer);
            state.buffer.clear();
        }
        return FilterDataResult::continue_();
    }
};
