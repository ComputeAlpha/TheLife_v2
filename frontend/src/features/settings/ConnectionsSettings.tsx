import { useEffect, useState } from "react";
import { Cpu, KeyRound, Laptop, Link2Off, Plus, Send, ShieldOff } from "lucide-react";
import {
  useApiKeys, useDeviceList, useIssueApiKey, useRegisterDevice, useRevokeApiKey, useRevokeDevice, useTelegramCode,
  useTelegramLink, useTelegramUnlink,
} from "@/api/settings";
import { SCOPES, type ApiKey, type Device, type Scope } from "@/api/types";
import { fmt, relative } from "@/lib/time";
import { useNow } from "@/lib/useNow";
import { Badge } from "@/ui/Badge";
import { Button } from "@/ui/Button";
import { Card, CardHeader } from "@/ui/Card";
import { Dialog } from "@/ui/Dialog";
import { Field, Input } from "@/ui/Field";
import { Chip } from "@/ui/Segmented";
import { EmptyState, ErrorState, Skeleton } from "@/ui/States";
import { toast } from "@/ui/toast";
import { cn } from "@/ui/cn";
import { errText } from "../plan/shared";
import { TokenOnce } from "./TokenOnce";

export function ConnectionsSettings() {
  return (
    <div className="space-y-6">
      <TelegramCard />
      <DevicesCard />
      <ApiKeysCard />
    </div>
  );
}

function TelegramCard() {
  const now = useNow(1000);
  const link = useTelegramLink();
  const code = useTelegramCode();
  const unlink = useTelegramUnlink();
  const [confirm, setConfirm] = useState(false);
  const c = code.data;
  const left = c ? Math.max(0, Math.round((Date.parse(c.expires_at) - now) / 1000)) : 0;
  // Once linked, the code is spent: poll while one is showing so the card flips by itself.
  const { refetch } = link;
  useEffect(() => {
    if (!c || left <= 0 || link.data?.linked) return;
    const t = setInterval(() => refetch(), 4000);
    return () => clearInterval(t);
  }, [c, left, link.data?.linked, refetch]);
  const l = link.data;
  return (
    <Card>
      <CardHeader title="Telegram" icon={<Send className="size-4" />} subtitle="Chat with the assistant and get reminders and digests on Telegram." />
      <div className="px-5 pb-5">
        {link.isPending ? <Skeleton className="h-16" /> : link.error ? <ErrorState error={link.error} retry={() => link.refetch()} /> : !l?.enabled ? (
          <p className="text-[13px] text-text-3">Telegram isn't set up on this server (no bot token configured).</p>
        ) : l.linked ? (
          <div className="flex flex-wrap items-center gap-3">
            <Badge tone="good">Linked</Badge>
            <span className="text-sm text-text">{l.username ? `@${l.username}` : "Your Telegram account"}</span>
            {l.linked_at ? <span className="text-[12.5px] text-text-3">since {fmt(l.linked_at, "UTC", "d MMM yyyy")}</span> : null}
            <Button className="ml-auto" variant="ghost" icon={<Link2Off className="size-4" />} onClick={() => setConfirm(true)}>Unlink</Button>
          </div>
        ) : c && left > 0 ? (
          <div className="space-y-3">
            <p className="text-sm text-text-2">Send this to the bot within <span className="font-mono text-text">{Math.floor(left / 60)}:{String(left % 60).padStart(2, "0")}</span>:</p>
            <p aria-label="Link code" className="w-fit rounded-xl border border-accent/40 bg-accent/[.06] px-5 py-3 font-mono text-2xl tracking-[.3em] text-accent">/link {c.code}</p>
            <p className="text-[12.5px] text-text-3">Open your bot in Telegram and send the line above. This card updates when it's done.</p>
          </div>
        ) : (
          <div className="flex flex-wrap items-center gap-3">
            <p className="flex-1 text-sm text-text-2">Not linked.{c ? " The last code expired." : ""}</p>
            <Button variant="primary" loading={code.isPending} onClick={() => code.mutate(undefined, { onError: (e) => toast.error(errText(e)) })}>Get a link code</Button>
          </div>
        )}
      </div>
      <Dialog open={confirm} onOpenChange={setConfirm} title="Unlink Telegram?" description="The bot stops answering you and reminders go to the web only. You can link again any time."
        footer={<><Button variant="ghost" onClick={() => setConfirm(false)}>Cancel</Button><Button variant="danger" loading={unlink.isPending} onClick={() => unlink.mutate(undefined, { onSuccess: () => { toast("Telegram unlinked"); setConfirm(false); code.reset(); }, onError: (e) => toast.error(errText(e)) })}>Unlink</Button></>}>
        <span />
      </Dialog>
    </Card>
  );
}

