cmake_minimum_required(VERSION 3.22)
project(perCoreShard CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    # 기본값은 RelWithDebInfo가 아니라 Debug: RelWithDebInfo/Release는
    # NDEBUG를 정의해서 GatewayShard/UpstreamManager의 assert() 기반
    # cross-thread 소유권 검증을 조용히 무력화시킨다. 이 프로젝트는
    # 아직 아키텍처 정확성을 검증하는 단계라 이 검증이 기본으로 켜져
    # 있어야 함 -- 그게 더 이상 우선순위가 아니게 되면 그때
    # -DCMAKE_BUILD_TYPE=Release를 명시적으로 지정할 것.
    set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(Boost 1.74 REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)

# hwloc (BSD, https://www.open-mpi.org/projects/hwloc/) -- P-core/E-core 같은
# 이기종 CPU 토폴로지 감지용 (Phase 5, src/util/cpu_topology.cpp). CMake
# config 패키지를 안 내놔서 pkg-config로 찾는다.
find_package(PkgConfig REQUIRED)
pkg_check_modules(HWLOC REQUIRED IMPORTED_TARGET hwloc)

# llhttp (MIT, https://github.com/nodejs/llhttp) -- HTTP 파싱만 담당하는
# 의존성 없는 C 라이브러리. third_party/llhttp/에 release 브랜치의
# pre-generated 소스를 vendoring (빌드에 Node.js 불필요). 직렬화는
# llhttp에 없어서 net/http/native/에 직접 구현 (외부 의존성 없음).
add_library(llhttp STATIC
    third_party/llhttp/src/api.c
    third_party/llhttp/src/http.c
    third_party/llhttp/src/llhttp.c
)
target_include_directories(llhttp PUBLIC third_party/llhttp/include)

set(PERCORESHARD_HTTP_SOURCES
    src/net/http/llhttp/factory.cpp
    src/net/http/llhttp/request_parser.cpp
    src/net/http/llhttp/response_parser.cpp
    src/net/http/native/factory.cpp
    src/net/http/native/request_serializer.cpp
    src/net/http/native/response_serializer.cpp
)

set(PERCORESHARD_FILTER_SOURCES
    src/filter/default_filters.cpp
)

add_executable(perCoreShard
    main.cpp
    src/runtime/gateway_shard.cpp
    src/runtime/gateway_runtime.cpp
    src/session/http_session.cpp
    src/upstream/upstream_manager.cpp
    src/util/cpu_topology.cpp
    src/net/race_with_timeout.cpp
    src/net/boost/acceptor.cpp
    src/net/boost/event_loop.cpp
    src/net/boost/factory.cpp
    src/net/boost/resolver.cpp
    src/net/boost/socket.cpp
    src/net/boost/timer.cpp
    ${PERCORESHARD_HTTP_SOURCES}
    ${PERCORESHARD_FILTER_SOURCES}
)

target_include_directories(perCoreShard PRIVATE src)
target_link_libraries(perCoreShard PRIVATE Boost::headers Boost::system Threads::Threads nlohmann_json::nlohmann_json llhttp PkgConfig::HWLOC)
target_compile_definitions(perCoreShard PRIVATE BOOST_ASIO_NO_DEPRECATED)

# 테스트: GTest가 없으면 조용히 건너뛰고 본체(perCoreShard)만 빌드한다.
# 테스트 대상(BufferPool/LocalMetrics/Config/UpstreamManager/Session)은
# net/ 인터페이스만 거치므로, 테스트 바이너리엔 Boost 구현체(net/boost/)가
# 전혀 필요 없다 -- tests/fakes/의 가짜 구현체로 대체.
find_package(GTest QUIET)
if(GTest_FOUND)
    enable_testing()

    add_executable(perCoreShard_tests
        src/upstream/upstream_manager.cpp
        src/util/cpu_topology.cpp
        src/net/race_with_timeout.cpp
        ${PERCORESHARD_HTTP_SOURCES}
        ${PERCORESHARD_FILTER_SOURCES}
        tests/buffer_pool_test.cpp
        tests/local_metrics_test.cpp
        tests/config_test.cpp
        tests/upstream_manager_test.cpp
        tests/session_test.cpp
        tests/http_request_parser_test.cpp
        tests/http_response_parser_test.cpp
        tests/http_request_serializer_test.cpp
        tests/http_response_serializer_test.cpp
        tests/filter_chain_test.cpp
        tests/body_filter_gate_test.cpp
        tests/cpu_topology_test.cpp
        tests/race_with_timeout_test.cpp
    )
    target_include_directories(perCoreShard_tests PRIVATE src tests)
    target_link_libraries(perCoreShard_tests PRIVATE
        nlohmann_json::nlohmann_json
        llhttp
        PkgConfig::HWLOC
        GTest::gtest
        GTest::gtest_main
    )

    include(GoogleTest)
    gtest_discover_tests(perCoreShard_tests)
else()
    message(STATUS "GTest not found -- skipping perCoreShard_tests target")
endif()
