import { cn } from "./cn";

/** An on/off switch (role="switch"). */
export function Switch({ on, onChange, label, disabled }: { on: boolean; onChange: (v: boolean) => void; label: string; disabled?: boolean }) {
  return (
    <button type="button" role="switch" aria-checked={on} aria-label={label} disabled={disabled} onClick={() => onChange(!on)}
      className={cn("relative h-6 w-11 shrink-0 rounded-full border transition-colors disabled:opacity-50", on ? "border-accent/60 bg-accent/30" : "border-line-strong bg-surface-3")}>
      <span className={cn("absolute top-0.5 size-[18px] rounded-full transition-[left,background-color]", on ? "left-[22px] bg-accent" : "left-0.5 bg-text-3")} />
    </button>
  );
}