function DevicesCard() {
  const now = useNow();
  const q = useDeviceList();
  const reg = useRegisterDevice();
  const revoke = useRevokeDevice();
  const [name, setName] = useState("");
  const [adding, setAdding] = useState(false);
  const [token, setToken] = useState<string | null>(null);
  const [revoking, setRevoking] = useState<Device | null>(null);
  const list = [...(q.data ?? [])].sort((a, b) => Number(!!a.revoked_at) - Number(!!b.revoked_at) || (b.last_seen_at ?? "").localeCompare(a.last_seen_at ?? ""));
  const origin = location.origin;
  return (
    <Card>
      <CardHeader title="Devices" icon={<Laptop className="size-4" />} subtitle="Computers running the collector." action={<Button size="sm" icon={<Plus className="size-3.5" />} onClick={() => setAdding(true)}>Register</Button>} />
      <div className="px-5 pb-5">
        {adding ? (
          <form className="mb-4 flex items-end gap-2 rounded-xl border border-line p-3" onSubmit={(e) => {
            e.preventDefault();
            if (!name.trim()) return;
            reg.mutate(name.trim(), { onSuccess: (r) => { setToken(r.token); setName(""); setAdding(false); }, onError: (err) => toast.error(errText(err)) });
          }}>
            <Field label="Device name" className="flex-1">{(f) => <Input {...f} autoFocus maxLength={200} value={name} onChange={(e) => setName(e.target.value)} placeholder="Work laptop" />}</Field>
            <span className="pb-2.5 text-[12.5px] text-text-3">Linux (X11)</span>
            <Button type="submit" variant="primary" loading={reg.isPending} disabled={!name.trim()}>Register</Button>
            <Button variant="ghost" onClick={() => setAdding(false)}>Cancel</Button>
          </form>
        ) : null}
        {q.isPending ? <Skeleton className="h-20" /> : q.error ? <ErrorState error={q.error} retry={() => q.refetch()} /> : !list.length ? (
          <EmptyState className="py-6" art={<Cpu className="size-9 text-accent" />} title="No devices" body="Register a computer, then enroll the collector on it with the token you get." />
        ) : (
          <ul className="divide-y divide-line">
            {list.map((d) => {
              const seen = d.last_seen_at ? now - Date.parse(d.last_seen_at) : null;
              const online = !d.revoked_at && seen != null && seen < 10 * 60_000;
              const paused = d.status?.paused_until && Date.parse(d.status.paused_until) > now;
              return (
                <li key={d.id} className="flex flex-wrap items-center gap-3 py-3">
                  <span className={cn("size-2 shrink-0 rounded-full", d.revoked_at ? "bg-text-3/50" : online ? "bg-good shadow-[0_0_8px_var(--color-good)]" : "bg-warn")} aria-hidden />
                  <div className="min-w-0 flex-1">
                    <p className={cn("text-sm", d.revoked_at ? "text-text-3 line-through" : "text-text")}>{d.name}{d.hostname ? <span className="text-text-3"> · {d.hostname}</span> : null}</p>
                    <p className="text-[12px] text-text-3">
                      {d.revoked_at ? `revoked ${relative(d.revoked_at, now)}` : d.last_seen_at ? `seen ${relative(d.last_seen_at, now)}` : "never connected"}
                      {d.collector_version ? ` · v${d.collector_version}` : ""}
                      {paused ? " · paused" : ""}
                      {d.status?.x_ok === false ? " · can't reach the display" : ""}
                      {d.status?.queue_depth ? ` · ${d.status.queue_depth} queued` : ""}
                    </p>
                    {d.status?.last_error && !d.revoked_at ? <p className="text-[12px] text-alert-text">{d.status.last_error}</p> : null}
                  </div>
                  {d.revoked_at ? <Badge>Revoked</Badge> : <Button size="sm" variant="ghost" icon={<ShieldOff className="size-3.5" />} onClick={() => setRevoking(d)}>Revoke</Button>}
                </li>
              );
            })}
          </ul>
        )}
      </div>
      <TokenOnce token={token} title="Device token" onDone={() => setToken(null)}>
        <div className="space-y-2 text-[13px] text-text-2">
          <p>On that computer, run this and paste the token when it asks:</p>
          <code className="block rounded-lg border border-line bg-surface-2 px-3 py-2 font-mono text-[12.5px] text-text">thelife-collector enroll --url {origin} --web-ui {origin}</code>
          <p className="text-text-3">Then start it: <code className="font-mono">systemctl --user enable --now thelife-collector</code></p>
        </div>
      </TokenOnce>
      <Dialog open={!!revoking} onOpenChange={(o) => !o && setRevoking(null)} title={`Revoke ${revoking?.name ?? "device"}?`} description="Its token stops working at once. Detections it already sent are kept."
        footer={<><Button variant="ghost" onClick={() => setRevoking(null)}>Cancel</Button><Button variant="danger" loading={revoke.isPending} onClick={() => revoking && revoke.mutate(revoking, { onSuccess: () => { toast("Device revoked"); setRevoking(null); }, onError: (e) => toast.error(errText(e)) })}>Revoke</Button></>}>
        <span />
      </Dialog>
    </Card>
  );
}

