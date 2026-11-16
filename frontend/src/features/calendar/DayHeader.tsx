import * as DM from "@radix-ui/react-dropdown-menu";
import { MoreHorizontal, Check } from "lucide-react";
import type { DayProfileOverride, Score } from "@/api/types";
import { fmtDay, type Day } from "@/lib/calendar/days";
import { Badge } from "@/ui/Badge";
import { cn } from "@/ui/cn";

export const PROFILE_TONE: Record<string, "accent" | "good" | "alert" | "warn" | "neutral"> = {
  holiday: "good",
  leave: "warn",
  sick: "alert",
  weekend: "neutral",
  workday: "neutral",
};

export function scoreColor(final: number | null | undefined): string {
  if (final == null) return "var(--color-text-3)";
  if (final >= 80) return "var(--color-good)";
  if (final >= 60) return "var(--color-accent)";
  if (final >= 40) return "var(--color-warn)";
  return "var(--color-alert-text)";
}

export function ScoreDot({ score }: { score?: Score }) {
  if (!score || score.final == null) return null;
  return (
    <span
      className="inline-flex items-center gap-1 text-[11px] tabular text-text-2"
      title={`Score ${Math.round(score.final)}${score.status === "provisional" ? " (provisional)" : ""}`}
    >
      <span className={cn("size-2 rounded-full", score.status === "provisional" && "opacity-60")} style={{ background: scoreColor(score.final) }} />
      {Math.round(score.final)}
    </span>
  );
}

/** Day profile override menu ("Mark as leave / sick / holiday …"). */
export function ProfileMenu({ day, profiles, override, onSet, children }: {
  day: Day;
  profiles: string[];
  override?: DayProfileOverride;
  onSet: (profile: string | null) => void;
  children?: React.ReactNode;
}) {
  return (
    <DM.Root>
      <DM.Trigger
        className="grid size-6 place-items-center rounded-md text-text-3 opacity-70 hover:bg-surface-2 hover:text-text hover:opacity-100 focus-visible:opacity-100"
        aria-label={`Day options for ${fmtDay(day, "EEEE d MMMM")}`}
      >
        {children ?? <MoreHorizontal className="size-4" />}
      </DM.Trigger>
      <DM.Portal>
        <DM.Content align="start" sideOffset={6} className="z-50 min-w-52 rounded-xl border border-line-strong bg-bg p-1.5 shadow-2xl">
          <DM.Label className="px-3 pb-1 pt-1.5 text-[11px] font-semibold uppercase tracking-wider text-text-3">Kind of day</DM.Label>
          {profiles.map((p) => (
            <DM.Item key={p} onSelect={() => onSet(p)}
              className="flex cursor-pointer items-center gap-2 rounded-lg px-3 py-2 text-sm capitalize text-text-2 outline-none data-[highlighted]:bg-surface-2 data-[highlighted]:text-text">
              <Check className={cn("size-3.5", override?.profile === p ? "text-accent" : "invisible")} />
              {p}
            </DM.Item>
          ))}
          {override ? (
            <>
              <DM.Separator className="my-1 h-px bg-line" />
              <DM.Item onSelect={() => onSet(null)}
                className="cursor-pointer rounded-lg px-3 py-2 text-sm text-text-2 outline-none data-[highlighted]:bg-surface-2 data-[highlighted]:text-text">
                Back to the usual
              </DM.Item>
            </>
          ) : null}
        </DM.Content>
      </DM.Portal>
    </DM.Root>
  );
}

export function ProfileBadge({ override }: { override?: DayProfileOverride }) {
  if (!override) return null;
  return <Badge tone={PROFILE_TONE[override.profile] ?? "accent"} className="capitalize" title={override.note ?? undefined}>{override.profile}</Badge>;
}
