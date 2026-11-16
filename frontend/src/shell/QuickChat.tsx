import * as D from "@radix-ui/react-dialog";
import { useState } from "react";
import { Link, useLocation } from "react-router";
import { Maximize2, Sparkles, X } from "lucide-react";
import { useUser } from "@/api/auth";
import { useTodayConversation } from "@/api/chat";
import { localDay } from "@/lib/time";
import { useNow } from "@/lib/useNow";
import { Thread } from "@/features/chat/Thread";

/** The floating "Ask" button: today's chat in a side sheet over any page (frontend_lld.md §4). */
export function QuickChat() {
  const [open, setOpen] = useState(false);
  const location = useLocation();
  const user = useUser();
  const now = useNow();
  const today = localDay(now, user.timezone, user.day_start_hour);
  const conv = useTodayConversation(today);
  if (location.pathname.startsWith("/chat")) return null;
  return (
    <D.Root open={open} onOpenChange={setOpen}>
      <D.Trigger
        className="fixed bottom-24 right-4 z-30 inline-flex h-12 items-center gap-2 rounded-full bg-accent px-5 font-semibold text-accent-ink shadow-[0_0_0_1px_rgb(34_211_238/.5),0_10px_40px_-6px_rgb(34_211_238/.8)] transition-transform hover:scale-105 lg:bottom-6 lg:right-6"
        aria-label="Ask the assistant"
      >
        <Sparkles className="size-5" /> <span className="hidden sm:inline">Ask</span>
      </D.Trigger>
      <D.Portal>
        <D.Overlay className="fixed inset-0 z-50 bg-black/60 backdrop-blur-[2px] data-[state=open]:animate-[fade-in_180ms_ease-out]" />
        <D.Content className="fixed inset-y-0 right-0 z-50 flex w-full max-w-[440px] flex-col border-l border-line-strong bg-bg data-[state=open]:animate-[sheet-in_240ms_var(--ease-out-soft)] focus:outline-none" aria-describedby={undefined}>
          <div className="flex items-center gap-2 border-b border-line px-4 py-3">
            <Sparkles className="size-4 text-accent" />
            <D.Title className="text-base font-semibold">Ask the assistant</D.Title>
            <Link to="/chat" onClick={() => setOpen(false)} className="ml-auto grid size-8 place-items-center rounded-md text-text-3 hover:bg-surface-2 hover:text-text" aria-label="Open full chat">
              <Maximize2 className="size-4" />
            </Link>
            <D.Close className="grid size-8 place-items-center rounded-md text-text-3 hover:bg-surface-2 hover:text-text" aria-label="Close"><X className="size-4" /></D.Close>
          </div>
          <div className="min-h-0 flex-1">
            <Thread conversationId={conv.data?.id ?? null} writable tz={user.timezone} compact loading={conv.isPending} />
          </div>
        </D.Content>
      </D.Portal>
    </D.Root>
  );
}
