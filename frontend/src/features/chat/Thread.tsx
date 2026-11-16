import { useEffect, useLayoutEffect, useMemo, useReducer, useRef, useState, type KeyboardEvent } from "react";
import { useLocation } from "react-router";
import { AnimatePresence, motion } from "motion/react";
import { AlertTriangle, ArrowUp, Bell, Bot, ChevronRight, CircleCheck, CircleX, EyeOff, Loader2, RotateCw, Wrench } from "lucide-react";
import { useMessages, useSendMessage, newId, type Outgoing } from "@/api/chat";
import { liveReducer, pruneLive, toolLabel, type LiveTurn } from "@/api/live";
import { useStream } from "@/api/stream";
import { useLlmStatus } from "@/api/system";
import { useTurn } from "@/api/chat";
import { ApiError } from "@/api/errors";
import { Markdown } from "@/ui/Markdown";
import { Button } from "@/ui/Button";
import { Skeleton } from "@/ui/States";
import { cn } from "@/ui/cn";
import { fmt, relative } from "@/lib/time";
import { ApprovalById } from "../approvals/ApprovalCard";
import { pretty, transcript, type Entry, type Step } from "./transcript";

const STARTERS = ["Plan my day", "What did I do yesterday?", "Add a daily reading habit of 20 pages", "How is my week going?"];

export function Thread({ conversationId, writable, tz, compact, draft = "", loading }: {
  conversationId: string | null | undefined;
  writable: boolean;
  tz: string;
  compact?: boolean;
  draft?: string;
  loading?: boolean;
}) {
  const msgs = useMessages(conversationId);
  const entries = useMemo(() => transcript(msgs.data ?? []), [msgs.data]);
  const stored = useMemo(() => new Set((msgs.data ?? []).map((m) => m.id)), [msgs.data]);
  const [live, dispatch] = useReducer(liveReducer, {});
  const [sent, setOutgoing] = useState<Outgoing[]>([]);
  // Sent messages and finished turns show until the stored list has them (derived, nothing to clean up).
  const outgoing = sent.filter((x) => !(x.turn && stored.has(x.turn.message_id)));
  const stream = useStream();
  const send = useSendMessage();
  const scroller = useRef<HTMLDivElement>(null);
  const stick = useRef(true);
  const location = useLocation();
  const highlight = location.hash.startsWith("#approval-") ? location.hash.slice("#approval-".length) : null;

  useEffect(() => (writable ? stream.subscribe((e) => e.type !== "notification" && dispatch(e)) : undefined), [stream, writable]);

  const liveTurns = Object.values(pruneLive(live, stored)).filter((t) => t.status !== "merged" && !(t.status === "done" && !t.messageId));
  const contentKey = `${entries.length}:${outgoing.length}:${liveTurns.map((t) => t.text.length + t.running.join()).join()}`;

  // Follow new content while the reader is at the bottom.
  useLayoutEffect(() => {
    const el = scroller.current;
    if (el && stick.current && !highlight) el.scrollTop = el.scrollHeight;
  }, [contentKey, highlight]);
  useEffect(() => {
    if (!highlight || !msgs.data) return;
    const t = setTimeout(() => document.getElementById(`approval-${highlight}`)?.scrollIntoView({ behavior: "smooth", block: "center" }), 150);
    return () => clearTimeout(t);
  }, [highlight, msgs.data]);

  const submit = (text: string, clientId = newId()) => {
    const t = text.trim();
    if (!t) return;
    stick.current = true;
    setOutgoing((o) => [...o.filter((x) => x.clientId !== clientId), { clientId, text: t }]);
    send.mutate(
      { text: t, clientId },
      {
        onSuccess: (turn) => setOutgoing((o) => o.map((x) => (x.clientId === clientId ? { ...x, turn, error: undefined } : x))),
        onError: (error) => setOutgoing((o) => o.map((x) => (x.clientId === clientId ? { ...x, error } : x))),
      },
    );
  };

  const empty = !loading && !msgs.isPending && !entries.length && !outgoing.length && !liveTurns.length;

  return (
    <div className="flex h-full min-h-0 flex-col">
      <div
        ref={scroller}
        onScroll={(e) => {
          const el = e.currentTarget;
          stick.current = el.scrollHeight - el.scrollTop - el.clientHeight < 80;
        }}
        className={cn("min-h-0 flex-1 overflow-y-auto", compact ? "px-4 py-4" : "px-4 py-6 sm:px-8")}
        aria-live="polite"
        aria-busy={liveTurns.some((t) => t.status === "running")}
      >
        <div className={cn("mx-auto space-y-5", compact ? "" : "max-w-3xl")}>
          {loading || (conversationId && msgs.isPending) ? (
            <div className="space-y-4"><Skeleton className="ml-auto h-10 w-1/2" /><Skeleton className="h-24" /><Skeleton className="ml-auto h-10 w-1/3" /></div>
          ) : null}
          {empty ? <Welcome writable={writable} onPick={(s) => submit(s)} /> : null}
          {entries.map((e, i) => (
            <EntryView key={e.id} e={e} tz={tz} highlight={highlight}
              continued={i > 0 && entries[i - 1]!.role === "assistant" && e.role === "assistant" && !!e.turnId && entries[i - 1]!.turnId === e.turnId} />
          ))}
          {outgoing.map((o) => (
            <div key={o.clientId} className="flex flex-col items-end gap-1">
              <UserBubble text={o.text} pending={!o.turn && !o.error} />
              {o.error ? (
                <p className="flex items-center gap-2 text-[12.5px] text-alert-text">
                  <AlertTriangle className="size-3.5" /> Not sent{o.error instanceof ApiError ? `: ${o.error.summary}` : ""}
                  <button className="inline-flex items-center gap-1 text-accent hover:underline" onClick={() => submit(o.text, o.clientId)}>
                    <RotateCw className="size-3" /> Retry
                  </button>
                </p>
              ) : null}
            </div>
          ))}
          {liveTurns.map((t) => <LiveView key={t.turnId} t={t} />)}
        </div>
      </div>
      {writable ? <Composer onSend={(t) => submit(t)} initial={draft} compact={compact} /> : (
        <p className="border-t border-line px-6 py-3 text-center text-[13px] text-text-3">An earlier conversation: read only. Today's chat continues where you left off.</p>
      )}
    </div>
  );
}

