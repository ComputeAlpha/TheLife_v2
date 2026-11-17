import { AREAS, type Area } from "@/api/types";
import { AREA_META } from "@/lib/areas";
import { cn } from "./cn";

/** Area as a row of coloured, icon-led chips (a radio group). */
export function AreaPicker({ value, onChange, label = "Area" }: { value: Area | null; onChange: (a: Area) => void; label?: string }) {
  return (
    <fieldset>
      <legend className="mb-2 text-[13px] font-medium text-text-2">{label}</legend>
      <div className="flex flex-wrap gap-1.5" role="radiogroup">
        {AREAS.map((a) => {
          const m = AREA_META[a];
          const on = value === a;
          return (
            <button
              key={a}
              type="button"
              role="radio"
              aria-checked={on}
              onClick={() => onChange(a)}
              style={{ "--a": m.color } as React.CSSProperties}
              className={cn(
                "inline-flex h-8 items-center gap-1.5 rounded-full border px-3 text-[12.5px] transition-all",
                on
                  ? "border-[var(--a)] bg-[color-mix(in_oklab,var(--a)_16%,transparent)] text-text shadow-[0_0_12px_-4px_var(--a)]"
                  : "border-line text-text-2 hover:border-line-strong hover:text-text",
              )}
            >
              <m.icon className="size-3.5 text-[var(--a)]" aria-hidden />
              {m.label}
            </button>
          );
        })}
      </div>
    </fieldset>
  );
}
