import { useMemo, useState } from "react";
import { Check, Clapperboard, Film, Gamepad2, Heart, Plus, Tv } from "lucide-react";
import { useUser } from "@/api/auth";
import { useDeleteItem, useDeleteSlot, useItems, useSaveItem, useSaveSlot, useSlots } from "@/api/entertainment";
import type { EntertainmentItem, EntertainmentSlot } from "@/api/types";
import { addDays, dayOf, dayStartMs, fmtDay } from "@/lib/calendar/days";
import { fmt, fromLocalInput, localDay, minutesLabel, toLocalInput } from "@/lib/time";
import { useNow } from "@/lib/useNow";
import { Badge } from "@/ui/Badge";
import { Button } from "@/ui/Button";
import { Card, CardHeader } from "@/ui/Card";
import { NativeSelect } from "@/ui/Field";
import { Chip, Segmented } from "@/ui/Segmented";
import { EmptyState, ErrorState, Skeleton } from "@/ui/States";
import { toast } from "@/ui/toast";
import { cn } from "@/ui/cn";
import { FormSheet, opt } from "../common/FormSheet";
import { OriginBadge } from "../common/OriginBadge";
import { Page } from "../common/Page";
import { errText } from "../plan/shared";

type Kind = EntertainmentItem["kind"];
type Status = NonNullable<EntertainmentItem["status"]>;
const KIND: Record<Kind, { label: string; plural: string; icon: typeof Film }> = {
  game: { label: "Game", plural: "Games", icon: Gamepad2 },
  movie: { label: "Movie", plural: "Movies", icon: Film },
  series: { label: "Series", plural: "Series", icon: Tv },
};
const STATUS: { value: Status; label: string }[] = [
  { value: "wishlist", label: "Wishlist" }, { value: "playing", label: "Playing" }, { value: "watching", label: "Watching" },
  { value: "paused", label: "Paused" }, { value: "finished", label: "Finished" }, { value: "dropped", label: "Dropped" },
];
const TONE: Record<Status, "neutral" | "accent" | "good" | "warn"> = { wishlist: "neutral", playing: "accent", watching: "accent", paused: "warn", finished: "good", dropped: "neutral" };

