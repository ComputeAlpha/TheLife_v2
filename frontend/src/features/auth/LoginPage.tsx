import { useState, type FormEvent } from "react";
import { Navigate, useNavigate, useSearchParams } from "react-router";
import { motion } from "motion/react";
import { ArrowRight, Lock, Mail } from "lucide-react";
import { useMe, useSignIn } from "@/api/auth";
import { ApiError } from "@/api/errors";
import { Button } from "@/ui/Button";
import { Field, Input } from "@/ui/Field";
import { Logo } from "@/shell/Logo";

/** Only same-origin paths are followed after sign-in. */
export function safeNext(next: string | null): string {
  if (!next || !next.startsWith("/") || next.startsWith("//") || next.startsWith("/\\")) return "/";
  return next;
}

export function LoginPage() {
  const [params] = useSearchParams();
  const next = safeNext(params.get("next"));
  const me = useMe();
  const signIn = useSignIn();
  const navigate = useNavigate();
  const [email, setEmail] = useState("");
  const [password, setPassword] = useState("");

  if (me.data) return <Navigate to={next} replace />;

  const submit = (e: FormEvent) => {
    e.preventDefault();
    signIn.mutate({ email: email.trim(), password }, { onSuccess: () => navigate(next, { replace: true }) });
  };
  const err = signIn.error instanceof ApiError ? signIn.error : null;
  const message = !err ? null : err.status === 401 ? "That email and password don't match." : err.summary;

  return (
    <div className="relative grid min-h-dvh place-items-center overflow-hidden bg-bg px-4">
      <div aria-hidden className="pointer-events-none absolute inset-0">
        <div className="absolute left-1/2 top-1/2 size-[720px] -translate-x-1/2 -translate-y-1/2 rounded-full bg-[radial-gradient(circle_at_30%_30%,rgb(34_211_238/.22),transparent_60%)] blur-3xl animate-drift" />
        <div className="absolute left-[60%] top-[30%] size-[420px] rounded-full bg-[radial-gradient(circle,rgb(219_39_119/.10),transparent_65%)] blur-3xl animate-drift [animation-delay:-6s]" />
        <svg className="absolute inset-0 h-full w-full opacity-[.06]">
          <defs>
            <pattern id="grid" width="40" height="40" patternUnits="userSpaceOnUse">
              <path d="M40 0H0V40" fill="none" stroke="#fff" strokeWidth=".5" />
            </pattern>
          </defs>
          <rect width="100%" height="100%" fill="url(#grid)" />
        </svg>
      </div>

      <motion.div
        initial={{ opacity: 0, y: 12, scale: 0.98 }}
        animate={{ opacity: 1, y: 0, scale: 1 }}
        transition={{ duration: 0.5, ease: [0.22, 1, 0.36, 1] }}
        className="relative w-full max-w-[400px]"
      >
        <div className="mb-8 flex flex-col items-center gap-3 text-center">
          <Logo className="size-14 drop-shadow-[0_0_18px_rgb(34_211_238/.55)]" />
          <h1 className="text-3xl font-semibold">TheLife</h1>
          <p className="text-sm text-text-2">Plan less by hand. Live more on plan.</p>
        </div>
        <form
          onSubmit={submit}
          className="rounded-2xl border border-line-strong bg-surface-1/80 p-6 shadow-[0_30px_80px_-30px_rgb(0_0_0/.9)] backdrop-blur-xl"
          noValidate
        >
          <div className="space-y-4">
            <Field label="Email">
              {(p) => (
                <div className="relative">
                  <Mail className="pointer-events-none absolute left-3 top-1/2 size-4 -translate-y-1/2 text-text-3" aria-hidden />
                  <Input {...p} type="email" autoComplete="username" required autoFocus value={email}
                    onChange={(e) => setEmail(e.target.value)} className="pl-9" placeholder="you@example.com" />
                </div>
              )}
            </Field>
            <Field label="Password">
              {(p) => (
                <div className="relative">
                  <Lock className="pointer-events-none absolute left-3 top-1/2 size-4 -translate-y-1/2 text-text-3" aria-hidden />
                  <Input {...p} type="password" autoComplete="current-password" required value={password}
                    onChange={(e) => setPassword(e.target.value)} className="pl-9" />
                </div>
              )}
            </Field>
            {message ? (
              <p role="alert" className="rounded-lg border border-alert/40 bg-alert/10 px-3 py-2 text-[13px] text-alert-text">
                {message}
              </p>
            ) : null}
            <Button type="submit" variant="primary" size="lg" className="w-full" loading={signIn.isPending}
              disabled={!email || !password}>
              Sign in <ArrowRight className="size-4" aria-hidden />
            </Button>
          </div>
        </form>
        <p className="mt-5 text-center text-[12.5px] text-text-3">You stay signed in for 30 days on this browser.</p>
      </motion.div>
    </div>
  );
}
