import { useMemo, useState } from "react";
import { AnimatePresence, motion } from "motion/react";
import { Link, NavLink, useNavigate, useParams, useSearchParams } from "react-router";
import { ChevronsLeft, ChevronsRight, Globe, MessageSquarePlus, Send, ShieldCheck } from "lucide-react";
import { useUser } from "@/api/auth";
import { useConversations, useTodayConversation, type Conversation } from "@/api/chat";
import { usePendingApprovals } from "@/api/today";
import { useStream } from "@/api/stream";
import { addDays, fmtDay } from "@/lib/calendar/days";
import { localDay } from "@/lib/time";
import { useNow } from "@/lib/useNow";
import { cn } from "@/ui/cn";
import { Badge } from "@/ui/Badge";
import { IconButton } from "@/ui/Button";
import { Thread } from "./Thread";
import { ApprovalCard } from "../approvals/ApprovalCard";

export function ChatPage() {
  const user = useUser();
  const tz = user.timezone;
  const now = useNow();
  const today = localDay(now, tz, user.day_start_hour);
  const { id } = useParams();
  const [params] = useSearchParams();
  const todayConv = useTodayConversation(today);
  const all = useConversations();
  const viewing = id ? all.data?.items.find((c) => c.id === id) : undefined;
  const isToday = !id || id === todayConv.data?.id;
  const convId = isToday ? todayConv.data?.id ?? null : id;
  const stream = useStream();
  const navigate = useNavigate();

  return (
    <div className="flex h-[calc(100dvh-4rem-4.5rem)] lg:h-[calc(100dvh-4rem)]">
      <History items={all.data?.items ?? []} today={today} todayId={todayConv.data?.id} />
      <section className="flex min-w-0 flex-1 flex-col" aria-label="Conversation">
        <header className="flex items-center gap-3 border-b border-line px-4 py-3 sm:px-6">
          <div className="min-w-0">
            <h1 className="truncate text-lg font-semibold">
              {isToday ? "Today" : viewing?.local_day ? fmtDay(viewing.local_day, "EEEE d MMMM") : "Conversation"}
            </h1>
            <p className="text-[12.5px] text-text-3">
              {isToday ? "Your assistant: plans, tasks, memory and more" : viewing?.channel === "telegram" ? "On Telegram" : "On the web"}
            </p>
          </div>
          <span className={cn("ml-auto inline-flex items-center gap-1.5 text-[12px]", stream.connected ? "text-text-3" : "text-warn")} title={stream.connected ? "Live updates on" : "Reconnecting to live updates…"}>
            <span className={cn("size-1.5 rounded-full", stream.connected ? "bg-good" : "animate-pulse bg-warn")} />
            {stream.connected ? "Live" : "Reconnecting"}
          </span>
          <select
            aria-label="Conversation"
            className="h-8 max-w-36 rounded-lg border border-line bg-surface-2 px-2 text-[12.5px] text-text md:hidden"
            value={isToday ? "" : id}
            onChange={(e) => navigate(e.target.value ? `/chat/c/${e.target.value}` : "/chat")}
          >
            <option value="">Today</option>
            {(all.data?.items ?? []).filter((c) => c.id !== todayConv.data?.id).map((c) => (
              <option key={c.id} value={c.id}>{c.local_day ? fmtDay(c.local_day, "d MMM") : "Earlier"} · {c.channel}</option>
            ))}
          </select>
          {!isToday ? (
            <Link to="/chat" className="inline-flex items-center gap-1.5 rounded-lg border border-line px-3 py-1.5 text-[13px] text-text-2 hover:text-text">
              <MessageSquarePlus className="size-4" /> Today
            </Link>
          ) : null}
        </header>
        <div className="min-h-0 flex-1">
          <Thread
            key={convId ?? "new"}
            conversationId={convId}
            writable={isToday}
            tz={tz}
            draft={params.get("draft") ?? ""}
            loading={isToday && todayConv.isPending}
          />
        </div>
      </section>
      <ContextPanel />
    </div>
  );
}

