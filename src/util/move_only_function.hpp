#pragma once

#include <memory>
#include <type_traits>
#include <utility>

// std::function과 같은 타입 소거 콜백이지만, 복사 생성자를 명시적으로
// delete해서 move-only 캡처(예: unique_ptr)를 그대로 담을 수 있게 한
// 버전. C++23의 std::move_only_function과 동일한 목적 -- 이 프로젝트는
// C++20이라 직접 구현.
//
// net:: 인터페이스(ISocket 등)는 가상함수라 템플릿이 될 수 없고, 그래서
// 콜백 타입을 고정해야 했다. std::function으로 고정하면 "담기는 대상이
// 복사 가능해야 한다"는 제약이 생겨서, unique_ptr를 캡처한 람다를 넘길
// 때마다 shared_ptr<unique_ptr<T>>로 감싸는 boxing이 필요했다 (지금은
// 이 타입으로 대체돼서 그 boxing이 필요 없다).
template <typename Signature>
class MoveOnlyFunction;

template <typename R, typename... Args>
class MoveOnlyFunction<R(Args...)> {
public:
    MoveOnlyFunction() = default;
    MoveOnlyFunction(std::nullptr_t) noexcept {}

    template <typename F, typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, MoveOnlyFunction>>>
    MoveOnlyFunction(F&& f) : impl_(std::make_unique<Impl<std::decay_t<F>>>(std::forward<F>(f))) {}

    MoveOnlyFunction(MoveOnlyFunction&&) noexcept = default;
    MoveOnlyFunction& operator=(MoveOnlyFunction&&) noexcept = default;
    MoveOnlyFunction(const MoveOnlyFunction&) = delete;
    MoveOnlyFunction& operator=(const MoveOnlyFunction&) = delete;

    R operator()(Args... args) const { return impl_->call(std::forward<Args>(args)...); }

    explicit operator bool() const noexcept { return static_cast<bool>(impl_); }

private:
    struct ImplBase {
        virtual R call(Args...) = 0;
        virtual ~ImplBase() = default;
    };

    template <typename F>
    struct Impl : ImplBase {
        F f;
        explicit Impl(F&& f) : f(std::move(f)) {}
        R call(Args... args) override { return f(std::forward<Args>(args)...); }
    };

    std::unique_ptr<ImplBase> impl_;
};
