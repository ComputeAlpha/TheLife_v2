import { AppWindow, Eye, Plug, UserRound, type LucideIcon } from "lucide-react";
import type { DetectionSource } from "@/api/types";
import type { ReviewStatus } from "@/lib/activity/review";

export const SOURCE_META: Record<DetectionSource, { label: string; icon: LucideIcon; hint: string }> = {
  vision: { label: "Screen", icon: Eye, hint: "Read from the screen by the collector" },
  process: { label: "Apps", icon: AppWindow, hint: "From the running app and window" },
  user: { label: "You", icon: UserRound, hint: "Entered or corrected by you" },
  external: { label: "External", icon: Plug, hint: "From another service" },
};

export const STATUS_META: Record<ReviewStatus, { label: string; tone: "neutral" | "good" | "warn" | "accent" }> = {
  detected: { label: "To review", tone: "accent" },
  confirmed: { label: "Confirmed", tone: "good" },
  disputed: { label: "Under review", tone: "warn" },
  superseded: { label: "Corrected", tone: "neutral" },
};

export function ConfidenceMeter({ value, className }: { value: number | undefined; className?: string }) {
  const v = value ?? 1;
  const pct = Math.round(v * 100);
  const color = v >= 0.75 ? "var(--color-good)" : v >= 0.4 ? "var(--color-accent)" : "var(--color-warn)";
  return (
    <span className={className} role="meter" aria-label={`Confidence ${pct}%`} aria-valuemin={0} aria-valuemax={100} aria-valuenow={pct} title={`Confidence ${pct}%`}>
      <span className="flex h-1 w-10 overflow-hidden rounded-full bg-surface-3">
        <span className="h-full rounded-full" style={{ width: `${pct}%`, background: color }} />
      </span>
    </span>
  );
}
