import {
  Bot, CalendarDays, CheckSquare, Gamepad2, GraduationCap, LayoutDashboard, MessageSquare, Settings, ShieldCheck,
  Briefcase, Target, Trophy, Activity, type LucideIcon,
} from "lucide-react";

export interface NavItem {
  to: string;
  label: string;
  icon: LucideIcon;
  /** "g <key>" jumps here. */
  key?: string;
  section: "main" | "life" | "system";
  mobile?: boolean;
}

export const NAV: NavItem[] = [
  { to: "/", label: "Today", icon: LayoutDashboard, key: "t", section: "main", mobile: true },
  { to: "/calendar", label: "Calendar", icon: CalendarDays, key: "c", section: "main", mobile: true },
  { to: "/chat", label: "Chat", icon: MessageSquare, key: "h", section: "main", mobile: true },
  { to: "/activity", label: "Activity", icon: Activity, key: "a", section: "main", mobile: true },
  { to: "/approvals", label: "Approvals", icon: ShieldCheck, key: "p", section: "main" },
  { to: "/plan", label: "Goals & plans", icon: Target, key: "g", section: "life" },
  { to: "/tasks", label: "Tasks", icon: CheckSquare, key: "k", section: "life" },
  { to: "/skills", label: "Skills", icon: GraduationCap, section: "life" },
  { to: "/work", label: "Work", icon: Briefcase, section: "life" },
  { to: "/entertainment", label: "Entertainment", icon: Gamepad2, section: "life" },
  { to: "/scores", label: "Scores", icon: Trophy, key: "s", section: "life" },
  { to: "/assistant", label: "Assistant", icon: Bot, section: "system" },
  { to: "/settings", label: "Settings", icon: Settings, key: ",", section: "system" },
];

export const SECTION_LABEL = { main: "", life: "Life", system: "System" } as const;
