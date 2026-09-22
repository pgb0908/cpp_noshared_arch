#pragma once

#include <any>
#include <unordered_map>

// 타입된 key. 같은 요청을 처리하는 서로 다른 필터끼리 데이터를 주고받을 때
// 쓴다 (예: 인증 필터가 심은 user_id를 rate-limit 필터가 읽음). key 자체의
// 주소로 식별하므로 문자열 오타/타입 불일치가 컴파일 타임에 걸러진다 --
// get<T>()/set<T>()가 key의 T를 그대로 따라간다.
template <typename T>
struct ContextKey {
    const char* debug_name;
};

// HttpSession 하나(=요청 하나)를 위해 새로 만들어지고 세션과 함께
// 소멸하는 상태 저장소. 필터 인스턴스 자체는 shard당 싱글턴으로 공유되지만
// (필터 간 멤버 변수 공유는 불가능하므로), 같은 요청을 지나가는 필터들이
// request 단계에서 심은 값을 response 단계 또는 다른 필터에서 읽고 싶을 때
// 이 context에 저장한다.
class FilterContext {
public:
    template <typename T>
    void set(const ContextKey<T>& key, T value) {
        data_[&key] = std::move(value);
    }

    template <typename T>
    T* get(const ContextKey<T>& key) {
        auto it = data_.find(&key);
        if (it == data_.end()) {
            return nullptr;
        }
        return std::any_cast<T>(&it->second);
    }

private:
    std::unordered_map<const void*, std::any> data_;
};
