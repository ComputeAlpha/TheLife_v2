import { useMemo, useState, type FormEvent } from "react";
import { Check, Globe2 } from "lucide-react";
import { useUpdateProfile, useUser, type ProfilePatch } from "@/api/auth";
import { ApiError } from "@/api/errors";
import { Button } from "@/ui/Button";
import { Card, CardHeader } from "@/ui/Card";
import { Field, Input, NativeSelect } from "@/ui/Field";
import { toast } from "@/ui/toast";
import { cn } from "@/ui/cn";
import { fmt, timezones } from "@/lib/time";
import { useNow } from "@/lib/useNow";

const DAYS = [
  ["MO", "Mon"], ["TU", "Tue"], ["WE", "Wed"], ["TH", "Thu"], ["FR", "Fri"], ["SA", "Sat"], ["SU", "Sun"],
] as const;
type Day = (typeof DAYS)[number][0];

export function ProfileSettings() {
  const user = useUser();
  const update = useUpdateProfile();
  const now = useNow();
  const [name, setName] = useState(user.display_name);
  const [tz, setTz] = useState(user.timezone);
  const [dayStart, setDayStart] = useState(user.day_start_hour);
  const [workdays, setWorkdays] = useState<Day[]>((user.workdays ?? ["MO", "TU", "WE", "TH", "FR"]) as Day[]);
  const zones = useMemo(() => {
    const all = timezones();
    return all.includes(user.timezone) ? all : [user.timezone, ...all];
  }, [user.timezone]);

  const patch: ProfilePatch = {};
  if (name.trim() !== user.display_name) patch.display_name = name.trim();
  if (tz !== user.timezone) patch.timezone = tz;
  if (dayStart !== user.day_start_hour) patch.day_start_hour = dayStart;
  const sortedDays = DAYS.map(([d]) => d).filter((d) => workdays.includes(d));
  if (sortedDays.join() !== (user.workdays ?? []).join()) patch.workdays = sortedDays;
  const dirty = Object.keys(patch).length > 0;
  const errors = update.error instanceof ApiError ? update.error.fieldErrors() : {};

  const submit = (e: FormEvent) => {
    e.preventDefault();
    if (!dirty) return;
    update.mutate(patch, {
      onSuccess: () => toast.success("Profile saved"),
      onError: (err) => toast.error("Couldn't save", err instanceof ApiError ? err.summary : undefined),
    });
  };

  return (
    <form onSubmit={submit} className="space-y-6">
      <Card>
        <CardHeader title="About you" subtitle={user.email} />
        <div className="grid gap-5 px-5 pb-5 sm:grid-cols-2">
          <Field label="Display name" error={errors.display_name}>
            {(p) => <Input {...p} value={name} maxLength={200} onChange={(e) => setName(e.target.value)} required />}
          </Field>
        </div>
      </Card>

      <Card>
        <CardHeader title="Time" subtitle="Every date and time is shown in this timezone." icon={<Globe2 className="size-4" />} />
        <div className="grid gap-5 px-5 pb-5 sm:grid-cols-2">
          <Field label="Timezone" error={errors.timezone} hint={`Now: ${fmt(now, tz, "EEE d MMM, HH:mm")}`}>
            {(p) => (
              <NativeSelect {...p} value={tz} onChange={(e) => setTz(e.target.value)}>
                {zones.map((z) => (
                  <option key={z} value={z}>{z.replaceAll("_", " ")}</option>
                ))}
              </NativeSelect>
            )}
          </Field>
          <Field
            label="Day starts at"
            error={errors.day_start_hour}
            hint="Late nights before this hour still count toward the previous day."
          >
            {(p) => (
              <NativeSelect {...p} value={dayStart} onChange={(e) => setDayStart(Number(e.target.value))}>
                {Array.from({ length: 13 }, (_, h) => (
                  <option key={h} value={h}>{`${String(h).padStart(2, "0")}:00`}</option>
                ))}
              </NativeSelect>
            )}
          </Field>
          <fieldset className="sm:col-span-2">
            <legend className="mb-2 text-[13px] font-medium text-text-2">Workdays</legend>
            <div className="flex flex-wrap gap-2">
              {DAYS.map(([d, label]) => {
                const on = workdays.includes(d);
                return (
                  <button
                    key={d}
                    type="button"
                    aria-pressed={on}
                    onClick={() => setWorkdays((w) => (on ? w.filter((x) => x !== d) : [...w, d]))}
                    className={cn(
                      "inline-flex h-9 min-w-16 items-center justify-center gap-1.5 rounded-full border px-3 text-sm transition-all",
                      on
                        ? "border-accent/60 bg-accent/12 text-text shadow-[0_0_14px_-4px_rgb(34_211_238/.6)]"
                        : "border-line bg-surface-2 text-text-3 hover:text-text-2",
                    )}
                  >
                    {on ? <Check className="size-3.5 text-accent" aria-hidden /> : null}
                    {label}
                  </button>
                );
              })}
            </div>
            <p className="mt-2 text-[12.5px] text-text-3">Used for the work-week calendar view and workday scoring.</p>
          </fieldset>
        </div>
      </Card>

      <div className="flex justify-end gap-2">
        <Button
          variant="ghost"
          disabled={!dirty || update.isPending}
          onClick={() => {
            setName(user.display_name);
            setTz(user.timezone);
            setDayStart(user.day_start_hour);
            setWorkdays((user.workdays ?? []) as Day[]);
          }}
        >
          Discard
        </Button>
        <Button type="submit" variant="primary" disabled={!dirty} loading={update.isPending}>
          Save changes
        </Button>
      </div>
    </form>
  );
}
