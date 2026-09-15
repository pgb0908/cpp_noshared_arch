#pragma once

#include <cstddef>
#include <memory>
#include <vector>

// shard-local free-list 기반 버퍼 풀. 항상 소유 shard의 event loop
// 스레드에서만 호출되므로 락이 필요 없다. free-list 크기는
// buffer_pool_max_free로 상한을 둬서, 트래픽이 몰렸다 빠진 뒤에도
// 버퍼가 무한정 쌓이지 않게 한다.
class BufferPool {
public:
    BufferPool(std::size_t buffer_size, std::size_t max_free)
        : buffer_size_(buffer_size), max_free_(max_free) {}

    std::unique_ptr<std::vector<char>> acquire() {
        if (!free_list_.empty()) {
            auto buf = std::move(free_list_.back());
            free_list_.pop_back();
            return buf;
        }
        return std::make_unique<std::vector<char>>(buffer_size_);
    }

    void release(std::unique_ptr<std::vector<char>> buf) {
        if (!buf) {
            return;
        }
        if (free_list_.size() >= max_free_) {
            return;  // 상한 초과 -- 그냥 버림 (buf가 여기서 소멸)
        }
        free_list_.push_back(std::move(buf));
    }

    // 테스트/관찰용: 현재 free-list에 쌓여있는 버퍼 개수.
    std::size_t free_count() const { return free_list_.size(); }

private:
    std::size_t buffer_size_;
    std::size_t max_free_;
    std::vector<std::unique_ptr<std::vector<char>>> free_list_;
};
