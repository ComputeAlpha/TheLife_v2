import * as T from "@radix-ui/react-toast";
import { useSyncExternalStore, type ReactNode } from "react";
import { CheckCircle2, Info, TriangleAlert, X } from "lucide-react";
import { cn } from "./cn";

type Tone = "info" | "good" | "alert";
interface ToastItem {
  id: number;
  tone: Tone;
  title: ReactNode;
  body?: ReactNode;
  action?: { label: string; onClick: () => void };
}

let items: ToastItem[] = [];
let seq = 0;
const listeners = new Set<() => void>();
const emit = () => listeners.forEach((l) => l());

/** Shows a toast from anywhere (no hook needed). */
export function toast(title: ReactNode, opts: Omit<Partial<ToastItem>, "id" | "title"> = {}) {
  items = [...items.slice(-3), { id: ++seq, tone: opts.tone ?? "info", title, body: opts.body, action: opts.action }];
  emit();
}
toast.error = (title: ReactNode, body?: ReactNode) => toast(title, { tone: "alert", body });
toast.success = (title: ReactNode, body?: ReactNode) => toast(title, { tone: "good", body });

function remove(id: number) {
  items = items.filter((t) => t.id !== id);
  emit();
}

const icons = {
  info: <Info className="size-4 text-accent" />,
  good: <CheckCircle2 className="size-4 text-good" />,
  alert: <TriangleAlert className="size-4 text-alert-text" />,
};

export function Toaster() {
  const list = useSyncExternalStore(
    (cb) => (listeners.add(cb), () => listeners.delete(cb)),
    () => items,
  );
  return (
    <T.Provider swipeDirection="right" duration={5000}>
      {list.map((t) => (
        <T.Root
          key={t.id}
          onOpenChange={(open) => !open && remove(t.id)}
          className={cn(
            "group pointer-events-auto flex w-full items-start gap-3 rounded-[var(--radius-card)] border bg-surface-2/95 p-3.5 pr-9 shadow-2xl backdrop-blur-md",
            "data-[state=open]:animate-[toast-in_220ms_var(--ease-out-soft)] data-[state=closed]:opacity-0 transition-opacity",
            t.tone === "alert" ? "border-alert/40" : t.tone === "good" ? "border-good/30" : "border-line-strong",
          )}
        >
          <span className="mt-0.5">{icons[t.tone]}</span>
          <div className="min-w-0 flex-1">
            <T.Title className="text-sm font-medium text-text">{t.title}</T.Title>
            {t.body ? <T.Description className="mt-0.5 text-[13px] text-text-2">{t.body}</T.Description> : null}
            {t.action ? (
              <T.Action altText={t.action.label} asChild>
                <button onClick={t.action.onClick} className="mt-2 text-[13px] font-medium text-accent hover:text-accent-strong">
                  {t.action.label}
                </button>
              </T.Action>
            ) : null}
          </div>
          <T.Close aria-label="Dismiss" className="absolute right-2.5 top-2.5 rounded p-1 text-text-3 hover:text-text">
            <X className="size-3.5" />
          </T.Close>
        </T.Root>
      ))}
      <T.Viewport className="fixed bottom-4 right-4 z-[100] flex w-[min(380px,calc(100vw-2rem))] flex-col gap-2 outline-none" />
    </T.Provider>
  );
}
