import { useState } from "react";
import { ShieldCheck } from "lucide-react";
import { useApprovals } from "@/api/chat";
import { Page } from "../common/Page";
import { Card } from "@/ui/Card";
import { EmptyState, ErrorState, Skeleton } from "@/ui/States";
import { cn } from "@/ui/cn";
import { ApprovalCard } from "./ApprovalCard";

type Tab = "pending" | "history";

export function ApprovalsPage() {
  const [tab, setTab] = useState<Tab>("pending");
  const pending = useApprovals("pending");
  const all = useApprovals(undefined);
  const q = tab === "pending" ? pending : all;
  const items = tab === "pending" ? (pending.data?.items ?? []) : (all.data?.items ?? []).filter((a) => a.status !== "pending");
  return (
    <Page title="Approvals" subtitle="Changes the assistant proposed that need your yes or no. Approving runs exactly what is shown.">
      <div className="mb-4 flex gap-1 rounded-xl border border-line p-1 sm:w-fit" role="tablist">
        {(["pending", "history"] as const).map((t) => (
          <button key={t} role="tab" aria-selected={tab === t} onClick={() => setTab(t)}
            className={cn("flex-1 rounded-lg px-4 py-1.5 text-sm capitalize sm:flex-none", tab === t ? "bg-accent/12 text-text shadow-[inset_0_0_0_1px_rgb(34_211_238/.4)]" : "text-text-2 hover:text-text")}>
            {t === "pending" ? `Waiting${pending.data?.items.length ? ` (${pending.data.items.length})` : ""}` : "History"}
          </button>
        ))}
      </div>
      {q.isPending ? (
        <div className="space-y-3">{[0, 1].map((i) => <Skeleton key={i} className="h-32" />)}</div>
      ) : q.error ? (
        <ErrorState error={q.error} retry={() => q.refetch()} />
      ) : !items.length ? (
        <Card>
          <EmptyState art={<ShieldCheck className="size-10 text-accent" />} title={tab === "pending" ? "Nothing waiting" : "No decisions yet"}
            body={tab === "pending" ? "When the assistant wants to delete, bulk-change or touch something you entered, it asks here first." : "Approved, rejected and expired requests appear here."} />
        </Card>
      ) : (
        <div className="grid gap-3 lg:grid-cols-2">{items.map((a) => <ApprovalCard key={a.id} a={a} anchor />)}</div>
      )}
    </Page>
  );
}
