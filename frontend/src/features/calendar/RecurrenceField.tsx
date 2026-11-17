import { Repeat } from "lucide-react";
import { Field, Input, NativeSelect } from "@/ui/Field";
import { describeRule, presetOf, presetRule, ruleLooksValid, type Preset } from "@/lib/rrule";
import type { Day } from "@/lib/calendar/days";
import { fmtDay } from "@/lib/calendar/days";
import { useState } from "react";

export function RecurrenceField({ value, onChange, day, error }: { value: string | null; onChange: (r: string | null) => void; day: Day; error?: string }) {
  const [preset, setPreset] = useState<Preset>(() => presetOf(value, day));
  const text = describeRule(value);
  const labels: Record<Preset, string> = {
    none: "Does not repeat",
    daily: "Every day",
    weekdays: "Every weekday (Mon–Fri)",
    weekly: `Every week on ${fmtDay(day, "EEEE")}`,
    monthly: `Every month on day ${Number(day.slice(8))}`,
    yearly: `Every year on ${fmtDay(day, "d MMMM")}`,
    custom: "Custom rule…",
  };
  return (
    <div className="space-y-2">
      <Field label="Repeat" error={error}>
        {(p) => (
          <NativeSelect
            {...p}
            value={preset}
            onChange={(e) => {
              const next = e.target.value as Preset;
              setPreset(next);
              if (next !== "custom") onChange(presetRule(next, day));
            }}
          >
            {(Object.keys(labels) as Preset[]).map((k) => (
              <option key={k} value={k}>{labels[k]}</option>
            ))}
          </NativeSelect>
        )}
      </Field>
      {preset === "custom" ? (
        <Field label="RRULE" hint="RFC 5545, for example FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,TH" error={value && !ruleLooksValid(value) ? "Not a recognised rule." : undefined}>
          {(p) => <Input {...p} className="font-mono" value={value ?? ""} placeholder="FREQ=WEEKLY;BYDAY=MO" onChange={(e) => onChange(e.target.value.trim() || null)} />}
        </Field>
      ) : null}
      {text && preset !== "none" ? (
        <p className="flex items-center gap-1.5 text-[12.5px] text-accent"><Repeat className="size-3.5" aria-hidden /> Repeats {text}</p>
      ) : null}
    </div>
  );
}