function History({ items, today, todayId }: { items: Conversation[]; today: string; todayId?: string }) {
  const groups = useMemo(() => {
    const sorted = [...items].sort((a, b) => (b.local_day ?? "").localeCompare(a.local_day ?? "") || (a.channel ?? "").localeCompare(b.channel ?? ""));
    const out = new Map<string, Conversation[]>();
    for (const c of sorted) {
      const k = c.local_day === today ? "Today" : c.local_day === addDays(today, -1) ? "Yesterday" : c.local_day ? fmtDay(c.local_day, "EEE d MMM") : "Earlier";
      (out.get(k) ?? out.set(k, []).get(k)!).push(c);
    }
    return [...out.entries()];
  }, [items, today]);
  return (
    <aside className="hidden w-64 shrink-0 flex-col border-r border-line md:flex" aria-label="Conversation history">
      <div className="px-4 pb-2 pt-4">
        <NavLink to="/chat" end className={({ isActive }) => cn(
          "flex items-center gap-2 rounded-lg border px-3 py-2 text-sm transition-colors",
          isActive ? "border-accent/40 bg-accent/10 text-text" : "border-line text-text-2 hover:text-text")}>
          <MessageSquarePlus className="size-4 text-accent" /> Today's chat
        </NavLink>
      </div>
      <nav className="min-h-0 flex-1 overflow-y-auto px-3 pb-4">
        {groups.map(([label, convs]) => (
          <div key={label} className="mt-3">
            <p className="px-2 pb-1 text-[11px] font-semibold uppercase tracking-wider text-text-3">{label}</p>
            <ul className="space-y-0.5">
              {convs.map((c) => (
                <li key={c.id}>
                  <NavLink
                    to={c.id === todayId ? "/chat" : `/chat/c/${c.id}`}
                    end
                    className={({ isActive }) => cn("flex items-center gap-2 rounded-lg px-2.5 py-2 text-[13px]", isActive ? "bg-surface-3 text-text" : "text-text-2 hover:bg-surface-2 hover:text-text")}
                  >
                    {c.channel === "telegram" ? <Send className="size-3.5 text-[#38bdf8]" aria-hidden /> : <Globe className="size-3.5 text-text-3" aria-hidden />}
                    <span className="flex-1 capitalize">{c.channel}</span>
                    {c.status === "open" ? <span className="size-1.5 rounded-full bg-accent" aria-label="open" /> : null}
                  </NavLink>
                </li>
              ))}
            </ul>
          </div>
        ))}
        {!items.length ? <p className="px-2 pt-4 text-[13px] text-text-3">Conversations appear here, one per day per channel.</p> : null}
      </nav>
    </aside>
  );
}

/**
 * Pending approvals beside the thread. Collapsed to a slim rail by default; it slides open whenever
 * a new approval arrives and folds away again once nothing is waiting. The user can toggle it.
 */
function ContextPanel() {
  const approvals = usePendingApprovals();
  const items = approvals.data?.items ?? [];
  const count = items.length;
  const [open, setOpen] = useState(false);
  const [seen, setSeen] = useState(0);
  if (count !== seen) {
    // Adjusting state from a changed input during render (no effect round-trip).
    setSeen(count);
    if (count > seen) setOpen(true);
    else if (count === 0) setOpen(false);
  }
  return (
    <motion.aside
      className="relative hidden shrink-0 overflow-hidden border-l border-line lg:block"
      aria-label="Waiting for you"
      initial={false}
      animate={{ width: open ? 320 : 52 }}
      transition={{ duration: 0.28, ease: [0.22, 1, 0.36, 1] }}
    >
      <AnimatePresence initial={false} mode="popLayout">
        {open ? (
          <motion.div
            key="open"
            className="flex h-full w-80 flex-col"
            initial={{ opacity: 0, x: 24 }}
            animate={{ opacity: 1, x: 0 }}
            exit={{ opacity: 0, x: 24 }}
            transition={{ duration: 0.2 }}
          >
            <div className="flex items-center gap-2 px-4 pb-3 pt-4">
              <h2 className="flex flex-1 items-center gap-2 text-sm font-semibold">
                <ShieldCheck className="size-4 text-accent" /> Waiting for you {count ? <Badge tone="warn">{count}</Badge> : null}
              </h2>
              <IconButton label="Collapse panel" onClick={() => setOpen(false)}>
                <ChevronsRight className="size-4" />
              </IconButton>
            </div>
            <div className="min-h-0 flex-1 overflow-y-auto px-4 pb-4">
              {count ? (
                <div className="space-y-3">{items.map((a) => <ApprovalCard key={a.id} a={a} />)}</div>
              ) : (
                <p className="text-[13px] text-text-3">Nothing to approve. When the assistant wants to make a bigger change, it asks here first.</p>
              )}
            </div>
          </motion.div>
        ) : (
          <motion.button
            key="rail"
            type="button"
            onClick={() => setOpen(true)}
            aria-label={count ? `Show ${count} waiting for you` : "Show waiting for you"}
            aria-expanded={false}
            className="flex h-full w-[52px] flex-col items-center gap-3 pt-4 text-text-3 transition-colors hover:bg-surface-2 hover:text-text"
            initial={{ opacity: 0 }}
            animate={{ opacity: 1 }}
            exit={{ opacity: 0 }}
            transition={{ duration: 0.15 }}
          >
            <ChevronsLeft className="size-4" aria-hidden />
            <span className="relative">
              <ShieldCheck className={cn("size-5", count ? "text-accent" : "")} aria-hidden />
              {count ? (
                <span className="absolute -right-2 -top-2 grid min-w-4 place-items-center rounded-full bg-warn px-1 text-[10px] font-bold leading-4 text-black">
                  {count}
                </span>
              ) : null}
            </span>
            <span className="text-[11px] font-medium uppercase tracking-wider [writing-mode:vertical-rl]">Waiting for you</span>
          </motion.button>
        )}
      </AnimatePresence>
    </motion.aside>
  );
}
