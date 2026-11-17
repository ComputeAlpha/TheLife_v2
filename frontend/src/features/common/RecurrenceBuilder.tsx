import { useState } from "react";
import { Repeat } from "lucide-react";
import { buildRule, nextDays, parseRule, type Freq, type RuleParts, type Weekday } from "@/lib/recurrence";
import { describeRule, ruleLooksValid } from "@/lib/rrule";
import { fmtDay, WEEKDAY_CODES, weekdayCode, type Day } from "@/lib/calendar/days";
import { Input, NativeSelect } from "@/ui/Field";
import { Chip } from "@/ui/Segmented";

const UNIT: Record<Freq, [string, string]> = { DAILY: ["day", "days"], WEEKLY: ["week", "weeks"], MONTHLY: ["month", "months"], YEARLY: ["year", "years"] };
const WD_LABEL: Record<Weekday, string> = { MO: "Mon", TU: "Tue", WE: "Wed", TH: "Thu", FR: "Fri", SA: "Sat", SU: "Sun" };

/** Frequency, interval, weekdays and end, with the next five dates (frontend_lld.md §6.9). */
export function RecurrenceBuilder({ value, onChange, startDay, today }: { value: string | null; onChange: (r: string | null) => void; startDay: Day; today: Day }) {
  const parsed = parseRule(value);
  const [raw, setRaw] = useState(!!value && !parsed);
  const parts: RuleParts = parsed ?? { freq: "WEEKLY", interval: 1, byday: [weekdayCode(startDay)], end: { kind: "never" } };
  const set = (p: Partial<RuleParts>) => onChange(buildRule({ ...parts, ...p }));
  const preview = value && ruleLooksValid(value) ? nextDays(value, startDay, today > startDay ? today : startDay) : [];

  return (
    <fieldset className="space-y-3 rounded-xl border border-line p-4">
      <legend className="px-1 text-[13px] font-medium text-text-2">Repeat</legend>
      <label className="flex cursor-pointer items-center gap-2.5 text-sm text-text">
        <input type="checkbox" className="size-4 accent-[var(--color-accent)]" checked={!!value}
          onChange={(e) => { setRaw(false); onChange(e.target.checked ? buildRule(parts) : null); }} />
        <Repeat className="size-4 text-accent" aria-hidden /> Repeats
      </label>
      {value && raw ? (
        <div className="space-y-1.5">
          <Input aria-label="Rule (RFC 5545)" className="font-mono text-[13px]" value={value} onChange={(e) => onChange(e.target.value.toUpperCase())} />
          <p className="text-[12.5px] text-text-3">{describeRule(value) ?? "Not a rule the server accepts yet."} <button type="button" className="text-accent hover:underline" onClick={() => { setRaw(false); onChange(buildRule(parts)); }}>Use the builder</button></p>
        </div>
      ) : value ? (
        <>
          <div className="flex flex-wrap items-center gap-2 text-sm text-text-2">
            Every
            <Input aria-label="Interval" type="number" min={1} max={99} className="h-9 w-16" value={parts.interval}
              onChange={(e) => set({ interval: Math.max(1, Math.min(99, Number(e.target.value) || 1)) })} />
            <NativeSelect aria-label="Frequency" className="h-9 w-auto" value={parts.freq}
              onChange={(e) => { const f = e.target.value as Freq; set({ freq: f, byday: f === "WEEKLY" ? parts.byday.length ? parts.byday : [weekdayCode(startDay)] : [] }); }}>
              {(Object.keys(UNIT) as Freq[]).map((f) => <option key={f} value={f}>{UNIT[f][parts.interval > 1 ? 1 : 0]}</option>)}
            </NativeSelect>
            {parts.freq === "MONTHLY" ? <span>on day {Number(startDay.slice(8))}</span> : parts.freq === "YEARLY" ? <span>on {fmtDay(startDay, "d MMMM")}</span> : null}
          </div>
          {parts.freq === "WEEKLY" ? (
            <div className="flex flex-wrap gap-1.5" role="group" aria-label="On these days">
              {WEEKDAY_CODES.map((d) => (
                <Chip key={d} on={parts.byday.includes(d)} onClick={() => {
                  const next = parts.byday.includes(d) ? parts.byday.filter((x) => x !== d) : [...parts.byday, d];
                  set({ byday: next.length ? next : [d] });
                }}>{WD_LABEL[d]}</Chip>
              ))}
            </div>
          ) : null}
          <div className="flex flex-wrap items-center gap-2 text-sm text-text-2">
            Ends
            <NativeSelect aria-label="Ends" className="h-9 w-auto" value={parts.end.kind}
              onChange={(e) => {
                const k = e.target.value;
                set({ end: k === "until" ? { kind: "until", day: today } : k === "count" ? { kind: "count", n: 10 } : { kind: "never" } });
              }}>
              <option value="never">never</option>
              <option value="until">on a date</option>
              <option value="count">after a number of times</option>
            </NativeSelect>
            {parts.end.kind === "until" ? (
              <Input aria-label="End date" type="date" className="h-9 w-auto" value={parts.end.day} onChange={(e) => e.target.value && set({ end: { kind: "until", day: e.target.value } })} />
            ) : parts.end.kind === "count" ? (
              <><Input aria-label="Times" type="number" min={1} className="h-9 w-20" value={parts.end.n} onChange={(e) => set({ end: { kind: "count", n: Math.max(1, Number(e.target.value) || 1) } })} /> times</>
            ) : null}
          </div>
          <div>
            <p className="text-[12.5px] text-text-3">
              {describeRule(value) ? <span className="first-letter:uppercase">{describeRule(value)}</span> : null}
              {" · "}<button type="button" className="text-accent hover:underline" onClick={() => setRaw(true)}>Edit as text</button>
            </p>
            {preview.length ? (
              <ol className="mt-2 flex flex-wrap gap-1.5" aria-label="Next dates">
                {preview.map((d) => <li key={d} className="rounded-md border border-line bg-surface-2/60 px-2 py-0.5 text-[12px] text-text-2">{fmtDay(d, "EEE d MMM")}</li>)}
              </ol>
            ) : <p className="mt-1 text-[12.5px] text-text-3">No more dates.</p>}
          </div>
        </>
      ) : null}
    </fieldset>
  );
}
