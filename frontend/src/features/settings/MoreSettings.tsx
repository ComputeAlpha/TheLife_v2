import { useState } from "react";
import { Bell, Bot, CircleCheck, CircleX, Info, Send } from "lucide-react";
import { useLlmStatus, useServerInfo } from "@/api/system";
import { useTelegramLink } from "@/api/settings";
import { useUser } from "@/api/auth";
import { fmt, relative } from "@/lib/time";
import { Badge } from "@/ui/Badge";
import { Button } from "@/ui/Button";
import { Card, CardHeader } from "@/ui/Card";
import { ErrorState, Skeleton } from "@/ui/States";
import { toast } from "@/ui/toast";
import { cn } from "@/ui/cn";

// ---------------------------------------------------------------- notifications

export function NotificationsSettings() {
  const supported = typeof window !== "undefined" && "Notification" in window;
  const [perm, setPerm] = useState(() => (supported ? Notification.permission : "denied"));
  const tg = useTelegramLink();
  return (
    <div className="space-y-6">
      <Card>
        <CardHeader title="In this browser" icon={<Bell className="size-4" />} subtitle="Where alerts show while TheLife is open." />
        <div className="space-y-4 px-5 pb-5 text-sm">
          <Row ok label="While the tab is visible" text="A toast in the corner, and it counts as seen, so Telegram doesn't repeat it." />
          <Row ok={perm === "granted"} label="While the tab is in the background"
            text={!supported ? "This browser can't show notifications." : perm === "granted" ? "A system notification." : perm === "denied" ? "Blocked. Allow notifications for this site in the browser's settings." : "Not allowed yet."} />
          <div className="flex flex-wrap gap-2">
            {supported && perm === "default" ? <Button variant="primary" onClick={() => Notification.requestPermission().then(setPerm)}>Allow browser alerts</Button> : null}
            {supported && perm === "granted" ? (
              <Button onClick={() => {
                const n = new Notification("TheLife", { body: "Alerts work. This is what a reminder looks like.", tag: "thelife-test" });
                setTimeout(() => n.close(), 6000);
                toast.success("Test alert sent");
              }}>Send a test alert</Button>
            ) : null}
          </div>
        </div>
      </Card>
      <Card>
        <CardHeader title="Elsewhere" icon={<Send className="size-4" />} />
        <div className="space-y-3 px-5 pb-5 text-sm">
          <Row ok={!!tg.data?.linked} label="Telegram" text={tg.data?.linked ? "Anything you haven't seen in the browser goes to Telegram." : "Not linked. Link it under Connections to get alerts on your phone."} />
          <Row ok label="Desktop collector" text="Collector devices show reminders and approvals as desktop notifications." />
        </div>
      </Card>
    </div>
  );
}

function Row({ ok, label, text }: { ok: boolean; label: string; text: string }) {
  return (
    <div className="flex gap-3">
      {ok ? <CircleCheck className="mt-0.5 size-4 shrink-0 text-good" aria-label="On" /> : <CircleX className="mt-0.5 size-4 shrink-0 text-text-3" aria-label="Off" />}
      <div><p className="text-text">{label}</p><p className="text-[13px] text-text-3">{text}</p></div>
    </div>
  );
}

// ---------------------------------------------------------------- assistant (LLM)

const STATE_TONE = { healthy: "good", throttled: "warn", exhausted: "alert", probing: "accent", disabled: "neutral" } as const;

