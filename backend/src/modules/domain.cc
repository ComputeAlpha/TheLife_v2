// Work, skills, entertainment and activities (api/resources/work_*.yaml, skill_*.yaml, entertainment_*.yaml,
// activities.yaml): contract-driven CRUD. Cross-row rules (book vs course sessions, section ownership, links,
// derived progress and delivered_at) live in the schema (migrations 0004, 0005).
#include "crud/resource.h"
#include "modules/common.h"
#include "modules/modules.h"

namespace thelife::modules {

void bindWork(tools::ToolRegistry& reg) {
  crud::bindCrud(reg, {.prefix = "work_roles", .table = "work_roles", .filters = {}, .validate = nullptr});
  crud::bindCrud(reg, {.prefix = "work_responsibilities",
                       .table = "work_responsibilities",
                       .filters = {},
                       .validate = [](Json::Value& f, bool) { requireValidRrule(f, "cadence"); }});
  crud::bindCrud(reg, {.prefix = "work_projects", .table = "work_projects", .filters = {}, .validate = nullptr});
  crud::bindCrud(reg, {.prefix = "work_deliverables",
                       .table = "work_deliverables",
                       .filters = {{"due_before", "r.due_at < $?::timestamptz"}},
                       .validate = nullptr});
}

void bindSkills(tools::ToolRegistry& reg) {
  crud::bindCrud(reg, {.prefix = "skill_resources", .table = "skill_resources", .filters = {}, .validate = nullptr});
  crud::bindCrud(reg, {.prefix = "skill_sections", .table = "skill_sections", .filters = {}, .validate = nullptr});
  crud::bindCrud(reg, {.prefix = "skill_sessions",
                       .table = "skill_sessions",
                       .filters = intervalFilters("started_at", "ended_at"),
                       .validate = nullptr});
}

void bindEntertainment(tools::ToolRegistry& reg) {
  crud::bindCrud(reg, {.prefix = "entertainment_items", .table = "entertainment_items", .filters = {},
                       .validate = nullptr});
  crud::bindCrud(reg, {.prefix = "entertainment_slots",
                       .table = "entertainment_slots",
                       .filters = intervalFilters("starts_at", "ends_at"),
                       .validate = nullptr});
}

void bindActivities(tools::ToolRegistry& reg) {
  crud::bindCrud(reg, {.prefix = "activities",
                       .table = "activities",
                       .filters = intervalFilters("started_at", "ended_at"),
                       .validate = nullptr});
}

}  // namespace thelife::modules
