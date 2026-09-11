#pragma once

#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

struct Config {
    unsigned short listen_port = 8080;
    std::string upstream_host = "127.0.0.1";
    unsigned short upstream_port = 8000;
    std::size_t shard_count = std::thread::hardware_concurrency();
    std::size_t buffer_size = 64 * 1024;

    static Config from_args(int argc, char** argv) {
        Config config;
        if (config.shard_count == 0) {
            config.shard_count = 1;
        }

        auto next_value = [&](int& i) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << argv[i] << "\n";
                std::exit(1);
            }
            return argv[++i];
        };

        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--listen-port") {
                config.listen_port = static_cast<unsigned short>(std::stoi(next_value(i)));
            } else if (arg == "--upstream-host") {
                config.upstream_host = next_value(i);
            } else if (arg == "--upstream-port") {
                config.upstream_port = static_cast<unsigned short>(std::stoi(next_value(i)));
            } else if (arg == "--shards") {
                config.shard_count = static_cast<std::size_t>(std::stoul(next_value(i)));
            } else if (arg == "--help" || arg == "-h") {
                std::cout << "Usage: perCoreShard [--listen-port PORT] [--upstream-host HOST] "
                             "[--upstream-port PORT] [--shards N]\n";
                std::exit(0);
            } else {
                std::cerr << "Unknown argument: " << arg << "\n";
                std::exit(1);
            }
        }

        return config;
    }
};