function ApiKeysCard() {
  const now = useNow();
  const q = useApiKeys();
  const issue = useIssueApiKey();
  const revoke = useRevokeApiKey();
  const [adding, setAdding] = useState(false);
  const [name, setName] = useState("");
  const [scopes, setScopes] = useState<Set<Scope>>(new Set());
  const [expires, setExpires] = useState("");
  const [token, setToken] = useState<string | null>(null);
  const [revoking, setRevoking] = useState<ApiKey | null>(null);
  const groups = [...new Set(SCOPES.map((s) => s.split(":")[0]!))];
  const list = [...(q.data ?? [])].sort((a, b) => Number(!!a.revoked_at) - Number(!!b.revoked_at) || b.created_at.localeCompare(a.created_at));
  return (
    <Card>
      <CardHeader title="API keys" icon={<KeyRound className="size-4" />} subtitle="For your own scripts. Each key only gets the scopes you pick." action={<Button size="sm" icon={<Plus className="size-3.5" />} onClick={() => setAdding(true)}>New key</Button>} />
      <div className="px-5 pb-5">
        {adding ? (
          <form className="mb-4 space-y-3 rounded-xl border border-line p-4" onSubmit={(e) => {
            e.preventDefault();
            if (!name.trim() || !scopes.size) return;
            issue.mutate({ name: name.trim(), scopes: SCOPES.filter((s) => scopes.has(s)), expires_at: expires ? new Date(`${expires}T23:59:59Z`).toISOString() : null }, {
              onSuccess: (r) => { setToken(r.token); setAdding(false); setName(""); setScopes(new Set()); setExpires(""); },
              onError: (err) => toast.error(errText(err)),
            });
          }}>
            <div className="grid gap-3 sm:grid-cols-[1fr_180px]">
              <Field label="Name">{(f) => <Input {...f} autoFocus maxLength={200} value={name} onChange={(e) => setName(e.target.value)} placeholder="Home Assistant" />}</Field>
              <Field label="Expires" hint="Optional.">{(f) => <Input {...f} type="date" value={expires} onChange={(e) => setExpires(e.target.value)} />}</Field>
            </div>
            <fieldset>
              <legend className="mb-2 text-[13px] font-medium text-text-2">Scopes</legend>
              <div className="grid gap-2 sm:grid-cols-2">
                {groups.map((g) => (
                  <div key={g} className="flex items-center gap-2">
                    <span className="w-24 text-[12.5px] capitalize text-text-2">{g}</span>
                    {(["read", "write"] as const).map((rw) => {
                      const s = `${g}:${rw}` as Scope;
                      return <Chip key={s} on={scopes.has(s)} onClick={() => setScopes((x) => { const n = new Set(x); if (n.has(s)) n.delete(s); else n.add(s); return n; })}><span aria-label={s}>{rw}</span></Chip>;
                    })}
                  </div>
                ))}
              </div>
            </fieldset>
            <div className="flex justify-end gap-2">
              <Button variant="ghost" onClick={() => setAdding(false)}>Cancel</Button>
              <Button type="submit" variant="primary" loading={issue.isPending} disabled={!name.trim() || !scopes.size}>Create key</Button>
            </div>
          </form>
        ) : null}
        {q.isPending ? <Skeleton className="h-16" /> : q.error ? <ErrorState error={q.error} retry={() => q.refetch()} /> : !list.length ? (
          <p className="text-[13px] text-text-3">No keys.</p>
        ) : (
          <ul className="divide-y divide-line">
            {list.map((k) => {
              const expired = k.expires_at && Date.parse(k.expires_at) < now;
              return (
                <li key={k.id} className="flex flex-wrap items-center gap-3 py-3">
                  <KeyRound className="size-4 shrink-0 text-text-3" aria-hidden />
                  <div className="min-w-0 flex-1">
                    <p className={cn("text-sm", k.revoked_at || expired ? "text-text-3 line-through" : "text-text")}>{k.name} <span className="font-mono text-[12px] text-text-3">{k.token_prefix}…</span></p>
                    <p className="text-[12px] text-text-3">
                      {k.scopes.join(", ")}
                      {" · "}{k.last_used_at ? `used ${relative(k.last_used_at, now)}` : "never used"}
                      {k.expires_at ? ` · ${expired ? "expired" : "expires"} ${fmt(k.expires_at, "UTC", "d MMM yyyy")}` : ""}
                    </p>
                  </div>
                  {k.revoked_at ? <Badge>Revoked</Badge> : <Button size="sm" variant="ghost" icon={<ShieldOff className="size-3.5" />} onClick={() => setRevoking(k)}>Revoke</Button>}
                </li>
              );
            })}
          </ul>
        )}
      </div>
      <TokenOnce token={token} title="API key" onDone={() => setToken(null)}>
        <p className="text-[13px] text-text-2">Send it as <code className="font-mono text-text">Authorization: Bearer &lt;key&gt;</code>.</p>
      </TokenOnce>
      <Dialog open={!!revoking} onOpenChange={(o) => !o && setRevoking(null)} title={`Revoke ${revoking?.name ?? "key"}?`} description="Anything using it stops working at once."
        footer={<><Button variant="ghost" onClick={() => setRevoking(null)}>Cancel</Button><Button variant="danger" loading={revoke.isPending} onClick={() => revoking && revoke.mutate(revoking, { onSuccess: () => { toast("Key revoked"); setRevoking(null); }, onError: (e) => toast.error(errText(e)) })}>Revoke</Button></>}>
        <span />
      </Dialog>
    </Card>
  );
}
