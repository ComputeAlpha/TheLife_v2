import { Bot, Download, KeyRound } from "lucide-react";
import { Badge } from "@/ui/Badge";

/** Who made a row, when it wasn't you directly. */
export function OriginBadge({ row }: { row: { created_by: { kind: string }; origin: string } }) {
  if (row.created_by.kind === "agent") return <Badge tone="accent"><Bot className="size-3" aria-hidden /> Assistant</Badge>;
  if (row.created_by.kind === "api_key") return <Badge><KeyRound className="size-3" aria-hidden /> API</Badge>;
  if (row.origin === "imported") return <Badge><Download className="size-3" aria-hidden /> Imported</Badge>;
  return null;
}
