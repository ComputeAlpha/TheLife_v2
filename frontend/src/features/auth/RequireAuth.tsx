import { useEffect, type ReactNode } from "react";
import { Navigate, useLocation } from "react-router";
import { useQueryClient } from "@tanstack/react-query";
import { dropSession, useMe } from "@/api/auth";
import { onUnauthorized } from "@/api/client";
import { ErrorState } from "@/ui/States";
import { Logo } from "@/shell/Logo";

export function RequireAuth({ children }: { children: ReactNode }) {
  const me = useMe();
  const location = useLocation();
  const qc = useQueryClient();

  // A 401 anywhere (session revoked or expired) signs the tab out.
  useEffect(() => onUnauthorized(() => dropSession(qc)), [qc]);

  if (me.isPending) return <Splash />;
  if (me.isError) return <ErrorState error={me.error} retry={() => me.refetch()} className="min-h-dvh justify-center" />;
  if (!me.data) {
    const next = location.pathname + location.search + location.hash;
    return <Navigate to={next === "/" ? "/login" : `/login?next=${encodeURIComponent(next)}`} replace />;
  }
  return <>{children}</>;
}

function Splash() {
  return (
    <div className="grid min-h-dvh place-items-center" aria-busy="true" aria-label="Loading">
      <Logo className="size-12 animate-pulse drop-shadow-[0_0_14px_rgb(34_211_238/.5)]" />
    </div>
  );
}
