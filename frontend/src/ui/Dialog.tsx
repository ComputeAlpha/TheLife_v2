import * as D from "@radix-ui/react-dialog";
import type { ReactNode } from "react";
import { X } from "lucide-react";
import { cn } from "./cn";

interface Props {
  open: boolean;
  onOpenChange: (open: boolean) => void;
  title: ReactNode;
  description?: ReactNode;
  children: ReactNode;
  footer?: ReactNode;
  className?: string;
}

const overlay = "fixed inset-0 z-50 bg-black/70 backdrop-blur-[2px] data-[state=open]:animate-[fade-in_180ms_ease-out]";

export function Dialog({ open, onOpenChange, title, description, children, footer, className }: Props) {
  return (
    <D.Root open={open} onOpenChange={onOpenChange}>
      <D.Portal>
        <D.Overlay className={overlay} />
        <D.Content
          className={cn(
            "fixed left-1/2 top-1/2 z-50 w-[min(520px,calc(100vw-2rem))] -translate-x-1/2 -translate-y-1/2",
            "rounded-2xl border border-line-strong bg-surface-1 shadow-[0_30px_80px_-20px_rgb(0_0_0/.9),0_0_0_1px_rgb(34_211_238/.06)]",
            "data-[state=open]:animate-[pop-in_200ms_var(--ease-out-soft)] focus:outline-none",
            className,
          )}
        >
          <div className="flex items-start justify-between gap-4 px-6 pt-5">
            <div>
              <D.Title className="text-lg font-semibold">{title}</D.Title>
              {description ? <D.Description className="mt-1 text-sm text-text-2">{description}</D.Description> : null}
            </div>
            <D.Close aria-label="Close" className="rounded-md p-1.5 text-text-3 hover:bg-surface-2 hover:text-text">
              <X className="size-4" />
            </D.Close>
          </div>
          <div className="px-6 py-4">{children}</div>
          {footer ? <div className="flex justify-end gap-2 border-t border-line px-6 py-4">{footer}</div> : null}
        </D.Content>
      </D.Portal>
    </D.Root>
  );
}

/** Right-side panel for editors (full height on phones). */
export function Sheet({ open, onOpenChange, title, description, children, footer, className }: Props) {
  return (
    <D.Root open={open} onOpenChange={onOpenChange}>
      <D.Portal>
        <D.Overlay className={overlay} />
        <D.Content
          className={cn(
            "fixed inset-y-0 right-0 z-50 flex w-full max-w-[480px] flex-col border-l border-line-strong bg-surface-1",
            "data-[state=open]:animate-[sheet-in_240ms_var(--ease-out-soft)] focus:outline-none",
            className,
          )}
        >
          <div className="flex items-start justify-between gap-4 border-b border-line px-6 py-4">
            <div>
              <D.Title className="text-lg font-semibold">{title}</D.Title>
              {description ? <D.Description className="mt-1 text-sm text-text-2">{description}</D.Description> : null}
            </div>
            <D.Close aria-label="Close" className="rounded-md p-1.5 text-text-3 hover:bg-surface-2 hover:text-text">
              <X className="size-4" />
            </D.Close>
          </div>
          <div className="flex-1 overflow-y-auto px-6 py-5">{children}</div>
          {footer ? <div className="flex justify-end gap-2 border-t border-line px-6 py-4">{footer}</div> : null}
        </D.Content>
      </D.Portal>
    </D.Root>
  );
}
