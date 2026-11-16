import { NavLink, Outlet } from "react-router";
import { KeyRound, UserRound } from "lucide-react";
import { Page } from "../common/Page";
import { cn } from "@/ui/cn";

const TABS = [
  { to: "/settings/profile", label: "Profile", icon: UserRound },
  { to: "/settings/security", label: "Security", icon: KeyRound },
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
          <Outlet />
        </div>
      </div>
    </Page>
  );
}
