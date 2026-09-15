#pragma once

#include <cstring>
#include <vector>

#include "net/event_loop.hpp"
#include "net/socket.hpp"

// net::ISocket의 테스트용 가짜 구현체. 실제 소켓/네트워크 없이, 테스트
// 코드가 connect/read/write 결과를 직접 제어할 수 있게 해준다. 한 방향에
// pending 콜백은 하나만 허용 -- 실제 코드(Session)도 그렇게만 쓰므로
// 충분하다.
class FakeSocket : public net::ISocket {
public:
    explicit FakeSocket(net::IEventLoop& event_loop) : event_loop_(event_loop) {}

    // --- net::ISocket ---
    void async_connect(const net::Endpoint& endpoint, net::ErrorCallback cb) override {
        last_connect_endpoint_ = endpoint;
        connect_cb_ = std::move(cb);
    }

    void async_read_some(net::MutableBuffer buffer, net::IoCallback cb) override {
        read_buffer_ = buffer;
        read_cb_ = std::move(cb);
    }

    void async_write(net::ConstBuffer buffer, net::IoCallback cb) override {
        written_.insert(written_.end(), buffer.data, buffer.data + buffer.size);
        const net::Error err = next_write_error_;
        const std::size_t n = buffer.size;
        next_write_error_ = net::Error::none();
        event_loop_.post([cb = std::move(cb), err, n] { cb(err, n); });
    }

    void shutdown() override { shutdown_called_ = true; }
    void close() override {
        open_ = false;
        closed_ = true;
    }
    bool is_open() const override { return open_; }

    void cancel() override {
        cancel_called_ = true;
        if (connect_cb_) {
            auto cb = std::move(connect_cb_);
            connect_cb_ = nullptr;
            event_loop_.post([cb = std::move(cb)]() mutable { cb(net::Error{125, "operation cancelled"}); });
        }
        if (read_cb_) {
            auto cb = std::move(read_cb_);
            read_cb_ = nullptr;
            event_loop_.post([cb = std::move(cb)]() mutable { cb(net::Error{125, "operation cancelled"}, 0); });
        }
    }

    int release_native_handle() override { return -1; }  // fake라 실제 fd 없음

    // --- 테스트 제어용 ---
    // 아래 네 메서드 모두, 해당 방향에 pending 콜백이 없으면(이미
    // 소비됐거나 애초에 걸린 적 없으면) 조용히 아무 일도 안 한다 --
    // 실제 소켓도 pending 아닌 작업을 완료시킬 수 없는 것과 동일.
    // cancel()이 pending read/connect를 먼저 소비해버린 뒤 테스트가
    // 뒤늦게 complete_connect()/deliver_read() 등을 호출하는 경우가
    // 실제로 있어서(타임아웃 경쟁 테스트), 이 가드가 없으면 null
    // MoveOnlyFunction을 호출해 크래시난다.
    void complete_connect(net::Error err) {
        if (!connect_cb_) {
            return;
        }
        auto cb = std::move(connect_cb_);
        connect_cb_ = nullptr;
        event_loop_.post([cb = std::move(cb), err]() mutable { cb(err); });
    }

    void deliver_read(const std::string& data) {
        if (!read_cb_) {
            return;
        }
        auto cb = std::move(read_cb_);
        const auto buf = read_buffer_;
        read_cb_ = nullptr;
        const std::size_t n = std::min(data.size(), buf.size);
        std::memcpy(buf.data, data.data(), n);
        event_loop_.post([cb = std::move(cb), n]() mutable { cb(net::Error::none(), n); });
    }

    void fail_read(net::Error err) {
        if (!read_cb_) {
            return;
        }
        auto cb = std::move(read_cb_);
        read_cb_ = nullptr;
        event_loop_.post([cb = std::move(cb), err]() mutable { cb(err, 0); });
    }

    void set_next_write_error(net::Error err) { next_write_error_ = err; }

    bool has_pending_connect() const { return static_cast<bool>(connect_cb_); }
    bool has_pending_read() const { return static_cast<bool>(read_cb_); }
    bool cancel_called() const { return cancel_called_; }
    bool shutdown_called() const { return shutdown_called_; }
    bool closed() const { return closed_; }
    const std::vector<char>& written() const { return written_; }
    const net::Endpoint& last_connect_endpoint() const { return last_connect_endpoint_; }

private:
    net::IEventLoop& event_loop_;

    net::ErrorCallback connect_cb_;
    net::IoCallback read_cb_;
    net::MutableBuffer read_buffer_{};

    std::vector<char> written_;
    net::Error next_write_error_ = net::Error::none();
    net::Endpoint last_connect_endpoint_;

    bool open_ = true;
    bool closed_ = false;
    bool shutdown_called_ = false;
    bool cancel_called_ = false;
};
