import type { HTMLAttributes, ReactNode } from "react";
import { cn } from "./cn";

export function Card({ className, ...rest }: HTMLAttributes<HTMLDivElement>) {
  return (
    <div
      className={cn("rounded-[var(--radius-card)] border border-line bg-surface-1/80 backdrop-blur-sm", className)}
      {...rest}
    />
  );
}

export function CardHeader({ title, subtitle, action, icon }: { title: ReactNode; subtitle?: ReactNode; action?: ReactNode; icon?: ReactNode }) {
  return (
    <div className="flex items-start justify-between gap-3 px-5 pt-4 pb-3">
      <div className="flex items-center gap-2.5 min-w-0">
        {icon ? <span className="text-accent shrink-0">{icon}</span> : null}
        <div className="min-w-0">
          <h2 className="text-[15px] font-semibold text-text truncate">{title}</h2>
          {subtitle ? <p className="text-[12.5px] text-text-3 mt-0.5">{subtitle}</p> : null}
        </div>
      </div>
      {action}
    </div>
  );
}
