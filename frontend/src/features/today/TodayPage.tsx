import { useUser } from "@/api/auth";
import { fmt, greeting } from "@/lib/time";
import { useNow } from "@/lib/useNow";
import { Page } from "../common/Page";
import { Card } from "@/ui/Card";
import { EmptyState } from "@/ui/States";

export function TodayPage() {
  const user = useUser();
  const now = useNow();
  const first = user.display_name.split(/\s+/)[0];
  return (
    <Page title={`${greeting(user.timezone, now)}, ${first}`} subtitle={fmt(now, user.timezone, "EEEE, d MMMM")}>
      <Card>
        <EmptyState
          title="Your day at a glance arrives in increment 5b"
          body="Agenda, progress rings, approvals and detections to review will live here."
        />
      </Card>
    </Page>
  );
}