export function EntertainmentPage() {
  const user = useUser();
  const tz = user.timezone;
  const now = useNow();
  const today = localDay(now, tz, user.day_start_hour);
  const fromIso = new Date(dayStartMs(today, tz)).toISOString();
  const toIso = new Date(dayStartMs(addDays(today, 14), tz)).toISOString();
  const items = useItems();
  const slots = useSlots(fromIso, toIso);
  const [kind, setKind] = useState<Kind | "all">("all");
  const [statuses, setStatuses] = useState<Set<Status>>(new Set());
  const [itemEdit, setItemEdit] = useState<EntertainmentItem | "new" | null>(null);
  const [slotEdit, setSlotEdit] = useState<{ existing?: EntertainmentSlot; itemId?: string } | null>(null);
  const saveItem = useSaveItem();
  const saveSlot = useSaveSlot();
  const byId = useMemo(() => new Map((items.data ?? []).map((i) => [i.id, i])), [items.data]);
  const shown = (items.data ?? [])
    .filter((i) => (kind === "all" || i.kind === kind) && (!statuses.size || statuses.has(i.status ?? "wishlist")))
    .sort((a, b) => STATUS.findIndex((s) => s.value === a.status) - STATUS.findIndex((s) => s.value === b.status) || a.title.localeCompare(b.title));
  const slotDays = useMemo(() => {
    const m = new Map<string, EntertainmentSlot[]>();
    for (const s of [...(slots.data ?? [])].sort((a, b) => a.starts_at.localeCompare(b.starts_at))) {
      const d = dayOf(Date.parse(s.starts_at), tz);
      (m.get(d) ?? m.set(d, []).get(d)!).push(s);
    }
    return [...m.entries()];
  }, [slots.data, tz]);

  return (
    <Page
      wide
      title="Entertainment"
      subtitle="Games, movies and series, planned into your week."
      actions={
        <>
          <Button icon={<Clapperboard className="size-4" />} onClick={() => setSlotEdit({})}>Plan time</Button>
          <Button variant="primary" icon={<Plus className="size-4" />} onClick={() => setItemEdit("new")}>Add title</Button>
        </>
      }
    >
      <div className="mb-5 flex items-start gap-3 rounded-2xl border border-[color-mix(in_oklab,var(--color-area-entertainment)_35%,transparent)] bg-[color-mix(in_oklab,var(--color-area-entertainment)_6%,transparent)] p-4 text-[13.5px] text-text-2">
        <Heart className="mt-0.5 size-4 shrink-0 text-[var(--color-area-entertainment)]" aria-hidden />
        <p>Entertainment is <span className="text-text">planned, never limited</span>. It doesn't cost you score points on its own; the assistant only points out when it pushed planned work aside.</p>
      </div>

      <div className="grid gap-5 xl:grid-cols-[minmax(0,5fr)_minmax(0,7fr)]">
        <Card>
          <CardHeader title="Next two weeks" icon={<Clapperboard className="size-4" />} subtitle="Planned slots" />
          {slots.isPending ? <div className="px-5 pb-5"><Skeleton className="h-24" /></div> : slots.error ? <ErrorState error={slots.error} retry={() => slots.refetch()} /> : !slotDays.length ? (
            <EmptyState className="py-10" title="Nothing planned" body="Plan a gaming night or a movie. Planned fun is part of a good week." action={<Button size="sm" onClick={() => setSlotEdit({})}>Plan time</Button>} />
          ) : (
            <div className="space-y-4 px-5 pb-5">
              {slotDays.map(([d, list]) => (
                <section key={d} aria-label={fmtDay(d, "EEEE d MMMM")}>
                  <p className="mb-1.5 text-[11px] font-semibold uppercase tracking-wider text-text-3">{d === today ? "Today" : d === addDays(today, 1) ? "Tomorrow" : fmtDay(d, "EEE d MMM")}</p>
                  <ul className="space-y-1.5">
                    {list.map((s) => {
                      const it = s.item_id ? byId.get(s.item_id) : undefined;
                      const K = it ? KIND[it.kind].icon : Clapperboard;
                      const mins = Math.round((Date.parse(s.ends_at) - Date.parse(s.starts_at)) / 60_000);
                      return (
                        <li key={s.id} className="flex items-center gap-3 rounded-xl border border-line p-2.5">
                          <K className="size-4 shrink-0 text-[var(--color-area-entertainment)]" aria-hidden />
                          <button onClick={() => setSlotEdit({ existing: s })} className="min-w-0 flex-1 text-left">
                            <span className={cn("block truncate text-sm", s.consumed ? "text-text-3" : "text-text")}>{it?.title ?? s.notes ?? "Free time"}</span>
                            <span className="font-mono text-[12px] text-text-3">{fmt(s.starts_at, tz, "HH:mm")}–{fmt(s.ends_at, tz, "HH:mm")} · {minutesLabel(mins)}</span>
                          </button>
                          <button
                            onClick={() => saveSlot.mutate({ existing: s, body: { consumed: !s.consumed } }, { onError: (e) => toast.error(errText(e)) })}
                            aria-pressed={!!s.consumed}
                            className={cn("flex h-7 items-center gap-1 rounded-full border px-2.5 text-[12px]", s.consumed ? "border-good/40 bg-good/12 text-good" : "border-line text-text-2 hover:text-text")}
                          >
                            {s.consumed ? <Check className="size-3.5" aria-hidden /> : null}{s.consumed ? "Enjoyed" : "Mark enjoyed"}
                          </button>
                        </li>
                      );
                    })}
                  </ul>
                </section>
              ))}
            </div>
          )}
        </Card>

        <div>
          <div className="mb-4 flex flex-wrap items-center gap-3">
            <Segmented<Kind | "all"> label="Kind" value={kind} onChange={setKind}
              options={[{ value: "all", label: "All" }, ...(Object.keys(KIND) as Kind[]).map((k) => ({ value: k, label: KIND[k].plural }))]} />
            <div className="flex flex-wrap gap-1.5" role="group" aria-label="Status">
              {STATUS.map((s) => (
                <Chip key={s.value} on={statuses.has(s.value)} onClick={() => setStatuses((x) => { const n = new Set(x); if (n.has(s.value)) n.delete(s.value); else n.add(s.value); return n; })}>{s.label}</Chip>
              ))}
            </div>
          </div>
          {items.isPending ? <Skeleton className="h-48" /> : items.error ? <ErrorState error={items.error} retry={() => items.refetch()} /> : !shown.length ? (
            <Card><EmptyState art={<Gamepad2 className="size-10 text-[var(--color-area-entertainment)]" />} title={items.data?.length ? "Nothing matches" : "Nothing on the list yet"} body="Add what you're playing or want to watch." action={<Button variant="primary" onClick={() => setItemEdit("new")}>Add title</Button>} /></Card>
          ) : (
            <ul className="grid gap-3 sm:grid-cols-2">
              {shown.map((i) => {
                const K = KIND[i.kind];
                const st = i.status ?? "wishlist";
                return (
                  <li key={i.id}>
                    <Card className="flex h-full items-center gap-3 p-4">
                      <span className="grid size-10 shrink-0 place-items-center rounded-xl bg-[color-mix(in_oklab,var(--color-area-entertainment)_14%,transparent)] text-[var(--color-area-entertainment)]"><K.icon className="size-5" aria-hidden /></span>
                      <div className="min-w-0 flex-1">
                        <button onClick={() => setItemEdit(i)} className="block max-w-full truncate text-left text-sm font-medium text-text hover:text-accent">{i.title}</button>
                        <div className="mt-1 flex items-center gap-2">
                          <span className="text-[12px] text-text-3">{K.label}</span>
                          <Badge tone={TONE[st]}>{STATUS.find((s) => s.value === st)!.label}</Badge>
                          <OriginBadge row={i} />
                        </div>
                      </div>
                      <NativeSelect aria-label={`Status of ${i.title}`} className="h-8 w-auto text-[12.5px]" value={st}
                        onChange={(e) => saveItem.mutate({ existing: i, body: { kind: i.kind, title: i.title, status: e.target.value as Status } }, { onError: (err) => toast.error(errText(err)) })}>
                        {STATUS.map((s) => <option key={s.value} value={s.value}>{s.label}</option>)}
                      </NativeSelect>
                    </Card>
                  </li>
                );
              })}
            </ul>
          )}
        </div>
      </div>
      {itemEdit ? <ItemSheet existing={itemEdit === "new" ? undefined : itemEdit} onClose={() => setItemEdit(null)} onPlan={(id) => { setItemEdit(null); setSlotEdit({ itemId: id }); }} /> : null}
      {slotEdit ? <SlotSheet existing={slotEdit.existing} itemId={slotEdit.itemId} items={items.data ?? []} tz={tz} now={now} onClose={() => setSlotEdit(null)} /> : null}
    </Page>
  );
}

