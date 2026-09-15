#pragma once

#include <chrono>

#include "net/event_loop.hpp"
#include "net/timer.hpp"

// net::ITimer의 테스트용 가짜 구현체. 실제 시간이 흐르지 않고, 테스트가
// fire()를 직접 호출해야만 타임아웃이 발생한 것처럼 콜백이 실행된다.
class FakeTimer : public net::ITimer {
public:
    explicit FakeTimer(net::IEventLoop& event_loop) : event_loop_(event_loop) {}

    void expires_after(std::chrono::seconds duration) override { last_duration_ = duration; }

    void async_wait(net::ErrorCallback cb) override { cb_ = std::move(cb); }

    void cancel() override {
        if (cb_) {
            auto cb = std::move(cb_);
            cb_ = nullptr;
            event_loop_.post([cb = std::move(cb)]() mutable { cb(net::Error{125, "timer cancelled"}); });
        }
    }

    // --- 테스트 제어용: 실제로 타임아웃이 발생한 것처럼 콜백을 실행 ---
    void fire() {
        auto cb = std::move(cb_);
        cb_ = nullptr;
        if (cb) {
            event_loop_.post([cb = std::move(cb)]() mutable { cb(net::Error::none()); });
        }
    }

    bool has_pending() const { return static_cast<bool>(cb_); }
    std::chrono::seconds last_duration() const { return last_duration_; }

private:
    net::IEventLoop& event_loop_;
    net::ErrorCallback cb_;
    std::chrono::seconds last_duration_{0};
};
