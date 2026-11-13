#pragma once

#include <memory>

#include "llm/gateway.h"

namespace thelife::db {

// ops.provider_state persistence plus time-zone math done by Postgres (the toolchain's libstdc++ has no tzdb).
// Blocking: call from the LLM pool only.
std::shared_ptr<llm::StateStore> makeDbStateStore();

// llm_calls + llm_usage rows, written in the call's tenant context. Blocking: call from the LLM pool only.
std::shared_ptr<llm::UsageSink> makeDbUsageSink();

}  // namespace thelife::db
