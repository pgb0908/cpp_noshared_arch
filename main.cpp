#include "config/config.hpp"
#include "runtime/gateway_runtime.hpp"

int main(int argc, char** argv) {
    Config config = Config::from_args(argc, argv);

    GatewayRuntime runtime(config);
    runtime.start();
    runtime.run_until_signal();
    runtime.print_metrics_summary();

    return 0;
}
