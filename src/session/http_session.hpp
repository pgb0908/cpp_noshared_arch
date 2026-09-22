#pragma once

#include <array>
#include <cassert>
#include <memory>
#include <string>
#include <vector>

#include "filter/body_filter_gate.hpp"
#include "filter/filter_chain.hpp"
#include "filter/filter_context.hpp"
#include "net/event_loop.hpp"
#include "net/http/llhttp/factory.hpp"
#include "net/http/native/factory.hpp"
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

    void start() {
        metrics_.on_accept();
        read_buf_ = buffer_pool_.acquire();
        write_buf_ = buffer_pool_.acquire();
        request_parser_ = net::http::llhttp_backend::create_request_parser();
        request_serializer_ = net::http::native::create_request_serializer();
        read_request_chunk();
    }

private:
    void assert_on_owning_thread() const {
        assert(event_loop_.is_current_thread() && "HttpSession I/O callback running on a non-owning thread");
    }

    // ---------- 1단계: downstream 요청 파싱 -> 필터 -> (upstream 연결) -> upstream 전송 ----------

    void read_request_chunk() {
        auto self = shared_from_this();
        downstream_->async_read_some(
            net::MutableBuffer{read_buf_->data(), read_buf_->size()},
            [this, self](const net::Error& err, std::size_t n) {
                assert_on_owning_thread();
                if (!err.ok()) {
                    close();
                    return;
                }
                process_downstream_chunk(net::ConstBuffer{read_buf_->data(), n});
            });
    }

    void process_downstream_chunk(net::ConstBuffer raw) {
        while (raw.size > 0) {
            const std::size_t consumed = request_parser_->feed(raw);
            raw = net::ConstBuffer{raw.data + consumed, raw.size - consumed};

            if (!request_filtered_ && request_parser_->header_done()) {
                request_filtered_ = true;
                net::http::RequestHead head = request_parser_->head();
                const FilterHeaderResult result = filter_chain_.apply_request(head, filter_ctx_);
                if (result.action == FilterHeaderAction::kRespondDirectly) {
                    respond_directly(result.direct_response.head, result.direct_response.body);
                    return;
                }
                request_serializer_->start(head);
                request_serializer_started_ = true;
            }

            if (request_serializer_started_) {
                drain_request_body_into_serializer();
                if (direct_response_mode_) {
                    return;  // 바디 필터가 거부(413 등) -- upstream 연결/전송 로직 스킵
                }
            }

            if (consumed == 0) {
                if (request_parser_->has_error()) {
                    close();
                    return;
                }
                break;  // 진행 불가 (정상적으로는 도달하지 않음) -- 방어적 중단
            }
        }

        if (!request_serializer_started_) {
            read_request_chunk();  // 헤더가 아직 안 끝남 -- 계속 읽기
            return;
        }

        if (request_parser_->message_done() && !request_final_body_provided_) {
            request_final_body_provided_ = true;
            request_serializer_->provide_body(net::ConstBuffer{nullptr, 0}, true);
        }

        if (!upstream_connecting_) {
            // 필터를 통과했을 때만 여기 도달한다 (거부됐으면 위에서 이미
            // return). 지금 이 시점에 upstream 연결을 시작 -- 그 전에는
            // 절대 connect하지 않는다.
            upstream_connecting_ = true;
            connect_upstream();
            return;
        }
        if (upstream_) {
            pull_and_write_request_to_upstream();
        }
        // else: 아직 connect 중 -- connect_upstream()의 콜백이 이어서 처리
    }

    // 파서에서 뽑은 바디 조각을 시리얼라이저로 바로 넘기지 않고, 먼저
    // 필터 체인을 거친다. 어느 청크가 메시지의 마지막인지(end_stream)를
    // 알아야 하는데, read_body()는 "지금 당장 더 없다"만 알려주지
    // "메시지 전체가 끝났다"는 알려주지 않으므로, 청크 하나를 미리
    // 들고 있다가(pending) 다음 청크가 있으면 false로 흘려보내고,
    // 루프가 끝나서 더 없다는 걸 확인한 뒤에야 그 마지막 pending을
    // message_done()으로 판단한 end_stream과 함께 내보낸다.
    void drain_request_body_into_serializer() {
        std::string pending;
        bool has_pending = false;
        for (;;) {
            const std::size_t n =
                request_parser_->read_body(net::MutableBuffer{body_scratch_.data(), body_scratch_.size()});
            if (n == 0) {
                break;
            }
            if (has_pending) {
                if (!process_request_data_chunk(std::move(pending), false)) {
                    return;  // 필터가 거부했거나(413) 세션이 직접 응답 모드로 전환됨
                }
                pending.clear();
            }
            pending.assign(body_scratch_.data(), n);
            has_pending = true;
        }
        if (has_pending) {
            process_request_data_chunk(std::move(pending), request_parser_->message_done());
        }
    }

    // 바디 청크 하나를 필터 체인(BodyFilterGate)에 통과시킨 뒤 시리얼라이저로
    // 넘긴다. 필터 적용/watermark 판단 자체는 BodyFilterGate 책임 --
    // 여기서는 그 결과를 소켓/시리얼라이저 동작으로 옮기기만 한다.
    // 반환값이 false면 필터가 즉시 응답(거부)해서 세션이 direct response
    // 모드로 전환됐다는 뜻 -- 호출자는 이후 정상 relay 로직(다음 필터,
    // upstream 연결 등)을 계속하면 안 된다.
    bool process_request_data_chunk(std::string chunk, bool end_stream) {
        BodyFilterGate::Result result = body_filter_gate_.apply_request(std::move(chunk), end_stream);
        switch (result.outcome) {
            case BodyFilterGate::Outcome::kReject:
                respond_directly(result.reject_head, result.reject_body);
                return false;
            case BodyFilterGate::Outcome::kStillBuffering:
                return true;  // 계속 읽어들임 -- 아직 upstream엔 아무것도 안 보냄
            case BodyFilterGate::Outcome::kForward:
                if (end_stream) {
                    request_final_body_provided_ = true;
                }
                request_serializer_->provide_body(net::ConstBuffer{result.data.data(), result.data.size()}, end_stream);
                return true;
        }
        return true;  // 도달 불가 -- switch가 enum 전체를 다룸
    }

    void connect_upstream() {
        endpoint_index_ = upstream_manager_.select_endpoint();
        auto self = shared_from_this();
        upstream_manager_.acquire_connection(
            endpoint_index_, [this, self](const net::Error& err, std::unique_ptr<net::ISocket> socket) {
                assert_on_owning_thread();
                if (!err.ok() || !socket) {
                    metrics_.upstream_connect_errors.fetch_add(1, std::memory_order_relaxed);
                    close();
                    return;
                }
                upstream_ = std::move(socket);
                pull_and_write_request_to_upstream();
            });
    }

    void pull_and_write_request_to_upstream() {
        const std::size_t n = request_serializer_->pull(net::MutableBuffer{write_buf_->data(), write_buf_->size()});
        if (n == 0) {
            if (request_serializer_->done()) {
                start_response_phase();
            } else if (!request_parser_->message_done()) {
                read_request_chunk();  // 더 내보낼 게 없음 -- downstream에서 더 읽어야 함
            } else {
                close();  // 이 상태(메시지는 끝났는데 시리얼라이저가 안 끝남)는 도달 불가여야 함 -- 방어적 종료
            }
            return;
        }

        auto self = shared_from_this();
        upstream_->async_write(net::ConstBuffer{write_buf_->data(), n},
                                [this, self](const net::Error& err, std::size_t written) {
                                    assert_on_owning_thread();
                                    if (!err.ok()) {
                                        close();
                                        return;
                                    }
                                    metrics_.bytes_downstream_to_upstream.fetch_add(written, std::memory_order_relaxed);
                                    pull_and_write_request_to_upstream();
                                });
    }

    // ---------- 2단계: upstream 응답 파싱 -> 필터 -> downstream 전송 ----------

    void start_response_phase() {
        response_parser_ = net::http::llhttp_backend::create_response_parser();
        response_serializer_ = net::http::native::create_response_serializer();
        read_response_chunk();
    }

    void read_response_chunk() {
        auto self = shared_from_this();
        upstream_->async_read_some(
            net::MutableBuffer{read_buf_->data(), read_buf_->size()},
            [this, self](const net::Error& err, std::size_t n) {
                assert_on_owning_thread();
                if (!err.ok()) {
                    close();
                    return;
                }
                process_upstream_chunk(net::ConstBuffer{read_buf_->data(), n});
            });
    }

    void process_upstream_chunk(net::ConstBuffer raw) {
        while (raw.size > 0) {
            const std::size_t consumed = response_parser_->feed(raw);
            raw = net::ConstBuffer{raw.data + consumed, raw.size - consumed};

            if (!response_filtered_ && response_parser_->header_done()) {
                response_filtered_ = true;
                net::http::ResponseHead head = response_parser_->head();
                const FilterHeaderResult result = filter_chain_.apply_response(head, filter_ctx_);
                if (result.action == FilterHeaderAction::kRespondDirectly) {
                    respond_directly(result.direct_response.head, result.direct_response.body);
                    return;
                }
                response_serializer_->start(head);
                response_serializer_started_ = true;
            }

            if (response_serializer_started_) {
                drain_response_body_into_serializer();
                if (direct_response_mode_) {
                    return;  // 바디 필터가 거부 -- 아래 정상 relay 로직 스킵
                }
            }

            if (consumed == 0) {
                if (response_parser_->has_error()) {
                    close();
                    return;
                }
                break;
            }
        }

        if (!response_serializer_started_) {
            read_response_chunk();
            return;
        }

        if (response_parser_->message_done() && !response_final_body_provided_) {
            response_final_body_provided_ = true;
            response_serializer_->provide_body(net::ConstBuffer{nullptr, 0}, true);
        }

        pull_and_write_response_to_downstream();
    }

    // drain_request_body_into_serializer()와 대칭 (end_stream 판단을
    // 위한 pending-하나-미리보기 방식도 동일).
    void drain_response_body_into_serializer() {
        std::string pending;
        bool has_pending = false;
        for (;;) {
            const std::size_t n =
                response_parser_->read_body(net::MutableBuffer{body_scratch_.data(), body_scratch_.size()});
            if (n == 0) {
                break;
            }
            if (has_pending) {
                if (!process_response_data_chunk(std::move(pending), false)) {
                    return;
                }
                pending.clear();
            }
            pending.assign(body_scratch_.data(), n);
            has_pending = true;
        }
        if (has_pending) {
            process_response_data_chunk(std::move(pending), response_parser_->message_done());
        }
    }

    // process_request_data_chunk()와 대칭 (BodyFilterGate::apply_response()가
    // 등록 역순(onion) 실행과 413 대신 502를 쓰는 것까지 알아서 처리한다).
    bool process_response_data_chunk(std::string chunk, bool end_stream) {
        BodyFilterGate::Result result = body_filter_gate_.apply_response(std::move(chunk), end_stream);
        switch (result.outcome) {
            case BodyFilterGate::Outcome::kReject:
                respond_directly(result.reject_head, result.reject_body);
                return false;
            case BodyFilterGate::Outcome::kStillBuffering:
                return true;
            case BodyFilterGate::Outcome::kForward:
                if (end_stream) {
                    response_final_body_provided_ = true;
                }
                response_serializer_->provide_body(net::ConstBuffer{result.data.data(), result.data.size()}, end_stream);
                return true;
        }
        return true;  // 도달 불가
    }

    void pull_and_write_response_to_downstream() {
        const std::size_t n = response_serializer_->pull(net::MutableBuffer{write_buf_->data(), write_buf_->size()});
        if (n == 0) {
            if (response_serializer_->done()) {
                close();  // 요청 1회 + 응답 1회 완료 -- keep-alive 없이 정상 종료
            } else if (!direct_response_mode_ && response_parser_ && !response_parser_->message_done()) {
                read_response_chunk();
            } else {
                close();  // 방어적 종료 (도달 불가여야 함)
            }
            return;
        }

        auto self = shared_from_this();
        downstream_->async_write(net::ConstBuffer{write_buf_->data(), n},
                                  [this, self](const net::Error& err, std::size_t written) {
                                      assert_on_owning_thread();
                                      if (!err.ok()) {
                                          close();
                                          return;
                                      }
                                      metrics_.bytes_upstream_to_downstream.fetch_add(written, std::memory_order_relaxed);
                                      pull_and_write_response_to_downstream();
                                  });
    }

    // ---------- 필터의 즉시 응답 처리 ----------

    // 요청 필터(upstream 연결 전) 또는 응답 필터(upstream 응답 대신)가
    // kRespondDirectly를 반환했을 때 호출된다. upstream_이 null일 수도
    // 있다는 게 핵심 -- 요청 필터가 거부한 경우 upstream엔 아예 연결
    // 안 한 상태다.
    void respond_directly(const net::http::ResponseHead& head, const std::string& body) {
        direct_response_mode_ = true;
        response_serializer_ = net::http::native::create_response_serializer();
        response_serializer_started_ = true;
        response_serializer_->start(head);
        response_serializer_->provide_body(net::ConstBuffer{body.data(), body.size()}, true);
        pull_and_write_response_to_downstream();
    }

    // ---------- 종료 ----------

    void close() {
        if (closing_) {
            return;
        }
        closing_ = true;

        downstream_->shutdown();
        downstream_->close();

        // keep-alive를 지원하지 않으므로 upstream 연결도 항상 닫는다 --
        // UpstreamManager의 pool에는 절대 반납하지 않음. upstream_이
        // null일 수 있음(필터가 요청을 거부해서 애초에 연결 안 한 경우).
        if (upstream_) {
            upstream_->shutdown();
            upstream_->close();
        }

        if (read_buf_) {
            buffer_pool_.release(std::move(read_buf_));
        }
        if (write_buf_) {
            buffer_pool_.release(std::move(write_buf_));
        }

        metrics_.on_close();
    }

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
