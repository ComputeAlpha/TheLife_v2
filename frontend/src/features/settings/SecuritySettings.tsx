import { useState, type FormEvent } from "react";
import { ShieldCheck } from "lucide-react";
import { useChangePassword } from "@/api/auth";
import { ApiError } from "@/api/errors";
import { Button } from "@/ui/Button";
import { Card, CardHeader } from "@/ui/Card";
import { Field, Input } from "@/ui/Field";
import { toast } from "@/ui/toast";

const MIN = 12;

export function SecuritySettings() {
  const change = useChangePassword();
  const [current, setCurrent] = useState("");
  const [next, setNext] = useState("");
  const [repeat, setRepeat] = useState("");
  const serverErrors = change.error instanceof ApiError ? change.error.fieldErrors() : {};
  const mismatch = repeat.length > 0 && repeat !== next ? "The passwords don't match." : undefined;
  const short = next.length > 0 && next.length < MIN ? `Use at least ${MIN} characters.` : undefined;

  const submit = (e: FormEvent) => {
    e.preventDefault();
    if (mismatch || short || !current) return;
    change.mutate(
      { current_password: current, new_password: next },
      {
        onSuccess: () => {
          setCurrent("");
          setNext("");
          setRepeat("");
          toast.success("Password changed", "Other browsers and apps signed in as you were signed out.");
        },
      },
    );
  };

  return (
    <form onSubmit={submit}>
      <Card>
        <CardHeader title="Password" subtitle="Changing it signs out every other session." icon={<ShieldCheck className="size-4" />} />
        <div className="grid max-w-md gap-4 px-5 pb-5">
          <Field label="Current password" error={serverErrors.current_password}>
            {(p) => <Input {...p} type="password" autoComplete="current-password" value={current} onChange={(e) => setCurrent(e.target.value)} />}
          </Field>
          <Field label="New password" error={short ?? serverErrors.new_password} hint={`At least ${MIN} characters.`}>
            {(p) => <Input {...p} type="password" autoComplete="new-password" value={next} onChange={(e) => setNext(e.target.value)} />}
          </Field>
          <Field label="Repeat new password" error={mismatch}>
            {(p) => <Input {...p} type="password" autoComplete="new-password" value={repeat} onChange={(e) => setRepeat(e.target.value)} />}
          </Field>
          {change.error && !Object.keys(serverErrors).length ? (
            <p role="alert" className="text-[13px] text-alert-text">{(change.error as ApiError).summary}</p>
          ) : null}
          <div>
            <Button type="submit" variant="primary" loading={change.isPending}
              disabled={!current || !next || !repeat || !!mismatch || !!short}>
              Change password
            </Button>
          </div>
        </div>
      </Card>
    </form>
  );
}
