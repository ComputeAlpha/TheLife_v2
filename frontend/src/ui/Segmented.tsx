import type { ReactNode } from "react";
import { cn } from "./cn";

/** A segmented tab bar (role="tablist"). */
export function Segmented<T extends string>({ value, onChange, options, label, className }: {
  value: T;
  onChange: (v: T) => void;
  options: { value: T; label: ReactNode }[];
  label: string;
  className?: string;
}) {
  return (
    <div className={cn("flex gap-1 rounded-xl border border-line p-1 sm:w-fit", className)} role="tablist" aria-label={label}>
      {options.map((o) => (
        <button
          key={o.value}
          type="button"
          role="tab"
          aria-selected={value === o.value}
          onClick={() => onChange(o.value)}
          className={cn(
            "flex-1 whitespace-nowrap rounded-lg px-4 py-1.5 text-sm transition-colors sm:flex-none",
            value === o.value ? "bg-accent/12 text-text shadow-[inset_0_0_0_1px_rgb(34_211_238/.4)]" : "text-text-2 hover:text-text",
          )}
        >
          {o.label}
        </button>
      ))}
    </div>
  );
}

/** A toggle chip for filters (aria-pressed). */
export function Chip({ on, onClick, children, color, className }: {
  on: boolean;
  onClick: () => void;
  children: ReactNode;
  color?: string;
  className?: string;
}) {
  return (
    <button
      type="button"
      aria-pressed={on}
      onClick={onClick}
      style={color ? ({ "--c": color } as React.CSSProperties) : undefined}
      className={cn(
        "inline-flex h-7 items-center gap-1.5 rounded-full border px-2.5 text-[12.5px] transition-colors",
        on
          ? color
            ? "border-[color-mix(in_oklab,var(--c)_55%,transparent)] bg-[color-mix(in_oklab,var(--c)_14%,transparent)] text-text"
            : "border-accent/50 bg-accent/12 text-text"
          : "border-line text-text-2 hover:border-line-strong hover:text-text",
        className,
      )}
    >
      {children}
    </button>
  );
}
