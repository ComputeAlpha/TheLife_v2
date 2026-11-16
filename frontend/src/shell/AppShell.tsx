import { useState, type ReactNode } from "react";
import { NavLink, Outlet, useLocation } from "react-router";
import * as DM from "@radix-ui/react-dropdown-menu";
import { AnimatePresence, motion } from "motion/react";
import { ChevronsLeft, ChevronsRight, LogOut, MoreHorizontal, Search, Settings } from "lucide-react";
import { NAV, SECTION_LABEL, type NavItem } from "./nav";
import { StatusPill } from "./StatusPill";
import { CommandPalette } from "./CommandPalette";
import { useSignOut, useUser } from "@/api/auth";
import { Kbd } from "@/ui/Badge";
import { cn } from "@/ui/cn";
import { Logo } from "./Logo";

const COLLAPSE_KEY = "thelife.sidebar.collapsed";
function readCollapsed(): boolean {
  try {
    return localStorage.getItem(COLLAPSE_KEY) === "1";
  } catch {
    return false;
  }
}

export function AppShell() {
  const [collapsed, setCollapsed] = useState(readCollapsed);
  const [palette, setPalette] = useState(false);
  const location = useLocation();
  const toggle = () => {
    setCollapsed((c) => {
      try {
        localStorage.setItem(COLLAPSE_KEY, c ? "0" : "1");
      } catch {
        /* per-viewer convenience only */
      }
      return !c;
    });
  };

  return (
    <div className="flex min-h-dvh bg-bg">
      <a href="#main" className="sr-only focus:not-sr-only focus:fixed focus:left-3 focus:top-3 focus:z-[60] focus:rounded-md focus:bg-accent focus:px-3 focus:py-2 focus:text-accent-ink">
        Skip to content
      </a>
      <aside
        className={cn(
          "sticky top-0 hidden h-dvh shrink-0 flex-col border-r border-line bg-surface-1/60 transition-[width] duration-200 lg:flex",
          collapsed ? "w-[68px]" : "w-60",
        )}
        aria-label="Main navigation"
      >
        <div className={cn("flex h-16 items-center gap-2.5 px-4", collapsed && "justify-center px-0")}>
          <Logo className="size-8 shrink-0" />
          {collapsed ? null : <span className="font-display text-[17px] font-semibold tracking-tight">TheLife</span>}
        </div>
        <nav className="flex-1 overflow-y-auto px-3 pb-3">
          {(["main", "life", "system"] as const).map((section) => (
            <div key={section} className="mt-2 first:mt-0">
              {SECTION_LABEL[section] && !collapsed ? (
                <p className="px-3 pb-1 pt-4 text-[11px] font-semibold uppercase tracking-wider text-text-3">{SECTION_LABEL[section]}</p>
              ) : section !== "main" ? (
                <div className="mx-3 my-3 h-px bg-line" />
              ) : null}
              <ul className="space-y-0.5">
                {NAV.filter((n) => n.section === section).map((n) => (
                  <li key={n.to}>
                    <SideLink item={n} collapsed={collapsed} />
                  </li>
                ))}
              </ul>
            </div>
          ))}
        </nav>
        <div className="border-t border-line p-3">
          <button
            onClick={toggle}
            className="flex w-full items-center justify-center gap-2 rounded-lg px-3 py-2 text-[13px] text-text-3 hover:bg-surface-2 hover:text-text"
            aria-label={collapsed ? "Expand sidebar" : "Collapse sidebar"}
          >
            {collapsed ? <ChevronsRight className="size-4" /> : <><ChevronsLeft className="size-4" /> <span className="flex-1 text-left">Collapse</span></>}
          </button>
        </div>
      </aside>

      <div className="flex min-w-0 flex-1 flex-col">
        <TopBar onPalette={() => setPalette(true)} />
        <main id="main" className="flex-1 pb-24 lg:pb-8" tabIndex={-1}>
          <AnimatePresence mode="wait" initial={false}>
            <motion.div
              key={location.pathname.split("/")[1] ?? ""}
              initial={{ opacity: 0, y: 6 }}
              animate={{ opacity: 1, y: 0 }}
              exit={{ opacity: 0 }}
              transition={{ duration: 0.18, ease: [0.22, 1, 0.36, 1] }}
            >
              <Outlet />
            </motion.div>
          </AnimatePresence>
        </main>
      </div>

      <MobileNav />
      <CommandPalette open={palette} onOpenChange={setPalette} />
    </div>
  );
}

function SideLink({ item, collapsed }: { item: NavItem; collapsed: boolean }) {
  return (
    <NavLink
      to={item.to}
      end={item.to === "/"}
      title={collapsed ? item.label : undefined}
      className={({ isActive }) =>
        cn(
          "group relative flex h-9 items-center gap-3 rounded-lg px-3 text-[13.5px] font-medium transition-colors",
          collapsed && "justify-center px-0",
          isActive ? "bg-accent/10 text-text" : "text-text-2 hover:bg-surface-2 hover:text-text",
        )
      }
    >
      {({ isActive }) => (
        <>
          {isActive ? (
            <motion.span
              layoutId="nav-active"
              className="absolute inset-y-1.5 left-0 w-[3px] rounded-full bg-accent shadow-[0_0_10px_rgb(34_211_238/.8)]"
              transition={{ type: "spring", stiffness: 500, damping: 38 }}
            />
          ) : null}
          <item.icon className={cn("size-[18px] shrink-0", isActive ? "text-accent" : "text-text-3 group-hover:text-text-2")} aria-hidden />
          {collapsed ? <span className="sr-only">{item.label}</span> : <span className="truncate">{item.label}</span>}
        </>
      )}
    </NavLink>
  );
}

