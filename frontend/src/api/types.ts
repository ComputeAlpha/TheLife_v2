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
