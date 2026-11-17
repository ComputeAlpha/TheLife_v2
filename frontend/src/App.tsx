import { lazy } from "react";
import { Navigate, Route, Routes } from "react-router";
import { AppShell } from "./shell/AppShell";
import { LoginPage } from "./features/auth/LoginPage";
import { RequireAuth } from "./features/auth/RequireAuth";
import { TodayPage } from "./features/today/TodayPage";
import { SettingsLayout } from "./features/settings/SettingsLayout";
import { ProfileSettings } from "./features/settings/ProfileSettings";
import { SecuritySettings } from "./features/settings/SecuritySettings";
import { Page } from "./features/common/Page";
import { EmptyState } from "./ui/States";
import { Card } from "./ui/Card";

// Route-level code splitting: each big screen is its own chunk (the shell and Today load first).
const CalendarRoute = lazy(() => import("./features/calendar/CalendarPage").then((m) => ({ default: m.CalendarRoute })));
const ChatPage = lazy(() => import("./features/chat/ChatPage").then((m) => ({ default: m.ChatPage })));
const ApprovalsPage = lazy(() => import("./features/approvals/ApprovalsPage").then((m) => ({ default: m.ApprovalsPage })));
const ActivityRoute = lazy(() => import("./features/activity/ActivityPage").then((m) => ({ default: m.ActivityRoute })));
const ScoresRoute = lazy(() => import("./features/scores/ScoresPage").then((m) => ({ default: m.ScoresRoute })));
const PlanPage = lazy(() => import("./features/plan/PlanPage").then((m) => ({ default: m.PlanPage })));

const TasksPage = lazy(() => import("./features/tasks/TasksPage").then((m) => ({ default: m.TasksPage })));
const SkillsPage = lazy(() => import("./features/skills/SkillsPage").then((m) => ({ default: m.SkillsPage })));
const ResourcePage = lazy(() => import("./features/skills/ResourcePage").then((m) => ({ default: m.ResourcePage })));
const WorkPage = lazy(() => import("./features/work/WorkPage").then((m) => ({ default: m.WorkPage })));
const ProjectPage = lazy(() => import("./features/work/ProjectPage").then((m) => ({ default: m.ProjectPage })));
const EntertainmentPage = lazy(() => import("./features/entertainment/EntertainmentPage").then((m) => ({ default: m.EntertainmentPage })));
const AssistantLayout = lazy(() => import("./features/assistant/AssistantLayout").then((m) => ({ default: m.AssistantLayout })));
const RoutinesPage = lazy(() => import("./features/assistant/RoutinesPage").then((m) => ({ default: m.RoutinesPage })));
const RemindersPage = lazy(() => import("./features/assistant/RemindersPage").then((m) => ({ default: m.RemindersPage })));
const MemoryPage = lazy(() => import("./features/assistant/MemoryPage").then((m) => ({ default: m.MemoryPage })));
const ConnectionsSettings = lazy(() => import("./features/settings/ConnectionsSettings").then((m) => ({ default: m.ConnectionsSettings })));
const TrackingSettings = lazy(() => import("./features/settings/TrackingSettings").then((m) => ({ default: m.TrackingSettings })));
const DaysSettings = lazy(() => import("./features/settings/DaysSettings").then((m) => ({ default: m.DaysSettings })));
const NotificationsSettings = lazy(() => import("./features/settings/MoreSettings").then((m) => ({ default: m.NotificationsSettings })));
const AssistantSettings = lazy(() => import("./features/settings/MoreSettings").then((m) => ({ default: m.AssistantSettings })));
const AboutSettings = lazy(() => import("./features/settings/MoreSettings").then((m) => ({ default: m.AboutSettings })));

export function App() {
  return (
    <Routes>
      <Route path="/login" element={<LoginPage />} />
      <Route
        element={
          <RequireAuth>
            <AppShell />
          </RequireAuth>
        }
      >
        <Route index element={<TodayPage />} />
        <Route path="calendar" element={<CalendarRoute />} />
        <Route path="calendar/:view" element={<CalendarRoute />} />
        <Route path="calendar/:view/:date" element={<CalendarRoute />} />
        <Route path="chat" element={<ChatPage />} />
        <Route path="chat/c/:id" element={<ChatPage />} />
        <Route path="approvals" element={<ApprovalsPage />} />
        <Route path="activity" element={<ActivityRoute />} />
        <Route path="activity/:date" element={<ActivityRoute />} />
        <Route path="scores" element={<ScoresRoute />} />
        <Route path="scores/:period" element={<ScoresRoute />} />
        <Route path="scores/:period/:date" element={<ScoresRoute />} />
        <Route path="plan" element={<PlanPage />} />
        <Route path="tasks" element={<TasksPage />} />
        <Route path="skills" element={<SkillsPage />} />
        <Route path="skills/:id" element={<ResourcePage />} />
        <Route path="work" element={<WorkPage />} />
        <Route path="work/projects/:id" element={<ProjectPage />} />
        <Route path="entertainment" element={<EntertainmentPage />} />
        <Route path="assistant" element={<AssistantLayout />}>
          <Route index element={<Navigate to="routines" replace />} />
          <Route path="routines" element={<RoutinesPage />} />
          <Route path="reminders" element={<RemindersPage />} />
          <Route path="memory" element={<MemoryPage />} />
        </Route>
        <Route path="settings" element={<SettingsLayout />}>
          <Route index element={<Navigate to="profile" replace />} />
          <Route path="profile" element={<ProfileSettings />} />
          <Route path="security" element={<SecuritySettings />} />
          <Route path="connections" element={<ConnectionsSettings />} />
          <Route path="tracking" element={<TrackingSettings />} />
          <Route path="days" element={<DaysSettings />} />
          <Route path="notifications" element={<NotificationsSettings />} />
          <Route path="assistant" element={<AssistantSettings />} />
          <Route path="about" element={<AboutSettings />} />
        </Route>
        <Route
          path="*"
          element={
            <Page title="Not found">
              <Card>
                <EmptyState title="Nothing lives here" body="The page you followed doesn't exist." />
              </Card>
            </Page>
          }
        />
      </Route>
    </Routes>
  );
}
