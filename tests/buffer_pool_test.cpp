#include "util/buffer_pool.hpp"

#include <gtest/gtest.h>

TEST(BufferPool, acquire는_요청한_크기의_버퍼를_반환한다) {
    BufferPool pool(1024, 8);
    auto buf = pool.acquire();
    ASSERT_NE(buf, nullptr);
    EXPECT_EQ(buf->size(), 1024u);
}

TEST(BufferPool, free_list가_비어있으면_매번_새로_할당한다) {
    BufferPool pool(64, 8);
    auto a = pool.acquire();
    auto b = pool.acquire();
    // release 없이 연속 acquire하면 free-list를 쓸 수 없으므로 서로
    // 다른 할당이어야 한다.
    EXPECT_NE(a.get(), b.get());
    EXPECT_EQ(pool.free_count(), 0u);
}

TEST(BufferPool, release한_버퍼는_다음_acquire에서_재사용된다) {
    BufferPool pool(64, 8);
    auto buf = pool.acquire();
    const auto* raw = buf.get();

    pool.release(std::move(buf));
    EXPECT_EQ(pool.free_count(), 1u);

    auto reused = pool.acquire();
    // 새로 할당한 게 아니라 free-list에서 꺼낸 "같은 객체"여야 한다.
    EXPECT_EQ(reused.get(), raw);
    EXPECT_EQ(pool.free_count(), 0u);
}

TEST(BufferPool, release는_LIFO_순서로_재사용된다) {
    BufferPool pool(64, 8);
    auto a = pool.acquire();
    auto b = pool.acquire();
    const auto* a_raw = a.get();
    const auto* b_raw = b.get();

    pool.release(std::move(a));
    pool.release(std::move(b));  // 마지막에 반납한 b가

    auto first = pool.acquire();
    EXPECT_EQ(first.get(), b_raw);  // 먼저 나와야 함 (LIFO)
    auto second = pool.acquire();
    EXPECT_EQ(second.get(), a_raw);
}

TEST(BufferPool, max_free_상한을_넘으면_초과분은_버려진다) {
    BufferPool pool(64, 2);  // free-list 상한 2
    auto a = pool.acquire();
    auto b = pool.acquire();
    auto c = pool.acquire();

    pool.release(std::move(a));
    pool.release(std::move(b));
    EXPECT_EQ(pool.free_count(), 2u);

    pool.release(std::move(c));  // 상한 초과 -- 그냥 버려짐
    EXPECT_EQ(pool.free_count(), 2u);
}

TEST(BufferPool, null_버퍼_release는_아무_효과가_없다) {
    BufferPool pool(64, 8);
    std::unique_ptr<std::vector<char>> null_buf;
    pool.release(std::move(null_buf));
    EXPECT_EQ(pool.free_count(), 0u);
}
