import { useEffect, useState } from "react";
import { useSearchParams } from "react-router";
import { Plus } from "lucide-react";
import { useUser } from "@/api/auth";
import { useGoals, useOpenTasks, usePlans, useTargets } from "@/api/plan";
import type { Plan, Target } from "@/api/types";
import { localDay } from "@/lib/time";
import { useNow } from "@/lib/useNow";
import { Button } from "@/ui/Button";
import { Segmented } from "@/ui/Segmented";
import { ErrorState, Skeleton } from "@/ui/States";
import { Page } from "../common/Page";
import { GoalSheet, GoalsView, type GoalEdit } from "./Goals";
import { PlanSheet, PlansView } from "./Plans";
import { TargetSheet, TargetsView } from "./Targets";

type Tab = "goals" | "plans" | "targets";

/** /plan?tab=plans; ?goal= / ?plan= / ?target= deep-link to one item (scores link here). */
export function PlanPage() {
  const user = useUser();
  const now = useNow();
  const today = localDay(now, user.timezone, user.day_start_hour);
  const [params, setParams] = useSearchParams();
  const linked = (["goal", "plan", "target"] as const).find((k) => params.get(k));
  const highlight = linked ? params.get(linked) : null;
  const tab: Tab = (params.get("tab") as Tab | null) ?? (linked === "plan" ? "plans" : linked === "target" ? "targets" : "goals");
  const setTab = (t: Tab) => setParams(t === "goals" ? {} : { tab: t });

  const goals = useGoals();
  const plans = usePlans();
  const targets = useTargets();
  const tasks = useOpenTasks();
  const [goalEdit, setGoalEdit] = useState<GoalEdit | null>(null);
  const [planEdit, setPlanEdit] = useState<Plan | "new" | null>(null);
  const [targetEdit, setTargetEdit] = useState<Target | "new" | null>(null);

  useEffect(() => {
    if (highlight) document.getElementById(`${linked}-${highlight}`)?.scrollIntoView({ block: "center", behavior: "smooth" });
  }, [highlight, linked, goals.data, plans.data, targets.data]);

  const q = tab === "goals" ? goals : tab === "plans" ? plans : targets;
  const counts = { goals: goals.data?.filter((g) => g.status === "active").length, plans: plans.data?.filter((p) => p.period_start <= today && p.period_end >= today).length, targets: targets.data?.length };
  const add = () => (tab === "goals" ? setGoalEdit({}) : tab === "plans" ? setPlanEdit("new") : setTargetEdit("new"));

  return (
    <Page
      wide
      title="Goals & plans"
      subtitle="Where you're headed, how each stretch should go, and the amounts that keep you moving."
      actions={<Button variant="primary" icon={<Plus className="size-4" />} onClick={add}>{tab === "goals" ? "New goal" : tab === "plans" ? "New plan" : "New target"}</Button>}
    >
      <Segmented<Tab>
        className="mb-6"
        label="Plan sections"
        value={tab}
        onChange={setTab}
        options={[
          { value: "goals", label: `Goals${counts.goals ? ` · ${counts.goals}` : ""}` },
          { value: "plans", label: `Plans${counts.plans ? ` · ${counts.plans} now` : ""}` },
          { value: "targets", label: `Targets${counts.targets ? ` · ${counts.targets}` : ""}` },
        ]}
      />
      {q.isPending ? (
        <div className="space-y-2">{[0, 1, 2].map((i) => <Skeleton key={i} className="h-20" />)}</div>
      ) : q.error ? (
        <ErrorState error={q.error} retry={() => q.refetch()} />
      ) : tab === "goals" ? (
        <GoalsView goals={goals.data ?? []} plans={plans.data ?? []} targets={targets.data ?? []} tasks={tasks.data ?? []} today={today} onEdit={setGoalEdit} highlight={linked === "goal" ? highlight : null} />
      ) : tab === "plans" ? (
        <PlansView plans={plans.data ?? []} goals={goals.data ?? []} today={today} onEdit={setPlanEdit} highlight={linked === "plan" ? highlight : null} />
      ) : (
        <TargetsView targets={targets.data ?? []} goals={goals.data ?? []} today={today} onEdit={setTargetEdit} highlight={linked === "target" ? highlight : null} />
      )}
      <GoalSheet key={goalEdit ? goalEdit.goal?.id ?? `new-${goalEdit.parent?.id ?? ""}-${goalEdit.horizon ?? ""}` : "closed"} edit={goalEdit} goals={goals.data ?? []} onClose={() => setGoalEdit(null)} />
      <PlanSheet key={planEdit ? (planEdit === "new" ? "new" : planEdit.id) : "closed"} edit={planEdit} goals={goals.data ?? []} today={today} onClose={() => setPlanEdit(null)} />
      <TargetSheet key={targetEdit ? (targetEdit === "new" ? "new" : targetEdit.id) : "closed"} edit={targetEdit} goals={goals.data ?? []} onClose={() => setTargetEdit(null)} />
    </Page>
  );
}
