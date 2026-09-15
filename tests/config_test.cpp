#include "config/config.hpp"

#include <fstream>
#include <gtest/gtest.h>

// 주의: Config::from_file()은 malformed JSON이나 필수 필드 누락 시
// std::exit(1)을 직접 호출한다 (예외를 던지지 않음) -- 그래서 에러
// 경로는 테스트 프로세스 자체를 죽여버리기 때문에 단위 테스트로 검증할
// 수 없다. 여기선 성공 경로만 다룬다. 에러 경로까지 테스트하려면
// Config::from_file이 exit() 대신 예외를 던지도록 먼저 리팩토링해야
// 한다 (main.cpp에서 catch해서 exit하는 형태로).

namespace {

std::string write_temp_config(const std::string& content) {
    const std::string path = testing::TempDir() + "percoreshard_config_test.json";
    std::ofstream out(path);
    out << content;
    out.close();
    return path;
}

}  // namespace

TEST(Config, 모든_필드가_있으면_그대로_파싱된다) {
    const std::string path = write_temp_config(R"({
        "listen_port": 9999,
        "shard_count": 7,
        "buffer_size": 4096,
        "dns_refresh_interval_seconds": 15,
        "connection_pool_max_idle_per_endpoint": 3,
        "connect_timeout_seconds": 2,
        "buffer_pool_max_free": 10,
        "metrics_report_interval_seconds": 5,
        "upstreams": [
            { "host": "127.0.0.1", "port": 8000 },
            { "host": "10.0.0.1", "port": 9000 }
        ]
    })");

    const Config config = Config::from_file(path);

    EXPECT_EQ(config.listen_port, 9999);
    EXPECT_EQ(config.shard_count, 7u);
    EXPECT_EQ(config.buffer_size, 4096u);
    EXPECT_EQ(config.dns_refresh_interval_seconds, 15u);
    EXPECT_EQ(config.connection_pool_max_idle_per_endpoint, 3u);
    EXPECT_EQ(config.connect_timeout_seconds, 2u);
    EXPECT_EQ(config.buffer_pool_max_free, 10u);
    EXPECT_EQ(config.metrics_report_interval_seconds, 5u);

    ASSERT_EQ(config.upstreams.size(), 2u);
    EXPECT_EQ(config.upstreams[0].host, "127.0.0.1");
    EXPECT_EQ(config.upstreams[0].port, 8000);
    EXPECT_EQ(config.upstreams[1].host, "10.0.0.1");
    EXPECT_EQ(config.upstreams[1].port, 9000);
}

TEST(Config, upstreams만_있으면_나머지는_기본값이다) {
    const std::string path = write_temp_config(R"({
        "upstreams": [ { "host": "127.0.0.1", "port": 8000 } ]
    })");

    const Config config = Config::from_file(path);

    EXPECT_EQ(config.listen_port, 8080);
    EXPECT_EQ(config.dns_refresh_interval_seconds, 30u);
    EXPECT_EQ(config.connection_pool_max_idle_per_endpoint, 8u);
    EXPECT_EQ(config.connect_timeout_seconds, 5u);
    EXPECT_EQ(config.buffer_pool_max_free, 256u);
    EXPECT_EQ(config.metrics_report_interval_seconds, 10u);
    ASSERT_EQ(config.upstreams.size(), 1u);
}

TEST(Config, shard_count가_0이면_1로_보정된다) {
    const std::string path = write_temp_config(R"({
        "shard_count": 0,
        "upstreams": [ { "host": "127.0.0.1", "port": 8000 } ]
    })");

    const Config config = Config::from_file(path);
    EXPECT_EQ(config.shard_count, 1u);
}
