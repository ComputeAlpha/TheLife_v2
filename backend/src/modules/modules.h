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
void bindAgent(tools::ToolRegistry& reg);
void bindNotifications(tools::ToolRegistry& reg);
void bindMemory(tools::ToolRegistry& reg);
void bindWeb(tools::ToolRegistry& reg);
void bindScoring(tools::ToolRegistry& reg);
void bindProactive(tools::ToolRegistry& reg);
void bindTelegram(tools::ToolRegistry& reg);

}  // namespace thelife::modules
