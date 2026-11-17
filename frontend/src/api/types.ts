import type { components } from "./schema";

type S = components["schemas"];
export type User = S["User"];
export type Area = S["Area"];
export type CalendarEntry = S["CalendarEntry"];
export type CalendarEvent = S["CalendarEvent"];
export type CalendarEventCreate = S["CalendarEventCreate"];
export type CalendarEventPatch = S["CalendarEventPatch"];
export type Task = S["Task"];
export type Detection = S["Detection"];
export type Approval = S["Approval"];
export type Score = S["Score"];
export type Device = S["Device"];
export type DayProfile = S["DayProfile"];
export type DayProfileOverride = S["DayProfileOverride"];
export type DayProgress = S["DayProgress"];
export type Reminder = S["Reminder"];
export type Routine = S["Routine"];
export type SkillResource = S["SkillResource"];
export type SkillSession = S["SkillSession"];
export type EntertainmentItem = S["EntertainmentItem"];
export type EntertainmentSlot = S["EntertainmentSlot"];

export const AREAS = ["work", "skill", "fitness", "personal", "entertainment", "other"] as const satisfies readonly Area[];

/** Shape of DayProgress.areas[area] (scoring engine `progress()`; the contract leaves it open). */
export interface AreaProgress {
  planned_items: number;
  done_items: number;
  block_minutes_so_far: number;
  block_minutes_covered: number;
  targets: { title: string; metric: string; amount: number; achieved: number }[];
}

export type Activity = S["Activity"];
export type ActivityCreate = S["ActivityCreate"];
export type ActivityPatch = S["ActivityPatch"];
export type DetectionHistory = S["DetectionHistory"];
export type DetectionEvent = S["DetectionEvent"];
export type Goal = S["Goal"];
export type GoalCreate = S["GoalCreate"];
export type GoalPatch = S["GoalPatch"];
export type Plan = S["Plan"];
export type PlanCreate = S["PlanCreate"];
export type PlanPatch = S["PlanPatch"];
export type Target = S["Target"];
export type TargetCreate = S["TargetCreate"];
export type TargetPatch = S["TargetPatch"];

/** Detection.source as the server reports it (the generated read type carries the create-only enum). */
export type DetectionSource = "vision" | "process" | "user" | "external";
export function detectionSource(d: Detection): DetectionSource {
  return d.source as DetectionSource;
}

export type TaskCreate = S["TaskCreate"];
export type TaskPatch = S["TaskPatch"];
export type TaskOccurrence = S["TaskOccurrence"];
export type SkillResourceCreate = S["SkillResourceCreate"];
export type CourseSection = S["CourseSection"];
export type SkillSessionCreate = S["SkillSessionCreate"];
export type EntertainmentItemCreate = S["EntertainmentItemCreate"];
export type EntertainmentSlotCreate = S["EntertainmentSlotCreate"];
export type WorkRole = S["WorkRole"];
export type WorkRoleCreate = S["WorkRoleCreate"];
export type WorkResponsibility = S["WorkResponsibility"];
export type WorkResponsibilityCreate = S["WorkResponsibilityCreate"];
export type WorkProject = S["WorkProject"];
export type WorkProjectCreate = S["WorkProjectCreate"];
export type WorkDeliverable = S["WorkDeliverable"];
export type WorkDeliverableCreate = S["WorkDeliverableCreate"];
export type WorkContext = S["WorkContext"];
export type WorkContextCreate = S["WorkContextCreate"];
export type RoutineCreate = S["RoutineCreate"];
export type ReminderCreate = S["ReminderCreate"];
export type MemoryFact = S["MemoryFact"];
export type IngestRule = S["IngestRule"];
export type IngestRuleCreate = S["IngestRuleCreate"];
export type DayProfileCreate = S["DayProfileCreate"];
export type ApiKey = S["ApiKey"];
export type Scope = S["Scope"];
export type ScoringSettings = S["ScoringSettings"];

/** Every API-key scope, checked both ways against the contract's enum at compile time. */
export const SCOPES = [
  "activities:read", "activities:write", "calendar:read", "calendar:write", "detections:read", "detections:write",
  "entertainment:read", "entertainment:write", "planning:read", "planning:write", "scoring:read", "scoring:write",
  "skills:read", "skills:write", "tasks:read", "tasks:write", "work:read", "work:write",
] as const satisfies readonly Scope[];
type _AllScopes = Exclude<Scope, (typeof SCOPES)[number]> extends never ? true : never;
export const _scopesComplete: _AllScopes = true;