function Welcome({ writable, onPick }: { writable: boolean; onPick: (s: string) => void }) {
  return (
    <div className="flex flex-col items-center gap-5 py-10 text-center">
      <div className="relative grid size-16 place-items-center rounded-2xl border border-accent/30 bg-accent/5 shadow-[0_0_40px_-10px_rgb(34_211_238/.6)]">
        <Bot className="size-8 text-accent" />
      </div>
      <div>
        <h2 className="text-xl font-semibold">What's on your mind?</h2>
        <p className="mt-1 text-sm text-text-2">Plan, log, ask, or change anything. Big changes wait for your approval.</p>
      </div>
      {writable ? (
        <div className="flex max-w-xl flex-wrap justify-center gap-2">
          {STARTERS.map((s) => (
            <button key={s} onClick={() => onPick(s)} className="rounded-full border border-line px-3.5 py-1.5 text-[13px] text-text-2 transition-colors hover:border-accent/50 hover:text-text">
              {s}
            </button>
          ))}
        </div>
      ) : null}
    </div>
  );
}

function UserBubble({ text, pending }: { text: string; pending?: boolean }) {
  return (
    <div className={cn("max-w-[85%] whitespace-pre-wrap break-words rounded-2xl rounded-br-md border border-accent/25 bg-accent/[.08] px-4 py-2.5 text-[14.5px] text-text", pending && "opacity-70")}>
      {text}
    </div>
  );
}

