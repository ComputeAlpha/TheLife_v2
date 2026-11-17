import { Suspense } from "react";
import { NavLink, Outlet } from "react-router";
import { Skeleton } from "@/ui/States";
import { BellRing, Brain, Sunrise } from "lucide-react";
import { cn } from "@/ui/cn";
import { Page } from "../common/Page";

const TABS = [
  { to: "/assistant/routines", label: "Routines", icon: Sunrise },
  { to: "/assistant/reminders", label: "Reminders", icon: BellRing },
  { to: "/assistant/memory", label: "Memory", icon: Brain },
];

export function AssistantLayout() {
  return (
    <Page wide title="Assistant" subtitle="What the assistant does on a rhythm, what it reminds you of, and what it remembers about you.">
      <nav aria-label="Assistant sections" className="mb-6">
        <ul className="flex w-fit max-w-full gap-1 overflow-x-auto rounded-xl border border-line p-1">
          {TABS.map((t) => (
            <li key={t.to}>
              <NavLink to={t.to} className={({ isActive }) => cn("flex items-center gap-2 whitespace-nowrap rounded-lg px-4 py-1.5 text-sm transition-colors",
                isActive ? "bg-accent/12 text-text shadow-[inset_0_0_0_1px_rgb(34_211_238/.4)]" : "text-text-2 hover:text-text")}>
                {({ isActive }) => <><t.icon className={cn("size-4", isActive ? "text-accent" : "text-text-3")} aria-hidden />{t.label}</>}
              </NavLink>
            </li>
          ))}
        </ul>
      </nav>
      <Suspense fallback={<Skeleton className="h-64" />}><Outlet /></Suspense>
    </Page>
  );
}
