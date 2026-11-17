// Smart groups for the Tasks page (frontend_lld.md §6.9).
import type { Task } from "@/api/types";
import { addDays, dayStartMs, type Day } from "./calendar/days";

export type Group = "today" | "overdue" | "upcoming" | "recurring" | "nodate" | "done";
export const GROUPS: { id: Group; label: string }[] = [
  { id: "today", label: "Today" },
  { id: "overdue", label: "Overdue" },
  { id: "upcoming", label: "Upcoming" },
  { id: "recurring", label: "Recurring" },
  { id: "nodate", label: "No date" },
  { id: "done", label: "Done" },
];

/** When the user's local day `day` ends: the next day's day start (day_start_hour, e.g. 04:00). */
export function localDayEndMs(day: Day, tz: string, dayStartHour: number): number {
  return dayStartMs(addDays(day, 1), tz) + dayStartHour * 3_600_000;
}

/**
 * The one group a task belongs to. "Today" runs to the end of the user's local day (which respects day_start_hour);
 * overdue is due before now and still open.
 */
export function groupOf(t: Task, now: number, today: Day, tz: string, dayStartHour = 0): Group {
  if (t.status === "done" || t.status === "cancelled") return "done";
  if (t.rrule) return "recurring";
  if (!t.due_at) return "nodate";
  const due = Date.parse(t.due_at);
  if (due < now) return "overdue";
  return due < localDayEndMs(today, tz, dayStartHour) ? "today" : "upcoming";
}

/** Within a group: soonest due first, then highest priority, then title. Done: most recently completed first. */
export function sortTasks(list: Task[], group: Group): Task[] {
  return [...list].sort((a, b) =>
    group === "done"
      ? (b.completed_at ?? b.updated_at).localeCompare(a.completed_at ?? a.updated_at)
      : (a.due_at ?? "9999").localeCompare(b.due_at ?? "9999") || (b.priority ?? 1) - (a.priority ?? 1) || a.title.localeCompare(b.title));
}

export const PRIORITY: Record<number, { label: string; marks: string }> = {
  0: { label: "Low", marks: "" },
  1: { label: "Normal", marks: "" },
  2: { label: "High", marks: "!!" },
  3: { label: "Critical", marks: "!!!" },
};