function EntryView({ e, tz, highlight, continued }: { e: Entry; tz: string; highlight: string | null; continued?: boolean }) {
  const time = <span className="text-[11px] tabular text-text-3">{fmt(e.createdAt, tz, "HH:mm")}</span>;
  if (e.role === "user") {
    return (
      <div className="flex flex-col items-end gap-1">
        <UserBubble text={e.content} />
        {time}
      </div>
    );
  }
  if (e.kind === "suppressed") {
    return (
      <p className="flex items-center justify-center gap-2 text-[12px] text-text-3"><EyeOff className="size-3.5" /> The assistant held back a message here {time}</p>
    );
  }
  if (e.kind === "deterministic" || e.kind === "error") {
    return (
      <div className={cn("flex items-start gap-3 rounded-xl border px-4 py-3 text-sm", e.kind === "error" ? "border-alert/40 bg-alert/5" : "border-line")}>
        {e.kind === "error" ? <AlertTriangle className="mt-0.5 size-4 shrink-0 text-alert-text" /> : <Bell className="mt-0.5 size-4 shrink-0 text-accent" />}
        <div className="min-w-0 flex-1"><Markdown>{e.content}</Markdown></div>
        {time}
      </div>
    );
  }
  return (
    <div className={cn("flex gap-3", continued && "-mt-3")}>
      {continued ? <span className="w-7 shrink-0" aria-hidden /> : <Avatar />}
      <div className="min-w-0 flex-1 space-y-2">
        {e.steps.length ? (
          <div className="space-y-1.5">
            {e.steps.map((s) => <StepView key={s.id} s={s} />)}
          </div>
        ) : null}
        {e.steps.filter((s) => s.approvalId).map((s) => <ApprovalById key={s.approvalId} id={s.approvalId!} highlight={highlight === s.approvalId} />)}
        {e.content ? <Markdown>{e.content}</Markdown> : null}
        {e.content ? time : null}
      </div>
    </div>
  );
}

function Avatar({ busy }: { busy?: boolean }) {
  return (
    <span className={cn("mt-0.5 grid size-7 shrink-0 place-items-center rounded-lg border border-accent/30 bg-accent/10", busy && "animate-pulse-ring")}>
      <Bot className="size-4 text-accent" aria-hidden />
    </span>
  );
}

function StepView({ s }: { s: Step }) {
  const [open, setOpen] = useState(false);
  const held = !!s.approvalId;
  return (
    <div className="rounded-lg border border-line text-[13px]">
      <button onClick={() => setOpen((o) => !o)} aria-expanded={open} className="flex w-full items-center gap-2 px-3 py-1.5 text-left text-text-2 hover:text-text">
        <ChevronRight className={cn("size-3.5 transition-transform", open && "rotate-90")} aria-hidden />
        {held ? <Wrench className="size-3.5 text-warn" /> : s.failed ? <CircleX className="size-3.5 text-alert-text" /> : s.result == null ? <Loader2 className="size-3.5 animate-spin text-accent" /> : <CircleCheck className="size-3.5 text-good" />}
        <span className="truncate">{toolLabel(s.name)}</span>
        {held ? <span className="text-[11.5px] text-warn">needs approval</span> : null}
        <span className="ml-auto font-mono text-[11px] text-text-3">{s.name}</span>
      </button>
      {open ? (
        <div className="space-y-2 border-t border-line px-3 py-2">
          <div>
            <p className="mb-1 text-[11px] uppercase tracking-wider text-text-3">Arguments</p>
            <pre tabIndex={0} className="max-h-48 overflow-auto whitespace-pre-wrap break-words rounded-md bg-surface-2 p-2 font-mono text-[11.5px] text-text-2">{pretty(s.arguments)}</pre>
          </div>
          {s.result != null ? (
            <div>
              <p className="mb-1 text-[11px] uppercase tracking-wider text-text-3">Result</p>
              <pre tabIndex={0} className="max-h-48 overflow-auto whitespace-pre-wrap break-words rounded-md bg-surface-2 p-2 font-mono text-[11.5px] text-text-2">{pretty(s.result)}</pre>
            </div>
          ) : null}
        </div>
      ) : null}
    </div>
  );
}

