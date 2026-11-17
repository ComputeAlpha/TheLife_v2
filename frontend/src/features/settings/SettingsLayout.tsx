import { Suspense } from "react";
import { NavLink, Outlet } from "react-router";
import { Skeleton } from "@/ui/States";
import { Bell, Bot, CalendarCog, Info, KeyRound, Plug, Radar, UserRound } from "lucide-react";
import { Page } from "../common/Page";
import { cn } from "@/ui/cn";

const TABS = [
  { to: "/settings/profile", label: "Profile", icon: UserRound },
  { to: "/settings/security", label: "Security", icon: KeyRound },
  { to: "/settings/connections", label: "Connections", icon: Plug },
  { to: "/settings/tracking", label: "Tracking", icon: Radar },
  { to: "/settings/days", label: "Days & scoring", icon: CalendarCog },
  { to: "/settings/notifications", label: "Notifications", icon: Bell },
  { to: "/settings/assistant", label: "Assistant", icon: Bot },
  { to: "/settings/about", label: "About", icon: Info },
];

export function SettingsLayout() {
  return (
    <Page title="Settings" subtitle="Your profile, sign-in, connections and how days are scored.">
      <div className="flex flex-col gap-6 md:flex-row">
        <nav aria-label="Settings sections" className="md:w-52 md:shrink-0">
          <ul className="flex gap-1 overflow-x-auto md:flex-col">
            {TABS.map((t) => (
              <li key={t.to}>
                <NavLink
                  to={t.to}
                  className={({ isActive }) =>
                    cn(
                      "flex items-center gap-2.5 whitespace-nowrap rounded-lg px-3 py-2 text-sm transition-colors",
                      isActive ? "bg-surface-2 text-text" : "text-text-2 hover:bg-surface-1 hover:text-text",
                    )
                  }
                >
                  {({ isActive }) => (
                    <>
                      <t.icon className={cn("size-4", isActive ? "text-accent" : "text-text-3")} aria-hidden />
                      {t.label}
                    </>
                  )}
                </NavLink>
              </li>
            ))}
          </ul>
        </nav>
        <div className="min-w-0 flex-1">
          <Suspense fallback={<Skeleton className="h-64" />}><Outlet /></Suspense>
        </div>
      </div>
    </Page>
  );
}
