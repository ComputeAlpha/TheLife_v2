import type { components } from "./schema";

type S = components["schemas"];
export type User = S["User"];
export type Area = S["Area"];
export type CalendarEntry = S["CalendarEntry"];
export type Task = S["Task"];
export type Detection = S["Detection"];
export type Approval = S["Approval"];
export type Score = S["Score"];
export type Device = S["Device"];

export const AREAS = ["work", "skill", "fitness", "personal", "entertainment", "other"] as const satisfies readonly Area[];
