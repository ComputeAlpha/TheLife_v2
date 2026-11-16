import { Briefcase, Dumbbell, GraduationCap, Heart, Gamepad2, Shapes, type LucideIcon } from "lucide-react";
import type { Area } from "@/api/types";

// Area colour always comes with an icon, so colour is never the only cue (frontend_lld.md §3.1).
export const AREA_META: Record<Area, { label: string; color: string; icon: LucideIcon }> = {
  work: { label: "Work", color: "var(--color-area-work)", icon: Briefcase },
  skill: { label: "Skill", color: "var(--color-area-skill)", icon: GraduationCap },
  fitness: { label: "Fitness", color: "var(--color-area-fitness)", icon: Dumbbell },
  personal: { label: "Personal", color: "var(--color-area-personal)", icon: Heart },
  entertainment: { label: "Entertainment", color: "var(--color-area-entertainment)", icon: Gamepad2 },
  other: { label: "Other", color: "var(--color-area-other)", icon: Shapes },
};

export function areaMeta(a: string | null | undefined) {
  return AREA_META[(a ?? "other") as Area] ?? AREA_META.other;
}