function TopBar({ onPalette }: { onPalette: () => void }) {
  return (
    <header className="sticky top-0 z-30 flex h-16 items-center gap-3 border-b border-line bg-bg/80 px-4 backdrop-blur-xl sm:px-6">
      <Logo className="size-7 lg:hidden" />
      <button
        onClick={onPalette}
        className="flex h-9 min-w-0 flex-1 items-center gap-2.5 rounded-lg border border-line bg-surface-1 px-3 text-left text-[13px] text-text-3 transition-colors hover:border-line-strong hover:text-text-2 sm:max-w-md"
      >
        <Search className="size-4 shrink-0" aria-hidden />
        <span className="flex-1 truncate">Search or ask…</span>
        <span className="hidden gap-1 sm:flex">
          <Kbd>Ctrl</Kbd>
          <Kbd>K</Kbd>
        </span>
      </button>
      <div className="ml-auto flex items-center gap-2">
        <StatusPill />
        <UserMenu />
      </div>
    </header>
  );
}

function UserMenu() {
  const user = useUser();
  const signOut = useSignOut();
  const initials = user.display_name
    .split(/\s+/)
    .map((p) => p[0])
    .join("")
    .slice(0, 2)
    .toUpperCase();
  return (
    <DM.Root>
      <DM.Trigger
        className="grid size-9 place-items-center rounded-full bg-gradient-to-br from-accent/30 to-accent/5 text-[13px] font-semibold text-accent ring-1 ring-accent/30 hover:ring-accent/60"
      >
        <span aria-hidden>{initials || "?"}</span>
        <span className="sr-only">Account: {user.display_name}</span>
      </DM.Trigger>
      <DM.Portal>
        <DM.Content align="end" sideOffset={8} className="z-50 min-w-56 rounded-xl border border-line-strong bg-surface-1/95 p-1.5 shadow-2xl backdrop-blur-md">
          <div className="px-3 py-2">
            <p className="text-sm font-medium text-text">{user.display_name}</p>
            <p className="truncate text-[12px] text-text-3">{user.email}</p>
          </div>
          <DM.Separator className="my-1 h-px bg-line" />
          <MenuLink to="/settings" icon={<Settings className="size-4" />}>Settings</MenuLink>
          <DM.Item
            onSelect={() => signOut.mutate()}
            className="flex cursor-pointer items-center gap-2.5 rounded-lg px-3 py-2 text-sm text-text-2 outline-none data-[highlighted]:bg-surface-2 data-[highlighted]:text-text"
          >
            <LogOut className="size-4" /> Sign out
          </DM.Item>
        </DM.Content>
      </DM.Portal>
    </DM.Root>
  );
}

function MenuLink({ to, icon, children }: { to: string; icon: ReactNode; children: ReactNode }) {
  return (
    <DM.Item asChild>
      <NavLink to={to} className="flex items-center gap-2.5 rounded-lg px-3 py-2 text-sm text-text-2 outline-none data-[highlighted]:bg-surface-2 data-[highlighted]:text-text">
        {icon}
        {children}
      </NavLink>
    </DM.Item>
  );
}

function MobileNav() {
  const items = NAV.filter((n) => n.mobile);
  return (
    <nav className="fixed inset-x-0 bottom-0 z-40 border-t border-line bg-bg/90 pb-[env(safe-area-inset-bottom)] backdrop-blur-xl lg:hidden" aria-label="Main navigation">
      <ul className="grid grid-cols-5">
        {items.map((n) => (
          <li key={n.to}>
            <NavLink
              to={n.to}
              end={n.to === "/"}
              className={({ isActive }) => cn("flex flex-col items-center gap-1 py-2.5 text-[11px]", isActive ? "text-accent" : "text-text-3")}
            >
              <n.icon className="size-5" aria-hidden />
              {n.label}
            </NavLink>
          </li>
        ))}
        <li>
          <MoreMenu />
        </li>
      </ul>
    </nav>
  );
}

function MoreMenu() {
  const rest = NAV.filter((n) => !n.mobile);
  return (
    <DM.Root>
      <DM.Trigger className="flex w-full flex-col items-center gap-1 py-2.5 text-[11px] text-text-3">
        <MoreHorizontal className="size-5" aria-hidden />
        More
      </DM.Trigger>
      <DM.Portal>
        <DM.Content side="top" align="end" sideOffset={8} className="z-50 min-w-52 rounded-xl border border-line-strong bg-surface-1/95 p-1.5 shadow-2xl backdrop-blur-md">
          {rest.map((n) => (
            <MenuLink key={n.to} to={n.to} icon={<n.icon className="size-4" />}>
              {n.label}
            </MenuLink>
          ))}
        </DM.Content>
      </DM.Portal>
    </DM.Root>
  );
}
