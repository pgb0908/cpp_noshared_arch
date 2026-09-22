#pragma once

#include "filter/filter_chain.hpp"

// 이번 MVP의 기본 필터 목록을 코드에서 직접 조립한다. config 기반 동적
// 구성이 필요해지면 RuntimeConfigManager와 함께 재검토 (doc/plan.md).
FilterChain build_default_filter_chain();
