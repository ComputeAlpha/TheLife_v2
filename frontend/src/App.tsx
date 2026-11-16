import { Navigate, Route, Routes } from "react-router";
import { AppShell } from "./shell/AppShell";
import { LoginPage } from "./features/auth/LoginPage";
import { RequireAuth } from "./features/auth/RequireAuth";
import { TodayPage } from "./features/today/TodayPage";
import { SettingsLayout } from "./features/settings/SettingsLayout";
import { ProfileSettings } from "./features/settings/ProfileSettings";
import { SecuritySettings } from "./features/settings/SecuritySettings";
import { ComingSoon } from "./features/common/ComingSoon";
import { Page } from "./features/common/Page";
import { EmptyState } from "./ui/States";
import { Card } from "./ui/Card";

const LATER: { path: string; title: string; inc: string; what: string }[] = [
  { path: "calendar/*", title: "Calendar", inc: "5b", what: "Day, week, month and year views of everything planned." },
  { path: "chat/*", title: "Chat", inc: "5c", what: "Talk to the assistant, with live replies and approvals inline." },
  { path: "approvals", title: "Approvals", inc: "5c", what: "Changes the assistant is waiting for you to approve." },
  { path: "activity/*", title: "Activity", inc: "5d", what: "Review what was detected: confirm, dispute or correct." },
  { path: "scores/*", title: "Scores", inc: "5d", what: "Day, week and month scores with their reasons." },
  { path: "plan/*", title: "Goals & plans", inc: "5d", what: "Goals by horizon, plans and targets." },
  { path: "tasks/*", title: "Tasks", inc: "5e", what: "One-off and recurring tasks." },
  { path: "skills/*", title: "Skills", inc: "5e", what: "Books and courses with progress." },
  { path: "work/*", title: "Work", inc: "5e", what: "Roles, projects and deliverables." },
  { path: "entertainment/*", title: "Entertainment", inc: "5e", what: "Games, movies and series, planned not limited." },
  { path: "assistant/*", title: "Assistant", inc: "5e", what: "Routines, reminders and what the assistant remembers." },
];

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
        <Route path="settings" element={<SettingsLayout />}>
          <Route index element={<Navigate to="profile" replace />} />
          <Route path="profile" element={<ProfileSettings />} />
          <Route path="security" element={<SecuritySettings />} />
        </Route>
        {LATER.map((l) => (
          <Route key={l.path} path={l.path} element={<ComingSoon title={l.title} increment={l.inc} what={l.what} />} />
        ))}
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
