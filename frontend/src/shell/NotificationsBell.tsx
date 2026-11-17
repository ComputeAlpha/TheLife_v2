import * as P from "@radix-ui/react-popover";
import { useState } from "react";
import { useNavigate } from "react-router";
import { Bell, BellRing, Check } from "lucide-react";
import { notificationHref, useAckNotification, useOpenNotifications, useRecentNotifications } from "@/api/notifications";
import { relative } from "@/lib/time";
import { useNow } from "@/lib/useNow";
import { Button } from "@/ui/Button";
import { cn } from "@/ui/cn";

export function NotificationsBell() {
  const open = useOpenNotifications();
  const [show, setShow] = useState(false);
  const recent = useRecentNotifications(show);
  const ack = useAckNotification();
  const navigate = useNavigate();
  const now = useNow();
  const count = open.data?.items.length ?? 0;
  const list = recent.data?.items ?? open.data?.items ?? [];
  const [perm, setPerm] = useState(() => ("Notification" in window ? Notification.permission : "denied"));
  return (
    <P.Root open={show} onOpenChange={setShow}>
      <P.Trigger className="relative grid size-9 place-items-center rounded-full text-text-2 hover:bg-surface-2 hover:text-text" aria-label={count ? `Notifications, ${count} new` : "Notifications"}>
        <Bell className="size-[18px]" />
        {count ? (
          <span className="absolute right-1 top-1 grid min-w-4 place-items-center rounded-full bg-accent px-1 text-[10px] font-bold leading-4 text-accent-ink shadow-[0_0_8px_var(--color-accent)]">
            {count > 9 ? "9+" : count}
          </span>
        ) : null}
      </P.Trigger>
      <P.Portal>
        <P.Content align="end" sideOffset={8} className="z-50 w-[min(380px,calc(100vw-1.5rem))] rounded-xl border border-line-strong bg-bg shadow-2xl">
          <div className="flex items-center justify-between border-b border-line px-4 py-3">
            <p className="text-sm font-semibold">Notifications</p>
            {count ? (
              <button className="text-[12.5px] text-text-2 hover:text-accent" onClick={() => open.data?.items.forEach((n) => ack.mutate({ id: n.id, clicked: false }))}>
                Mark all read
              </button>
            ) : null}
          </div>
          <ul className="max-h-[60vh] overflow-y-auto p-1.5">
            {list.map((n) => {
              const unread = !n.resolved_at;
              return (
                <li key={n.id}>
                  <button
                    onClick={() => {
                      if (unread) ack.mutate({ id: n.id, clicked: true });
                      setShow(false);
                      navigate(notificationHref(n));
                    }}
                    className="flex w-full items-start gap-3 rounded-lg px-3 py-2.5 text-left hover:bg-surface-2"
                  >
                    <span className={cn("mt-1.5 size-2 shrink-0 rounded-full", unread ? "bg-accent shadow-[0_0_6px_var(--color-accent)]" : "bg-transparent")} aria-hidden />
                    <span className="min-w-0 flex-1">
                      <span className={cn("block text-sm", unread ? "text-text" : "text-text-2")}>{n.title}</span>
                      <span className="line-clamp-2 text-[12.5px] text-text-3">{n.body}</span>
                      <span className="text-[11px] text-text-3">{relative(n.created_at, now)}</span>
                    </span>
                    {unread ? (
                      <span
                        role="button"
                        tabIndex={0}
                        aria-label="Mark read"
                        onClick={(e) => { e.stopPropagation(); ack.mutate({ id: n.id, clicked: false }); }}
                        onKeyDown={(e) => { if (e.key === "Enter") { e.stopPropagation(); ack.mutate({ id: n.id, clicked: false }); } }}
                        className="grid size-7 place-items-center rounded-md text-text-3 hover:bg-surface-3 hover:text-good"
                      >
                        <Check className="size-3.5" />
                      </span>
                    ) : null}
                  </button>
                </li>
              );
            })}
            {!list.length ? <li className="px-4 py-8 text-center text-[13px] text-text-3">You're all caught up.</li> : null}
          </ul>
          {perm === "default" ? (
            <div className="flex items-center gap-3 border-t border-line px-4 py-3">
              <BellRing className="size-4 shrink-0 text-accent" />
              <p className="flex-1 text-[12.5px] text-text-2">Get alerts in this browser when the tab is in the background.</p>
              <Button size="sm" onClick={() => Notification.requestPermission().then(setPerm)}>Enable</Button>
            </div>
          ) : null}
        </P.Content>
      </P.Portal>
    </P.Root>
  );
}