function ItemSheet({ existing, onClose, onPlan }: { existing?: EntertainmentItem; onClose: () => void; onPlan: (id: string) => void }) {
  const save = useSaveItem();
  const del = useDeleteItem();
  return (
    <FormSheet
      id="item-form"
      title={existing ? "Edit title" : "Add a game, movie or series"}
      submitLabel={existing ? "Save" : "Add"}
      onClose={onClose}
      initial={{ kind: existing?.kind ?? "game", title: existing?.title ?? "", status: existing?.status ?? "wishlist" }}
      fields={[
        { key: "kind", label: "Kind", type: "segmented", options: (Object.keys(KIND) as Kind[]).map((k) => ({ value: k, label: KIND[k].label })) },
        { key: "title", label: "Title", type: "text", required: true, maxLength: 200, placeholder: "Hades II" },
        { key: "status", label: "Status", type: "select", options: STATUS },
      ]}
      onSubmit={(v) => save.mutateAsync({ existing, body: { kind: v.kind as Kind, title: String(v.title).trim(), status: v.status as Status } })}
      onDelete={existing ? () => del.mutateAsync(existing) : undefined}
    >
      {existing ? <Button type="button" variant="ghost" icon={<Clapperboard className="size-4" />} onClick={() => onPlan(existing.id)}>Plan time for it</Button> : null}
    </FormSheet>
  );
}

function SlotSheet({ existing, itemId, items, tz, now, onClose }: { existing?: EntertainmentSlot; itemId?: string; items: EntertainmentItem[]; tz: string; now: number; onClose: () => void }) {
  const save = useSaveSlot();
  const del = useDeleteSlot();
  const evening = dayStartMs(dayOf(now, tz), tz) + 20 * 3_600_000;
  const start = existing?.starts_at ?? new Date(evening > now ? evening : evening + 86_400_000).toISOString();
  return (
    <FormSheet
      id="slot-form"
      title={existing ? "Edit planned time" : "Plan entertainment time"}
      description="It shows on your calendar like any other block."
      submitLabel={existing ? "Save" : "Plan it"}
      onClose={onClose}
      initial={{ item_id: existing?.item_id ?? itemId ?? "", starts_at: toLocalInput(start, tz), ends_at: toLocalInput(existing?.ends_at ?? Date.parse(start) + 2 * 3_600_000, tz), consumed: existing?.consumed ?? false, notes: existing?.notes ?? "" }}
      validate={(v): Record<string, string> => {
        const s = fromLocalInput(String(v.starts_at), tz), e = fromLocalInput(String(v.ends_at), tz);
        return !s || !e || Date.parse(e) <= Date.parse(s) ? { ends_at: "Ends before it starts" } : {};
      }}
      fields={[
        { key: "item_id", label: "What", type: "select", options: [{ value: "", label: "Anything (free time)" }, ...items.filter((i) => i.status !== "finished" && i.status !== "dropped" || i.id === existing?.item_id).map((i) => ({ value: i.id, label: `${KIND[i.kind].label}: ${i.title}` }))] },
        { key: "starts_at", label: "From", type: "datetime" },
        { key: "ends_at", label: "To", type: "datetime" },
        { key: "notes", label: "Notes", type: "text", maxLength: 500 },
        ...(existing ? [{ key: "consumed", label: "Enjoyed it", type: "checkbox" as const }] : []),
      ]}
      onSubmit={(v) => save.mutateAsync({ existing, body: { item_id: opt(v.item_id), starts_at: fromLocalInput(String(v.starts_at), tz)!, ends_at: fromLocalInput(String(v.ends_at), tz)!, notes: opt(v.notes), ...(existing ? { consumed: !!v.consumed } : {}) } })}
      onDelete={existing ? () => del.mutateAsync(existing) : undefined}
    />
  );
}
