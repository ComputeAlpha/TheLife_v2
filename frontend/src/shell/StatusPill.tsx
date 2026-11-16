import * as P from "@radix-ui/react-popover";
import { useLlmStatus } from "@/api/system";
import { cn } from "@/ui/cn";
import { relative } from "@/lib/time";

// Assistant availability from /system/llm-status (frontend_lld.md §4).
export function StatusPill({ compact }: { compact?: boolean }) {
  const { data, isError } = useLlmStatus();
  const mode = isError ? "unknown" : (data?.mode ?? "loading");
  const look = {
    online: { dot: "bg-accent animate-pulse-ring", text: "Assistant online" },
    manual: { dot: "bg-warn", text: "Assistant offline" },
    unconfigured: { dot: "bg-text-3", text: "No assistant configured" },
    unknown: { dot: "bg-alert", text: "Status unavailable" },
    loading: { dot: "bg-text-3", text: "Checking assistant…" },
  }[mode];
  return (
    <P.Root>
      <P.Trigger
        className={cn(
          "inline-flex h-8 items-center gap-2 rounded-full border border-line bg-surface-1 px-3 text-[12.5px] text-text-2",
          "hover:border-line-strong hover:text-text transition-colors",
        )}
        aria-label={look.text}
      >
        <span className={cn("size-2 rounded-full", look.dot)} aria-hidden />
        {compact ? null : <span className="hidden sm:inline">{look.text}</span>}
      </P.Trigger>
      <P.Portal>
        <P.Content
          align="end"
          sideOffset={8}
          className="z-50 w-80 rounded-xl border border-line-strong bg-surface-1/95 p-4 text-sm shadow-2xl backdrop-blur-md data-[state=open]:animate-[fade-in_150ms_ease-out]"
        >
          <p className="font-medium text-text">{look.text}</p>
          {data?.mode === "manual" ? (
            <p className="mt-1 text-text-2">
              Chat answers built-in commands only{data.resets_at ? `; back ${relative(data.resets_at)}` : ""}. Reminders still arrive.
            </p>
          ) : null}
          {data?.providers?.length ? (
            <ul className="mt-3 space-y-2">
              {data.providers.map((p) => (
                <li key={p.name} className="flex items-center justify-between gap-2 rounded-lg bg-surface-2 px-3 py-2">
                  <div className="min-w-0">
                    <p className="truncate text-text">{p.model ?? p.name}</p>
                    <p className="text-[12px] text-text-3">{p.role}</p>
                  </div>
                  <span className={cn("text-[12px]", p.state === "healthy" ? "text-good" : p.state === "disabled" ? "text-text-3" : "text-warn")}>
                    {p.state}
                  </span>
                </li>
              ))}
            </ul>
          ) : null}
          {data?.usage ? (
            <p className="mt-3 text-[12px] text-text-3 tabular">
              Today: {data.usage.requests ?? 0} requests · {data.usage.images ?? 0} images
            </p>
          ) : null}
        </P.Content>
      </P.Portal>
    </P.Root>
  );
}
