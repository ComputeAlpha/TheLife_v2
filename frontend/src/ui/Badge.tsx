import type { HTMLAttributes } from "react";
import { cn } from "./cn";

type Tone = "neutral" | "accent" | "good" | "alert" | "warn";
const tones: Record<Tone, string> = {
  neutral: "bg-surface-3 text-text-2 border-line-strong",
  accent: "bg-accent/12 text-accent border-accent/30",
  good: "bg-good/12 text-good border-good/30",
  alert: "bg-alert/15 text-alert-text border-alert/35",
  warn: "bg-warn/12 text-warn border-warn/30",
};

export function Badge({ tone = "neutral", className, ...rest }: HTMLAttributes<HTMLSpanElement> & { tone?: Tone }) {
  return (
    <span
      className={cn("inline-flex items-center gap-1 rounded-full border px-2 py-0.5 text-[11.5px] font-medium leading-4", tones[tone], className)}
      {...rest}
    />
  );
}

export function Kbd({ className, ...rest }: HTMLAttributes<HTMLElement>) {
  return (
    <kbd
      className={cn("inline-flex h-5 min-w-5 items-center justify-center rounded border border-line-strong bg-surface-2 px-1 font-mono text-[11px] text-text-2", className)}
      {...rest}
    />
  );
}