function LiveView({ t }: { t: LiveTurn }) {
  const failed = t.status === "failed";
  const turn = useTurn(failed ? t.turnId : null);
  return (
    <div className="flex gap-3">
      <Avatar busy={t.status === "running"} />
      <div className="min-w-0 flex-1 space-y-2">
        <AnimatePresence initial={false}>
          {t.running.map((tool, i) => (
            <motion.div key={tool + i} initial={{ opacity: 0, y: 4 }} animate={{ opacity: 1, y: 0 }} exit={{ opacity: 0 }}
              className="flex items-center gap-2 rounded-lg border border-accent/30 bg-accent/5 px-3 py-1.5 text-[13px] text-text-2">
              <Loader2 className="size-3.5 animate-spin text-accent" /> {toolLabel(tool)}…
            </motion.div>
          ))}
        </AnimatePresence>
        {failed ? (
          <p className="flex items-center gap-2 rounded-xl border border-alert/40 bg-alert/5 px-4 py-3 text-sm text-alert-text">
            <AlertTriangle className="size-4" /> The assistant couldn't finish this reply{turn.data?.error ? `: ${turn.data.error}` : "."}
          </p>
        ) : t.text ? (
          <div className="relative">
            <Markdown>{t.text}</Markdown>
            <span className="ml-0.5 inline-block h-4 w-[3px] translate-y-0.5 animate-pulse rounded-full bg-accent shadow-[0_0_8px_var(--color-accent)]" aria-hidden />
          </div>
        ) : t.reset ? (
          <div className="skeleton h-5 w-2/3 rounded-md" aria-label="Rethinking" />
        ) : !t.running.length ? (
          <span className="inline-flex gap-1 py-2" aria-label="Thinking">
            {[0, 1, 2].map((i) => <span key={i} className="size-1.5 animate-bounce rounded-full bg-accent" style={{ animationDelay: `${i * 120}ms` }} />)}
          </span>
        ) : null}
      </div>
    </div>
  );
}

function Composer({ onSend, initial, compact }: { onSend: (t: string) => void; initial: string; compact?: boolean }) {
  const [text, setText] = useState(initial);
  const ref = useRef<HTMLTextAreaElement>(null);
  const status = useLlmStatus();
  const offline = status.data?.mode === "manual" || status.data?.mode === "unconfigured";
  useLayoutEffect(() => {
    const el = ref.current;
    if (!el) return;
    el.style.height = "auto";
    el.style.height = `${Math.min(200, el.scrollHeight)}px`;
  }, [text]);
  useEffect(() => {
    if (!compact) ref.current?.focus();
  }, [compact]);
  const go = () => {
    if (!text.trim()) return;
    onSend(text);
    setText("");
  };
  const onKey = (e: KeyboardEvent<HTMLTextAreaElement>) => {
    if (e.key === "Enter" && !e.shiftKey && !e.nativeEvent.isComposing) {
      e.preventDefault();
      go();
    }
  };
  return (
    <div className={cn("border-t border-line bg-bg", compact ? "p-3" : "px-4 py-4 sm:px-8")}>
      <div className="mx-auto max-w-3xl">
        {offline ? (
          <p className="mb-2 flex items-center gap-2 text-[12.5px] text-warn">
            <AlertTriangle className="size-3.5" />
            {status.data?.mode === "unconfigured" ? "No assistant model is configured: only built-in commands answer." : `The assistant is offline${status.data?.resets_at ? ` (back ${relative(status.data.resets_at)})` : ""}: built-in commands still work.`}
          </p>
        ) : null}
        <div className="flex items-end gap-2 rounded-2xl border border-line-strong bg-surface-2 p-2 transition-[border-color,box-shadow] focus-within:border-accent focus-within:shadow-[0_0_0_3px_rgb(34_211_238/.15)]">
          <textarea
            ref={ref}
            rows={1}
            value={text}
            maxLength={8000}
            onChange={(e) => setText(e.target.value)}
            onKeyDown={onKey}
            placeholder="Message the assistant…"
            aria-label="Message"
            className="max-h-[200px] min-h-[40px] flex-1 resize-none bg-transparent px-2 py-2 text-[14.5px] text-text placeholder:text-text-3 focus:outline-none"
          />
          <Button variant="primary" size="md" aria-label="Send" disabled={!text.trim()} onClick={go} className="size-10 rounded-xl px-0">
            <ArrowUp className="size-5" />
          </Button>
        </div>
        {!compact ? <p className="mt-1.5 text-center text-[11px] text-text-3">Enter to send · Shift+Enter for a new line</p> : null}
      </div>
    </div>
  );
}
