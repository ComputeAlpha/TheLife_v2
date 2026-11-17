import { Command } from "cmdk";
import * as D from "@radix-ui/react-dialog";
import { useEffect, useState } from "react";
import { useNavigate } from "react-router";
import { BookOpen, CalendarDays, Flag, ListTodo, MessageSquare, Search, LogOut } from "lucide-react";
import { NAV } from "./nav";
import { Kbd } from "@/ui/Badge";
import { useMe, useSignOut } from "@/api/auth";
import { useSearchIndex, type Hit } from "@/api/search";
import { dayOf } from "@/lib/calendar/days";

const FIND_ICON: Record<Hit["kind"], typeof Search> = { task: ListTodo, goal: Flag, resource: BookOpen, event: CalendarDays };

/** True when typing in a field, so single-key shortcuts stay out of the way. */
export function isTyping(e: KeyboardEvent): boolean {
  const t = e.target as HTMLElement | null;
  return !!t && (t.isContentEditable || ["INPUT", "TEXTAREA", "SELECT"].includes(t.tagName));
}

// Ctrl/⌘ K palette and the "g <key>" jumps (frontend_lld.md §4).
export function CommandPalette({ open, onOpenChange }: { open: boolean; onOpenChange: (o: boolean) => void }) {
  const navigate = useNavigate();
  const signOut = useSignOut();
  const [query, setQuery] = useState("");
  const user = useMe().data;
  // Loaded as the palette opens, so hits are there before Enter (cmdk keeps its first selection otherwise).
  const index = useSearchIndex(open, (iso) => dayOf(Date.parse(iso), user?.timezone ?? "UTC"));

  useEffect(() => {
    let pendingG = 0;
    const onKey = (e: KeyboardEvent) => {
      if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === "k") {
        e.preventDefault();
        onOpenChange(!open);
        return;
      }
      if (open || isTyping(e) || e.metaKey || e.ctrlKey || e.altKey) return;
      if (e.key === "g") {
        pendingG = Date.now();
        return;
      }
      if (Date.now() - pendingG < 1200) {
        const item = NAV.find((n) => n.key === e.key);
        pendingG = 0;
        if (item) {
          e.preventDefault();
          navigate(item.to);
        }
      }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [open, onOpenChange, navigate]);

  const go = (to: string) => {
    onOpenChange(false);
    setQuery("");
    navigate(to);
  };

  return (
    <D.Root open={open} onOpenChange={onOpenChange}>
      <D.Portal>
        <D.Overlay className="fixed inset-0 z-50 bg-black/70 backdrop-blur-[2px]" />
        <D.Content
          className="fixed left-1/2 top-[14vh] z-50 w-[min(600px,calc(100vw-2rem))] -translate-x-1/2 overflow-hidden rounded-2xl border border-line-strong bg-surface-1 shadow-[0_30px_80px_-20px_rgb(0_0_0/.9),0_0_40px_-10px_rgb(34_211_238/.25)] focus:outline-none"
          aria-describedby={undefined}
        >
          <D.Title className="sr-only">Command palette</D.Title>
          <Command label="Command palette" loop filter={paletteFilter}>
            <div className="flex items-center gap-3 border-b border-line px-4">
              <Search className="size-4 text-text-3" aria-hidden />
              <Command.Input
                value={query}
                onValueChange={setQuery}
                placeholder="Jump to, or ask the assistant…"
                className="h-14 flex-1 bg-transparent text-[15px] text-text placeholder:text-text-3 focus:outline-none"
              />
              <Kbd>Esc</Kbd>
            </div>
            <Command.List className="max-h-[50vh] overflow-y-auto p-2">
              <Command.Empty className="px-3 py-6 text-center text-sm text-text-3">No matches.</Command.Empty>
              <Command.Group heading="Go to" className={groupCls}>
                {NAV.map((n) => (
                  <Command.Item key={n.to} value={`go ${n.label}`} onSelect={() => go(n.to)} className={itemCls}>
                    <n.icon className="size-4 text-text-2" />
                    <span className="flex-1">{n.label}</span>
                    {n.key ? (
                      <span className="flex gap-1">
                        <Kbd>g</Kbd>
                        <Kbd>{n.key}</Kbd>
                      </span>
                    ) : null}
                  </Command.Item>
                ))}
              </Command.Group>
              {query.trim().length >= 2 ? (
                <Command.Group heading="Find" className={groupCls}>
                  {(index.data ?? []).map((h) => {
                    const I = FIND_ICON[h.kind];
                    return (
                      <Command.Item key={`${h.kind}:${h.id}`} value={`find ${h.kind} ${h.id}`} keywords={[h.title]} onSelect={() => go(h.to)} className={itemCls}>
                        <I className="size-4 text-text-2" />
                        <span className="flex-1 truncate">{h.title}</span>
                        <span className="text-[11.5px] capitalize text-text-3">{h.hint ?? h.kind}</span>
                      </Command.Item>
                    );
                  })}
                </Command.Group>
              ) : null}
              {/* "Ask" waits for the search index: shown earlier, cmdk would select it and Enter would skip real hits. */}
              {query.trim().length > 2 && (index.data || index.isError) ? (
                <Command.Group heading="Assistant" className={groupCls}>
                  <Command.Item
                    value={`ask ${query}`}
                    onSelect={() => go(`/chat?draft=${encodeURIComponent(query.trim())}`)}
                    className={itemCls}
                  >
                    <MessageSquare className="size-4 text-accent" />
                    <span className="truncate">Ask: “{query.trim()}”</span>
                  </Command.Item>
                </Command.Group>
              ) : null}
              <Command.Group heading="Account" className={groupCls}>
                <Command.Item value="sign out" onSelect={() => signOut.mutate()} className={itemCls}>
                  <LogOut className="size-4 text-text-2" />
                  Sign out
                </Command.Item>
              </Command.Group>
            </Command.List>
          </Command>
        </D.Content>
      </D.Portal>
    </D.Root>
  );
}

/** Substring beats in-order letters; "Ask" always ranks last so it never steals Enter from a page match. */
export function paletteFilter(value: string, search: string, keywords?: string[]): number {
  if (value.startsWith("ask ")) return 0.001;
  if (value.startsWith("find ")) {
    // Titles only (never ids), and no in-order-letter matches: long titles would match nearly anything.
    const t = (keywords ?? []).join(" ").toLowerCase();
    const q = search.toLowerCase().trim();
    return !q ? 0 : t.startsWith(q) ? 0.7 : t.includes(q) ? 0.5 : 0;
  }
  const v = value.toLowerCase().replace(/^go /, "");
  const s = search.toLowerCase().trim();
  if (!s) return 1;
  if (v.startsWith(s)) return 1;
  if (v.includes(s)) return 0.8;
  let i = 0;
  for (const ch of v) if (ch === s[i]) i++;
  return i === s.length ? 0.3 : 0;
}

const groupCls =
  "[&_[cmdk-group-heading]]:px-3 [&_[cmdk-group-heading]]:pb-1 [&_[cmdk-group-heading]]:pt-3 [&_[cmdk-group-heading]]:text-[11px] [&_[cmdk-group-heading]]:font-semibold [&_[cmdk-group-heading]]:uppercase [&_[cmdk-group-heading]]:tracking-wider [&_[cmdk-group-heading]]:text-text-3";
const itemCls =
  "flex cursor-pointer items-center gap-3 rounded-lg px-3 py-2.5 text-sm text-text-2 data-[selected=true]:bg-accent/10 data-[selected=true]:text-text";