export function AssistantSettings() {
  const q = useLlmStatus();
  const user = useUser();
  const d = q.data;
  const est = d?.usage.weekly_request_estimate ?? 0;
  const used = d?.usage.requests ?? 0;
  return (
    <Card>
      <CardHeader title="Assistant model" icon={<Bot className="size-4" />} subtitle="Which language model answers, and how much of this week's budget is used." />
      <div className="space-y-5 px-5 pb-5">
        {q.isPending ? <Skeleton className="h-32" /> : q.error ? <ErrorState error={q.error} retry={() => q.refetch()} /> : d ? (
          <>
            <div className="flex flex-wrap items-center gap-3">
              <span className={cn("size-2.5 rounded-full", d.mode === "online" ? "bg-accent shadow-[0_0_10px_var(--color-accent)]" : d.mode === "manual" ? "bg-warn" : "bg-text-3")} aria-hidden />
              <span className="text-sm text-text">{d.mode === "online" ? "Online" : d.mode === "manual" ? "Offline: built-in commands only" : "Not configured"}</span>
              {d.mode === "manual" && d.resets_at ? <span className="text-[12.5px] text-text-3">back {relative(d.resets_at)} ({fmt(d.resets_at, user.timezone, "EEE HH:mm")})</span> : null}
            </div>
            {d.providers.length ? (
              <ul className="divide-y divide-line rounded-xl border border-line" aria-label="Providers">
                {d.providers.map((p) => (
                  <li key={p.name} className="flex flex-wrap items-center gap-3 px-4 py-3">
                    <span className="min-w-0 flex-1">
                      <span className="block text-sm text-text">{p.name} <span className="text-text-3">· {p.role}</span></span>
                      <span className="font-mono text-[12px] text-text-3">{p.kind}{p.model ? ` · ${p.model}` : ""}{p.quota_scope ? ` · quota ${p.quota_scope}` : ""}</span>
                    </span>
                    <Badge tone={STATE_TONE[p.state]} className="capitalize">{p.state}</Badge>
                    {p.until ? <span className="text-[12px] text-text-3">until {fmt(p.until, user.timezone, "EEE HH:mm")}</span> : null}
                  </li>
                ))}
              </ul>
            ) : null}
            <div>
              <div className="mb-1.5 flex justify-between text-[13px]"><span className="text-text-2">This week{d.usage.week_start ? ` (from ${d.usage.week_start})` : ""}</span><span className="font-mono text-text">{used}{est ? ` / ~${est}` : ""} requests</span></div>
              {est ? <div className="h-1.5 overflow-hidden rounded-full bg-surface-3"><div className={cn("h-full rounded-full", used / est > 0.9 ? "bg-warn" : "bg-accent")} style={{ width: `${Math.min(100, (used / est) * 100)}%` }} /></div> : null}
              <dl className="mt-3 grid grid-cols-2 gap-3 text-[13px] sm:grid-cols-4">
                <Stat label="Tokens in" v={d.usage.tokens_in} />
                <Stat label="Tokens out" v={d.usage.tokens_out} />
                <Stat label="Images" v={d.usage.images} />
                <Stat label="Failures" v={d.usage.failures} />
              </dl>
            </div>
            <p className="text-[12.5px] text-text-3">Providers and keys are set on the server (environment), not here.</p>
          </>
        ) : null}
      </div>
    </Card>
  );
}

function Stat({ label, v }: { label: string; v?: number }) {
  return (
    <div className="rounded-lg border border-line px-3 py-2">
      <dt className="text-[11px] uppercase tracking-wider text-text-3">{label}</dt>
      <dd className="font-mono text-text">{(v ?? 0).toLocaleString()}</dd>
    </div>
  );
}

// ---------------------------------------------------------------- about

export function AboutSettings() {
  const q = useServerInfo();
  const checks = Object.entries(q.data?.ready?.checks ?? {});
  return (
    <Card>
      <CardHeader title="About" icon={<Info className="size-4" />} />
      <dl className="grid gap-4 px-5 pb-5 text-sm sm:grid-cols-2">
        <div><dt className="text-[12px] text-text-3">Web app</dt><dd className="font-mono text-text">v{__APP_VERSION__}</dd><dd className="text-[12px] text-text-3">built {fmt(__BUILT_AT__, "UTC", "d MMM yyyy, HH:mm")} UTC</dd></div>
        <div>
          <dt className="text-[12px] text-text-3">Server</dt>
          <dd className="font-mono text-text">{q.isPending ? "…" : q.data?.version ? `v${q.data.version}` : "unknown"}</dd>
          {q.data?.ready?.schema_version != null ? <dd className="text-[12px] text-text-3">database schema {String(q.data.ready.schema_version)}</dd> : null}
        </div>
        <div className="sm:col-span-2">
          <dt className="mb-1.5 text-[12px] text-text-3">Readiness</dt>
          <dd className="flex flex-wrap items-center gap-2">
            <Badge tone={q.data?.ready?.status === "ready" ? "good" : "warn"}>{q.data?.ready?.status === "ready" ? "Ready" : q.isPending ? "Checking" : "Not ready"}</Badge>
            {checks.map(([k, v]) => <Badge key={k} tone={v === "ok" || v === true ? "neutral" : "warn"}>{k}: {typeof v === "object" ? JSON.stringify(v) : String(v)}</Badge>)}
          </dd>
        </div>
      </dl>
      {q.error ? <div className="px-5 pb-5"><ErrorState error={q.error} retry={() => q.refetch()} /></div> : null}
    </Card>
  );
}
