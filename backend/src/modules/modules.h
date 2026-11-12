#pragma once

#include "tools/registry.h"

namespace thelife::modules {

// Binds every implemented tool to its handler. Called once at startup, before routes are registered.
void bindAll(tools::ToolRegistry& reg);

void bindPlanning(tools::ToolRegistry& reg);

}  // namespace thelife::modules
