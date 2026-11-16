import { Page } from "./Page";
import { Card } from "@/ui/Card";
import { EmptyState } from "@/ui/States";

/** Screens of later increments (frontend_lld.md §10) until they land. */
export function ComingSoon({ title, increment, what }: { title: string; increment: string; what: string }) {
  return (
    <Page title={title}>
      <Card>
        <EmptyState title={`Arrives in increment ${increment}`} body={what} />
      </Card>
    </Page>
  );
}
