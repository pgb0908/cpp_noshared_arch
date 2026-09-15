#pragma once

#include <unordered_map>
#include <utility>
#include <vector>

#include "net/resolver.hpp"

// net::IResolver의 테스트용 가짜 구현체. 실제 DNS 조회 없이, host별로
// resolve() 결과를 미리 정해둘 수 있게 한다 (host가 다르면 결과도
// 달라야 다중 upstream 시나리오를 제대로 테스트할 수 있어서 host별로
// 관리).
class FakeResolver : public net::IResolver {
public:
    void set_result_for(const std::string& host, net::Error err, std::vector<net::Endpoint> endpoints) {
        results_[host] = {std::move(err), std::move(endpoints)};
    }

    std::pair<net::Error, std::vector<net::Endpoint>> resolve(const std::string& host, uint16_t port) override {
        ++call_count_;
        last_host_ = host;
        last_port_ = port;

        const auto it = results_.find(host);
        if (it == results_.end()) {
            return {net::Error::none(), {}};  // 설정 안 해둔 host -- 빈 결과
        }
        return it->second;
    }

    int call_count() const { return call_count_; }
    const std::string& last_host() const { return last_host_; }

private:
    std::unordered_map<std::string, std::pair<net::Error, std::vector<net::Endpoint>>> results_;
    int call_count_ = 0;
    std::string last_host_;
    uint16_t last_port_ = 0;
};
