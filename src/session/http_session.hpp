#pragma once

#include <array>
#include <cassert>
#include <memory>
#include <vector>

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
// pool도 여기선 안 쓴다 (release_connection()을 절대 호출하지 않음) --
// 재사용할 일이 없는데 pool에 반납해봐야 의미가 없고, "pool에서 꺼낸
// 연결이 진짜 살아있는지 검증"이라는 어려운 문제도 자연히 피해간다.
//
// HTTP는 request를 다 보내야 response를 받을 수 있는 반이중(half-duplex)
// 프로토콜이라(파이프라이닝 미지원 전제), raw relay처럼 양방향을 동시에
// 안 돌리고 "요청 relay 완료 -> 응답 relay 시작"을 순차로 진행한다.
class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
    HttpSession(std::unique_ptr<net::ISocket> downstream_socket,
                net::IEventLoop& event_loop,
                UpstreamManager& upstream_manager,
                BufferPool& buffer_pool,
                LocalMetrics& metrics)
        : downstream_(std::move(downstream_socket)),
          event_loop_(event_loop),
          upstream_manager_(upstream_manager),
          buffer_pool_(buffer_pool),
          metrics_(metrics) {}

    void start() {
        metrics_.on_accept();
        connect_upstream();
    }

private:
    void assert_on_owning_thread() const {
        assert(event_loop_.is_current_thread() && "HttpSession I/O callback running on a non-owning thread");
    }

    // ---------- upstream 연결 ----------

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
                on_upstream_ready();
            });
    }

    void on_upstream_ready() {
        read_buf_ = buffer_pool_.acquire();
        write_buf_ = buffer_pool_.acquire();

        request_parser_ = net::http::llhttp_backend::create_request_parser();
        request_serializer_ = net::http::native::create_request_serializer();
        read_request_chunk();
    }

    // ---------- 1단계: downstream 요청 -> upstream ----------

    void read_request_chunk() {
        auto self = shared_from_this();
        downstream_->async_read_some(
            net::MutableBuffer{read_buf_->data(), read_buf_->size()}, [this, self](const net::Error& err, std::size_t n) {
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
            drain_request_body_into_serializer();
            if (consumed == 0) {
                if (request_parser_->has_error()) {
                    close();
                    return;
                }
                break;  // 진행 불가 (정상적으로는 도달하지 않음) -- 방어적 중단
            }
        }

        if (request_parser_->message_done() && !request_final_body_provided_) {
            request_final_body_provided_ = true;
            request_serializer_->provide_body(net::ConstBuffer{nullptr, 0}, true);
        }

        pull_and_write_request_to_upstream();
    }

    void drain_request_body_into_serializer() {
        if (!request_serializer_started_ && request_parser_->header_done()) {
            request_serializer_started_ = true;
            request_serializer_->start(request_parser_->head());
        }
        if (!request_serializer_started_) {
            return;
        }
        for (;;) {
            const std::size_t n =
                request_parser_->read_body(net::MutableBuffer{body_scratch_.data(), body_scratch_.size()});
            if (n == 0) {
                break;
            }
            request_serializer_->provide_body(net::ConstBuffer{body_scratch_.data(), n}, false);
        }
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

    // ---------- 2단계: upstream 응답 -> downstream ----------

    void start_response_phase() {
        response_parser_ = net::http::llhttp_backend::create_response_parser();
        response_serializer_ = net::http::native::create_response_serializer();
        read_response_chunk();
    }

    void read_response_chunk() {
        auto self = shared_from_this();
        upstream_->async_read_some(
            net::MutableBuffer{read_buf_->data(), read_buf_->size()}, [this, self](const net::Error& err, std::size_t n) {
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
            drain_response_body_into_serializer();
            if (consumed == 0) {
                if (response_parser_->has_error()) {
                    close();
                    return;
                }
                break;
            }
        }

        if (response_parser_->message_done() && !response_final_body_provided_) {
            response_final_body_provided_ = true;
            response_serializer_->provide_body(net::ConstBuffer{nullptr, 0}, true);
        }

        pull_and_write_response_to_downstream();
    }

    void drain_response_body_into_serializer() {
        if (!response_serializer_started_ && response_parser_->header_done()) {
            response_serializer_started_ = true;
            response_serializer_->start(response_parser_->head());
        }
        if (!response_serializer_started_) {
            return;
        }
        for (;;) {
            const std::size_t n =
                response_parser_->read_body(net::MutableBuffer{body_scratch_.data(), body_scratch_.size()});
            if (n == 0) {
                break;
            }
            response_serializer_->provide_body(net::ConstBuffer{body_scratch_.data(), n}, false);
        }
    }

    void pull_and_write_response_to_downstream() {
        const std::size_t n = response_serializer_->pull(net::MutableBuffer{write_buf_->data(), write_buf_->size()});
        if (n == 0) {
            if (response_serializer_->done()) {
                close();  // 요청 1회 + 응답 1회 완료 -- keep-alive 없이 정상 종료
            } else if (!response_parser_->message_done()) {
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

    // ---------- 종료 ----------

    void close() {
        if (closing_) {
            return;
        }
        closing_ = true;

        downstream_->shutdown();
        downstream_->close();

        // keep-alive를 지원하지 않으므로 upstream 연결도 항상 닫는다 --
        // UpstreamManager의 pool에는 절대 반납하지 않음 (release_connection()
        // 호출 없음). 자세한 이유는 클래스 주석 참고.
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
    std::size_t endpoint_index_ = 0;

    BufferPool& buffer_pool_;
    std::unique_ptr<std::vector<char>> read_buf_;
    std::unique_ptr<std::vector<char>> write_buf_;
    std::array<char, 4096> body_scratch_{};

    std::unique_ptr<net::http::IRequestParser> request_parser_;
    std::unique_ptr<net::http::IRequestSerializer> request_serializer_;
    bool request_serializer_started_ = false;
    bool request_final_body_provided_ = false;

    std::unique_ptr<net::http::IResponseParser> response_parser_;
    std::unique_ptr<net::http::IResponseSerializer> response_serializer_;
    bool response_serializer_started_ = false;
    bool response_final_body_provided_ = false;

    LocalMetrics& metrics_;
    bool closing_ = false;
};
