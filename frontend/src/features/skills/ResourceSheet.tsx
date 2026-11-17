import { useState } from "react";
import type { SkillResource } from "@/api/types";
import { useDeleteResource, useSaveResource } from "@/api/skills";
import { useGoals } from "@/api/plan";
import { Button } from "@/ui/Button";
import { Sheet } from "@/ui/Dialog";
import { Field, Input, NativeSelect } from "@/ui/Field";
import { Segmented } from "@/ui/Segmented";
import { toast } from "@/ui/toast";
import { DeleteButton, errText, fieldErr } from "../plan/shared";

export const RESOURCE_STATUS = ["planned", "active", "paused", "done", "dropped"] as const;

export function ResourceSheet({ open, existing, onClose, onDeleted }: { open: boolean; existing?: SkillResource; onClose: () => void; onDeleted?: () => void }) {
  const r = existing;
  const goals = useGoals();
  const [kind, setKind] = useState<"book" | "course">(r?.kind ?? "book");
  const [title, setTitle] = useState(r?.title ?? "");
  const [author, setAuthor] = useState(r?.author ?? "");
  const [url, setUrl] = useState(r?.url ?? "");
  const [pages, setPages] = useState(r?.total_pages ? String(r.total_pages) : "");
  const [hours, setHours] = useState(r?.total_duration_s ? String(Math.round((r.total_duration_s / 3600) * 10) / 10) : "");
  const [goalId, setGoalId] = useState(r?.goal_id ?? "");
  const [status, setStatus] = useState<(typeof RESOURCE_STATUS)[number]>(r?.status ?? "active");
  const save = useSaveResource();
  const del = useDeleteResource();
  const valid = !!title.trim();
  return (
    <Sheet
      open={open}
      onOpenChange={(o) => !o && onClose()}
      title={r ? "Edit resource" : "New book or course"}
      description="Something you're learning from. Sessions you log move its progress."
      footer={
        <>
          {r ? <DeleteButton loading={del.isPending} onDelete={() => del.mutate(r, { onSuccess: () => { toast("Deleted"); onClose(); onDeleted?.(); }, onError: (e) => toast.error(errText(e, "Couldn't delete")) })} /> : null}
          <Button variant="ghost" onClick={onClose}>Cancel</Button>
          <Button type="submit" form="resource-form" variant="primary" disabled={!valid} loading={save.isPending}>{r ? "Save" : "Add"}</Button>
        </>
      }
    >
      <form
        id="resource-form"
        className="space-y-4"
        onSubmit={(e) => {
          e.preventDefault();
          if (!valid) return;
          save.mutate(
            {
              existing: r,
              body: {
                kind, title: title.trim(), author: author.trim() || null, url: url.trim() || null, status, goal_id: goalId || null,
                total_pages: kind === "book" && pages ? Math.max(1, Math.round(Number(pages))) : null,
                total_duration_s: kind === "course" && hours ? Math.round(Number(hours) * 3600) : null,
              },
            },
            { onSuccess: () => { toast.success(r ? "Saved" : "Added to your library"); onClose(); }, onError: (err) => toast.error(errText(err)) },
          );
        }}
      >
        {!r ? (
          <div>
            <p className="mb-2 text-[13px] font-medium text-text-2">Kind</p>
            <Segmented label="Kind" value={kind} onChange={setKind} options={[{ value: "book", label: "Book" }, { value: "course", label: "Course" }]} />
          </div>
        ) : null}
        <Field label="Title" error={fieldErr(save.error, "title")}>
          {(f) => <Input {...f} autoFocus required maxLength={200} value={title} onChange={(e) => setTitle(e.target.value)} placeholder={kind === "book" ? "Designing Data-Intensive Applications" : "C++ Concurrency in Action"} />}
        </Field>
        <Field label={kind === "book" ? "Author" : "Instructor or provider"}>{(f) => <Input {...f} value={author} maxLength={200} onChange={(e) => setAuthor(e.target.value)} />}</Field>
        {kind === "book" ? (
          <Field label="Pages" error={fieldErr(save.error, "total_pages")}>{(f) => <Input {...f} type="number" min={1} value={pages} onChange={(e) => setPages(e.target.value)} />}</Field>
        ) : (
          <Field label="Length (hours)" hint="Or add sections with their durations.">{(f) => <Input {...f} type="number" min={0} step="0.5" value={hours} onChange={(e) => setHours(e.target.value)} />}</Field>
        )}
        <Field label="Link">{(f) => <Input {...f} type="url" value={url} onChange={(e) => setUrl(e.target.value)} placeholder="https://" />}</Field>
        <div className="grid grid-cols-2 gap-3">
          <Field label="Status">
            {(f) => (
              <NativeSelect {...f} value={status} onChange={(e) => setStatus(e.target.value as typeof status)}>
                {RESOURCE_STATUS.map((s) => <option key={s} value={s} className="capitalize">{s[0]!.toUpperCase() + s.slice(1)}</option>)}
              </NativeSelect>
            )}
          </Field>
          <Field label="For goal">
            {(f) => (
              <NativeSelect {...f} value={goalId} onChange={(e) => setGoalId(e.target.value)}>
                <option value="">None</option>
                {(goals.data ?? []).filter((g) => g.status !== "abandoned").map((g) => <option key={g.id} value={g.id}>{g.title}</option>)}
              </NativeSelect>
            )}
          </Field>
        </div>
      </form>
    </Sheet>
  );
}
