import { useState, type ReactNode } from "react";
import { Trash2 } from "lucide-react";
import { Button } from "@/ui/Button";
import { ApiError } from "@/api/errors";

export function errText(e: unknown, fallback = "Couldn't save") {
  return e instanceof ApiError ? e.detail ?? e.title : fallback;
}

export function fieldErr(e: unknown, name: string): string | undefined {
  return e instanceof ApiError ? e.fieldErrors()[name] : undefined;
}

/** Two-step delete in a sheet footer. */
export function DeleteButton({ onDelete, loading, children = "Delete" }: { onDelete: () => void; loading: boolean; children?: ReactNode }) {
  const [sure, setSure] = useState(false);
  return sure ? (
    <Button variant="danger" className="mr-auto" loading={loading} icon={<Trash2 className="size-4" />} onClick={onDelete}>Delete for good</Button>
  ) : (
    <Button variant="ghost" className="mr-auto" icon={<Trash2 className="size-4" />} onClick={() => setSure(true)}>{children}</Button>
  );
}

export function SectionLabel({ children }: { children: ReactNode }) {
  return <p className="mb-2 mt-6 text-[11px] font-semibold uppercase tracking-wider text-text-3 first:mt-0">{children}</p>;
}
