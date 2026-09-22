#include "filter/default_filters.hpp"

#include <memory>

#include "filter/via_header_filter.hpp"

FilterChain build_default_filter_chain() {
    FilterChain chain;
    chain.add_filter(std::make_unique<ViaHeaderFilter>());
    return chain;
}
