import { useState, type ReactNode } from "react";
import { Check, Copy, TriangleAlert } from "lucide-react";
import { Button } from "@/ui/Button";
import { Dialog } from "@/ui/Dialog";

/** Shows a new secret exactly once (the server keeps only its hash). Closing drops it from memory. */
export function TokenOnce({ token, title, onDone, children }: { token: string | null; title: string; onDone: () => void; children?: ReactNode }) {
  const [copied, setCopied] = useState(false);
  return (
    <Dialog
      open={!!token}
      onOpenChange={(o) => { if (!o) { setCopied(false); onDone(); } }}
      title={title}
      description="Copy it now: it's shown once. If you lose it, revoke this one and make another."
      footer={<Button variant="primary" onClick={() => { setCopied(false); onDone(); }}>I've copied it</Button>}
    >
      <div className="space-y-4">
        <div className="flex items-start gap-2 rounded-xl border border-warn/30 bg-warn/[.06] p-3 text-[13px] text-text-2">
          <TriangleAlert className="mt-0.5 size-4 shrink-0 text-warn" aria-hidden />
          Anyone with this token can act as it. Don't paste it into chats or commit it.
        </div>
        <div className="flex items-stretch gap-2">
          <code aria-label="Token" className="min-w-0 flex-1 break-all rounded-lg border border-line bg-surface-2 px-3 py-2 font-mono text-[13px] text-text">{token}</code>
          <Button
            aria-label="Copy token"
            icon={copied ? <Check className="size-4 text-good" /> : <Copy className="size-4" />}
            onClick={() => token && navigator.clipboard?.writeText(token).then(() => setCopied(true), () => setCopied(false))}
          >
            {copied ? "Copied" : "Copy"}
          </Button>
        </div>
        {children}
      </div>
    </Dialog>
  );
}
