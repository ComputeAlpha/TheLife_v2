// Goals and plans (api/resources/goals.yaml, plans.yaml): plain contract-driven CRUD.
#include "crud/resource.h"
#include "modules/modules.h"

namespace thelife::modules {

void bindPlanning(tools::ToolRegistry& reg) {
  crud::bindCrud(reg, {.prefix = "goals", .table = "goals", .filters = {}, .validate = nullptr});
  crud::bindCrud(reg, {.prefix = "plans",
                       .table = "plans",
                       .filters = {{"active_on", "r.period_start <= $?::date AND r.period_end >= $?::date"}},
                       .validate = nullptr});
}

void bindAll(tools::ToolRegistry& reg) {
  bindPlanning(reg);
  bindAgent(reg);
  bindNotifications(reg);
  bindMemory(reg);
  bindTasks(reg);
  bindCalendar(reg);
  bindWork(reg);
  bindSkills(reg);
  bindEntertainment(reg);
  bindActivities(reg);
  bindDetections(reg);
  bindAccess(reg);
}

}  // namespace thelife::modules
