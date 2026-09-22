#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "filter/body_filter_gate.hpp"
#include "filter/filter_chain.hpp"
#include "filter/filter_context.hpp"
#include "net/event_loop.hpp"
#include "net/http/parser.hpp"
#include "net/http/serializer.hpp"
#include "net/socket.hpp"
#include "upstream/upstream_manager.hpp"
#include "util/buffer_pool.hpp"
#include "util/local_metrics.hpp"

// Connection 하나: 이 shard가 생명주기 전체를 소유한다 (session.hpp와
// 동일한 원칙). raw byte relay였던 Session을 HTTP 인식 relay로 완전히
// 대체한다 -- doc/plan.md 참고, session.hpp는 참고용으로 남겨둠.
//
// keep-alive 없음 (이번 MVP 범위): downstream/upstream 둘 다 요청/응답
// 1회만 처리하고 양쪽 다 close한다. 그래서 UpstreamManager의 connection
// pool도 여기선 안 쓴다 (release_connection()을 절대 호출하지 않음).
//
// HTTP는 request를 다 보내야 response를 받을 수 있는 반이중(half-duplex)
// 프로토콜이라(파이프라이닝 미지원 전제), raw relay처럼 양방향을 동시에
// 안 돌리고 "요청 relay 완료 -> 응답 relay 시작"을 순차로 진행한다.
//
// 필터 체인은 요청 헤더가 다 파싱된 시점(upstream 연결 전!)에 한 번,
// 응답 헤더가 다 파싱된 시점에 한 번 실행된다 (등록 순서 -> 역순인 onion
// 모델, filter_chain.hpp 참고). 필터가 kRespondDirectly를 반환하면 --
// 인증 실패, rate limit 등 -- upstream에 아예 연결하지 않고 바로
// 응답한다. 이게 이 세션이 "필터 통과 전엔 upstream을 connect하지 않는"
// 순서로 짜인 이유다.
//
// 구현은 http_session.cpp에 있다 -- 소켓 I/O 오케스트레이션 부분만
// 여기 선언돼 있고, 필터/watermark 판단 정책은 BodyFilterGate로 이미
// 분리돼 있다 (doc/plan.md의 "BodyFilterGate 추출" 절 참고).
class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
    HttpSession(std::unique_ptr<net::ISocket> downstream_socket,
                net::IEventLoop& event_loop,
                UpstreamManager& upstream_manager,
                BufferPool& buffer_pool,
                const FilterChain& filter_chain,
                std::size_t body_buffer_high_watermark_bytes,
                LocalMetrics& metrics)
        : downstream_(std::move(downstream_socket)),
          event_loop_(event_loop),
          upstream_manager_(upstream_manager),
          buffer_pool_(buffer_pool),
          filter_chain_(filter_chain),
          body_filter_gate_(filter_chain_, filter_ctx_, body_buffer_high_watermark_bytes),
          metrics_(metrics) {}

    void start();

private:
    void assert_on_owning_thread() const;

    // ---------- 1단계: downstream 요청 파싱 -> 필터 -> (upstream 연결) -> upstream 전송 ----------

    void read_request_chunk();
    void process_downstream_chunk(net::ConstBuffer raw);
    void drain_request_body_into_serializer();
    bool process_request_data_chunk(std::string chunk, bool end_stream);
    void connect_upstream();
    void pull_and_write_request_to_upstream();

    // ---------- 2단계: upstream 응답 파싱 -> 필터 -> downstream 전송 ----------

    void start_response_phase();
    void read_response_chunk();
    void process_upstream_chunk(net::ConstBuffer raw);
    void drain_response_body_into_serializer();
    bool process_response_data_chunk(std::string chunk, bool end_stream);
    void pull_and_write_response_to_downstream();

    // ---------- 필터의 즉시 응답 처리 ----------

    void respond_directly(const net::http::ResponseHead& head, const std::string& body);

    // ---------- 종료 ----------

    void close();

    std::unique_ptr<net::ISocket> downstream_;
    net::IEventLoop& event_loop_;
    std::unique_ptr<net::ISocket> upstream_;
    UpstreamManager& upstream_manager_;
    const FilterChain& filter_chain_;
    // 이 세션(요청 하나)만을 위한 필터 상태 저장소. 필터 인스턴스 자체는
    // shard당 싱글턴으로 공유되므로, request 단계에서 심은 값을 response
    // 단계나 다른 필터에서 읽고 싶을 때 여기에 담는다 (filter_context.hpp
    // 참고). body_filter_gate_보다 먼저 선언돼야 함 -- 아래에서 그 참조를
    // 바인딩한다.
    FilterContext filter_ctx_;
    // 바디 청크를 필터에 통과시키고 watermark(413/502)를 판단하는 정책을
    // 캡슐화 (body_filter_gate.hpp 참고). 방향별 버퍼링 상태도 이 안에서
    // 관리한다.
    BodyFilterGate body_filter_gate_;
    std::size_t endpoint_index_ = 0;
    bool upstream_connecting_ = false;

    BufferPool& buffer_pool_;
    std::unique_ptr<std::vector<char>> read_buf_;
    std::unique_ptr<std::vector<char>> write_buf_;
    std::array<char, 4096> body_scratch_{};

    std::unique_ptr<net::http::IRequestParser> request_parser_;
    std::unique_ptr<net::http::IRequestSerializer> request_serializer_;
    bool request_filtered_ = false;
    bool request_serializer_started_ = false;
    bool request_final_body_provided_ = false;

    std::unique_ptr<net::http::IResponseParser> response_parser_;
    std::unique_ptr<net::http::IResponseSerializer> response_serializer_;
    bool response_filtered_ = false;
    bool response_serializer_started_ = false;
    bool response_final_body_provided_ = false;
    bool direct_response_mode_ = false;

    LocalMetrics& metrics_;
    bool closing_ = false;
};
