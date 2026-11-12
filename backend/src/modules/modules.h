#pragma once

#include "tools/registry.h"

namespace thelife::modules {

// Binds every implemented tool to its handler. Called once at startup, before routes are registered.
void bindAll(tools::ToolRegistry& reg);

void bindPlanning(tools::ToolRegistry& reg);
void bindTasks(tools::ToolRegistry& reg);
void bindCalendar(tools::ToolRegistry& reg);
void bindWork(tools::ToolRegistry& reg);
void bindSkills(tools::ToolRegistry& reg);
void bindEntertainment(tools::ToolRegistry& reg);
void bindActivities(tools::ToolRegistry& reg);
void bindDetections(tools::ToolRegistry& reg);
void bindAccess(tools::ToolRegistry& reg);

}  // namespace thelife::modules
