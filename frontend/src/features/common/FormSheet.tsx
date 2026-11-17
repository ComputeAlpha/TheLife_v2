import { useState, type ReactNode } from "react";
import type { Area } from "@/api/types";
import { ApiError } from "@/api/errors";
import { AreaPicker } from "@/ui/AreaPicker";
import { Button } from "@/ui/Button";
import { Sheet } from "@/ui/Dialog";
import { Field, Input, NativeSelect, Textarea } from "@/ui/Field";
import { Chip, Segmented } from "@/ui/Segmented";
import { toast } from "@/ui/toast";
import { DeleteButton, errText } from "../plan/shared";

export type FieldSpec =
  | { key: string; label: string; type: "text" | "textarea" | "date" | "datetime" | "time" | "url"; required?: boolean; hint?: string; placeholder?: string; maxLength?: number }
  | { key: string; label: string; type: "number"; required?: boolean; hint?: string; min?: number; max?: number; step?: number }
  | { key: string; label: string; type: "select"; options: { value: string; label: string }[]; hint?: string }
  | { key: string; label: string; type: "segmented"; options: { value: string; label: string }[] }
  | { key: string; label: string; type: "chips"; options: { value: string; label: string }[]; hint?: string }
  | { key: string; label: string; type: "checkbox"; hint?: string }
  | { key: string; label: string; type: "area" };

export type Values = Record<string, string | boolean | string[]>;

/**
 * A declarative editor sheet for the simpler resources (frontend_lld.md §6.11–§6.14). Values are strings, booleans
 * or string lists; the caller converts them to the request body.
 */
export function FormSheet({ title, description, fields, initial, onSubmit, onDelete, submitLabel, onClose, validate, children, id, onChange }: {
  title: string;
  description?: ReactNode;
  fields: FieldSpec[];
  initial: Values;
  onSubmit: (v: Values) => Promise<unknown>;
  onDelete?: () => Promise<unknown>;
  submitLabel: string;
  onClose: () => void;
  validate?: (v: Values) => Record<string, string>;
  children?: ReactNode;
  id: string;
  /** Told about every change, for state outside the field list (e.g. a recurrence anchored on a date field). */
  onChange?: (v: Values) => void;
}) {
  const [v, setV] = useState<Values>(initial);
  const [busy, setBusy] = useState<"save" | "delete" | null>(null);
  const [serverErrors, setServerErrors] = useState<Record<string, string>>({});
  const set = (k: string, x: Values[string]) => {
    const next = { ...v, [k]: x };
    setV(next);
    onChange?.(next);
  };
  const errors = { ...(validate?.(v) ?? {}), ...serverErrors };
  const missing = fields.some((f) => "required" in f && f.required && !String(v[f.key] ?? "").trim()) || fields.some((f) => f.type === "area" && !v[f.key]);
  const ok = !missing && !Object.keys(validate?.(v) ?? {}).length;

  return (
    <Sheet
      open
      onOpenChange={(o) => !o && onClose()}
      title={title}
      description={description}
      footer={
        <>
          {onDelete ? (
            <DeleteButton loading={busy === "delete"} onDelete={async () => {
              setBusy("delete");
              try { await onDelete(); toast("Deleted"); onClose(); } catch (e) { toast.error(errText(e, "Couldn't delete")); } finally { setBusy(null); }
            }} />
          ) : null}
          <Button variant="ghost" onClick={onClose}>Cancel</Button>
          <Button type="submit" form={id} variant="primary" disabled={!ok} loading={busy === "save"}>{submitLabel}</Button>
        </>
      }
    >
      <form
        id={id}
        className="space-y-4"
        onSubmit={async (e) => {
          e.preventDefault();
          if (!ok) return;
          setBusy("save");
          setServerErrors({});
          try {
            await onSubmit(v);
            onClose();
          } catch (err) {
            if (err instanceof ApiError) setServerErrors(err.fieldErrors());
            toast.error(errText(err));
          } finally {
            setBusy(null);
          }
        }}
      >
        {fields.map((f, i) => {
          const err = errors[f.key];
          switch (f.type) {
            case "area":
              return <AreaPicker key={f.key} label={f.label} value={(v[f.key] as Area) || null} onChange={(a) => set(f.key, a)} />;
            case "segmented":
              return (
                <div key={f.key}>
                  <p className="mb-2 text-[13px] font-medium text-text-2">{f.label}</p>
                  <Segmented label={f.label} value={String(v[f.key])} onChange={(x) => set(f.key, x)} options={f.options} />
                </div>
              );
            case "chips": {
              const cur = new Set((v[f.key] as string[]) ?? []);
              return (
                <fieldset key={f.key}>
                  <legend className="mb-2 text-[13px] font-medium text-text-2">{f.label}</legend>
                  <div className="flex flex-wrap gap-1.5">
                    {f.options.map((o) => (
                      <Chip key={o.value} on={cur.has(o.value)} onClick={() => { const x = new Set(cur); if (x.has(o.value)) x.delete(o.value); else x.add(o.value); set(f.key, f.options.map((q) => q.value).filter((q) => x.has(q))); }}>
                        <span className="capitalize">{o.label}</span>
                      </Chip>
                    ))}
                  </div>
                  {f.hint ? <p className="mt-1.5 text-[12.5px] text-text-3">{f.hint}</p> : null}
                </fieldset>
              );
            }
            case "checkbox":
              return (
                <label key={f.key} className="flex cursor-pointer items-start gap-3 rounded-xl border border-line p-3">
                  <input type="checkbox" className="mt-0.5 size-4 accent-[var(--color-accent)]" checked={!!v[f.key]} onChange={(e) => set(f.key, e.target.checked)} />
                  <span>
                    <span className="block text-sm text-text">{f.label}</span>
                    {f.hint ? <span className="text-[12.5px] text-text-3">{f.hint}</span> : null}
                  </span>
                </label>
              );
            case "select":
              return (
                <Field key={f.key} label={f.label} hint={f.hint} error={err}>
                  {(p) => (
                    <NativeSelect {...p} value={String(v[f.key] ?? "")} onChange={(e) => set(f.key, e.target.value)}>
                      {f.options.map((o) => <option key={o.value} value={o.value}>{o.label}</option>)}
                    </NativeSelect>
                  )}
                </Field>
              );
            case "textarea":
              return (
                <Field key={f.key} label={f.label} hint={f.hint} error={err}>
                  {(p) => <Textarea {...p} rows={4} value={String(v[f.key] ?? "")} placeholder={f.placeholder} onChange={(e) => set(f.key, e.target.value)} />}
                </Field>
              );
            case "number":
              return (
                <Field key={f.key} label={f.label} hint={f.hint} error={err}>
                  {(p) => <Input {...p} type="number" required={f.required} min={f.min} max={f.max} step={f.step ?? "any"} value={String(v[f.key] ?? "")} onChange={(e) => set(f.key, e.target.value)} />}
                </Field>
              );
            default:
              return (
                <Field key={f.key} label={f.label} hint={f.hint} error={err}>
                  {(p) => (
                    <Input
                      {...p}
                      autoFocus={i === 0}
                      required={f.required}
                      maxLength={f.maxLength}
                      placeholder={f.placeholder}
                      type={f.type === "datetime" ? "datetime-local" : f.type}
                      value={String(v[f.key] ?? "")}
                      onChange={(e) => set(f.key, e.target.value)}
                    />
                  )}
                </Field>
              );
          }
        })}
        {children}
      </form>
    </Sheet>
  );
}

/** "" → null for optional text; trims. */
export const opt = (x: Values[string] | undefined): string | null => (typeof x === "string" && x.trim() ? x.trim() : null);
