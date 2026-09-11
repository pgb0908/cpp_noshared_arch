#pragma once

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

struct UpstreamEndpointConfig {
    std::string host;
    unsigned short port = 0;
};

struct Config {
    unsigned short listen_port = 8080;
    std::size_t shard_count = std::thread::hardware_concurrency();
    std::size_t buffer_size = 64 * 1024;
    std::vector<UpstreamEndpointConfig> upstreams;
    unsigned dns_refresh_interval_seconds = 30;
    std::size_t connection_pool_max_idle_per_endpoint = 8;

    static Config from_file(const std::string& path) {
        std::ifstream in(path);
        if (!in) {
            std::cerr << "Cannot open config file: " << path << "\n";
            std::exit(1);
        }

        nlohmann::json j;
        try {
            in >> j;
        } catch (const std::exception& e) {
            std::cerr << "Invalid JSON in " << path << ": " << e.what() << "\n";
            std::exit(1);
        }

        Config config;
        try {
            if (j.contains("listen_port")) {
                config.listen_port = j.at("listen_port").get<unsigned short>();
            }
            if (j.contains("shard_count")) {
                config.shard_count = j.at("shard_count").get<std::size_t>();
            }
            if (j.contains("buffer_size")) {
                config.buffer_size = j.at("buffer_size").get<std::size_t>();
            }
            if (j.contains("dns_refresh_interval_seconds")) {
                config.dns_refresh_interval_seconds = j.at("dns_refresh_interval_seconds").get<unsigned>();
            }
            if (j.contains("connection_pool_max_idle_per_endpoint")) {
                config.connection_pool_max_idle_per_endpoint =
                    j.at("connection_pool_max_idle_per_endpoint").get<std::size_t>();
            }

            if (!j.contains("upstreams") || !j.at("upstreams").is_array() || j.at("upstreams").empty()) {
                std::cerr << "config: \"upstreams\" must be a non-empty array\n";
                std::exit(1);
            }
            for (const auto& item : j.at("upstreams")) {
                UpstreamEndpointConfig ep;
                ep.host = item.at("host").get<std::string>();
                ep.port = item.at("port").get<unsigned short>();
                config.upstreams.push_back(std::move(ep));
            }
        } catch (const std::exception& e) {
            std::cerr << "config: malformed field in " << path << ": " << e.what() << "\n";
            std::exit(1);
        }

        if (config.shard_count == 0) {
            config.shard_count = 1;
        }

        return config;
    }

    static Config from_args(int argc, char** argv) {
        std::string config_path;

        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--config") {
                if (i + 1 >= argc) {
                    std::cerr << "Missing value for --config\n";
                    std::exit(1);
                }
                config_path = argv[++i];
            } else if (arg == "--help" || arg == "-h") {
                std::cout << "Usage: perCoreShard --config <path/to/config.json>\n";
                std::exit(0);
            } else {
                std::cerr << "Unknown argument: " << arg << "\n";
                std::exit(1);
            }
        }

        if (config_path.empty()) {
            std::cerr << "Usage: perCoreShard --config <path/to/config.json>\n";
            std::exit(1);
        }

        return from_file(config_path);
    }
};
