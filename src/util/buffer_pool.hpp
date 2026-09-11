#pragma once

#include <cstddef>
#include <memory>
#include <vector>

// MVP shape: no free-list yet, just allocates a fresh buffer per acquire().
// Always called from the owning shard's io_context thread, so no locking.
// The acquire()/release() call sites in Session are shaped so a real
// free-list can drop in later (Phase 4) without touching callers.
class BufferPool {
public:
    explicit BufferPool(std::size_t buffer_size) : buffer_size_(buffer_size) {}

    std::unique_ptr<std::vector<char>> acquire() {
        return std::make_unique<std::vector<char>>(buffer_size_);
    }

    void release(std::unique_ptr<std::vector<char>> buf) {
        // MVP: just let it destruct. Future: return to a shard-local free-list.
        buf.reset();
    }

private:
    std::size_t buffer_size_;
};
