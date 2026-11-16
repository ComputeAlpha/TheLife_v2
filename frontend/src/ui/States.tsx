import type { ReactNode } from "react";
import { AlertTriangle, RotateCw } from "lucide-react";
import { ApiError } from "@/api/errors";
import { Button } from "./Button";
import { cn } from "./cn";

export function Skeleton({ className }: { className?: string }) {
  return <div className={cn("skeleton rounded-md", className)} aria-hidden />;
}

export function EmptyState({ art, title, body, action, className }: { art?: ReactNode; title: ReactNode; body?: ReactNode; action?: ReactNode; className?: string }) {
  return (
    <div className={cn("flex flex-col items-center justify-center text-center gap-3 px-6 py-12", className)}>
      {art ?? <OrbitArt />}
      <h3 className="text-base font-semibold text-text">{title}</h3>
      {body ? <p className="max-w-sm text-sm text-text-2">{body}</p> : null}
      {action}
    </div>
  );
}

export function ErrorState({ error, retry, className }: { error: unknown; retry?: () => void; className?: string }) {
  const e = error instanceof ApiError ? error : null;
  return (
    <div role="alert" className={cn("flex flex-col items-center gap-3 px-6 py-10 text-center", className)}>
      <AlertTriangle className="size-7 text-alert-text" aria-hidden />
      <p className="text-sm text-text">{e ? e.summary : "Something went wrong."}</p>
      {e?.requestId ? <p className="font-mono text-[11px] text-text-3">request {e.requestId}</p> : null}
      {retry ? (
        <Button size="sm" icon={<RotateCw className="size-3.5" />} onClick={retry}>
          Try again
        </Button>
      ) : null}
    </div>
  );
}

/** Line-art used by empty states: an orbit with a cyan satellite. */
export function OrbitArt({ className }: { className?: string }) {
  return (
    <svg viewBox="0 0 120 80" className={cn("h-20 w-32", className)} aria-hidden>
      <ellipse cx="60" cy="40" rx="50" ry="22" fill="none" stroke="var(--color-line-strong)" strokeDasharray="3 5" />
      <ellipse cx="60" cy="40" rx="30" ry="13" fill="none" stroke="var(--color-line)" />
      <circle cx="60" cy="40" r="7" fill="none" stroke="var(--color-text-3)" />
      <circle cx="105" cy="34" r="4" fill="var(--color-accent)">
        <animate attributeName="opacity" values="1;.4;1" dur="2.4s" repeatCount="indefinite" />
      </circle>
    </svg>
  );
}
