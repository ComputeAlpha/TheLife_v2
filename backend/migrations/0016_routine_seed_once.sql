-- migrate:up
-- The default routines (morning, evening, weekly) are seeded once per tenant. ops.enqueue_proactive_ticks checked
-- "no routines yet" without a lock, so two concurrent runs (two workers, or two servers sharing a database) could
-- both seed and leave duplicates that fire twice. A unique index on the seeded ones, and ON CONFLICT DO NOTHING.

-- Existing duplicates: keep the oldest of each kind, soft-delete the rest. routines is under forced RLS, so this
-- runs as thelife_definer in a function that exists only during the migration.
GRANT UPDATE (deleted_at) ON routines TO thelife_definer;
CREATE FUNCTION ops.dedupe_seeded_routines() RETURNS integer
  LANGUAGE sql VOLATILE SECURITY DEFINER SET search_path = pg_catalog AS $$
  WITH ranked AS (
    SELECT id, row_number() OVER (PARTITION BY tenant_id, kind ORDER BY created_at, id) AS n
      FROM public.routines WHERE created_by_kind = 'system' AND kind <> 'custom' AND deleted_at IS NULL),
  d AS (UPDATE public.routines r SET deleted_at = now() FROM ranked WHERE r.id = ranked.id AND ranked.n > 1
        RETURNING 1)
  SELECT count(*)::integer FROM d
$$;
ALTER FUNCTION ops.dedupe_seeded_routines() OWNER TO thelife_definer;
SELECT ops.dedupe_seeded_routines();
DROP FUNCTION ops.dedupe_seeded_routines();
REVOKE UPDATE (deleted_at) ON routines FROM thelife_definer;

CREATE UNIQUE INDEX routines_seeded_once ON routines (tenant_id, kind)
  WHERE created_by_kind = 'system' AND kind <> 'custom' AND deleted_at IS NULL;

CREATE OR REPLACE FUNCTION ops.enqueue_proactive_ticks() RETURNS integer
  LANGUAGE plpgsql VOLATILE SECURITY DEFINER SET search_path = pg_catalog AS $$
DECLARE n integer;
BEGIN
  INSERT INTO public.routines (id, tenant_id, created_by_kind, kind, title, mode, default_time, window_start,
                               window_end, weekdays)
  SELECT gen_random_uuid(), t.id, 'system', d.kind, d.title, d.mode, d.dt, d.ws, d.we, d.wd
    FROM public.tenants t
   CROSS JOIN (VALUES ('morning', 'Morning plan', 'adaptive', '08:00', '06:00', '10:00', '{}'::text[]),
                      ('evening', 'Evening review', 'adaptive', '21:00', '19:00', '22:00', '{}'::text[]),
                      ('weekly', 'Weekly review', 'fixed', '18:00', NULL, NULL, '{SU}'::text[]))
               AS d(kind, title, mode, dt, ws, we, wd)
   WHERE t.status = 'active' AND NOT EXISTS (SELECT 1 FROM public.routines r WHERE r.tenant_id = t.id)
  ON CONFLICT (tenant_id, kind) WHERE created_by_kind = 'system' AND kind <> 'custom' AND deleted_at IS NULL
  DO NOTHING;

  WITH u AS (
    SELECT u.tenant_id, u.id, (now() AT TIME ZONE u.timezone) AS lt,
           ((now() AT TIME ZONE u.timezone) - make_interval(hours => u.day_start_hour))::date AS day
      FROM public.users u JOIN public.tenants t ON t.id = u.tenant_id
     WHERE u.status = 'active' AND t.status = 'active'),
  due AS (
    SELECT u.tenant_id, u.id FROM u
     WHERE EXISTS (SELECT 1 FROM public.reminders r WHERE r.tenant_id = u.tenant_id AND r.deleted_at IS NULL
                     AND r.status = 'pending' AND r.due_at <= now())
        OR EXISTS (SELECT 1 FROM public.agent_followups f WHERE f.user_id = u.id AND f.deleted_at IS NULL
                     AND f.status = 'pending' AND f.due_at <= now())
        OR EXISTS (SELECT 1 FROM public.agent_triggers g WHERE g.user_id = u.id AND g.status = 'pending'
                     AND g.due_at <= now())
        OR EXISTS (SELECT 1 FROM public.events e WHERE e.tenant_id = u.tenant_id AND e.processed_at IS NULL)
        OR EXISTS (SELECT 1 FROM public.routines r WHERE r.tenant_id = u.tenant_id AND r.deleted_at IS NULL
                     AND r.enabled AND (r.last_fired_on IS NULL OR r.last_fired_on < u.day)
                     AND u.lt::time >= coalesce(r.window_start, r.default_time)::time)
        OR EXISTS (SELECT 1 FROM public.turns tu WHERE tu.user_id = u.id AND tu.status = 'manual'
                     AND tu.trigger = 'user_message' AND tu.created_at > now() - interval '7 days')
        OR extract(minute FROM now())::int % 10 = 0),
  queued AS (
    INSERT INTO ops.jobs (id, tenant_id, kind, payload, dedupe_key, priority)
    SELECT gen_random_uuid(), tenant_id, 'proactive.tick', jsonb_build_object('user_id', id), 'tick:' || id, 1
      FROM due
    ON CONFLICT (kind, dedupe_key) WHERE dedupe_key IS NOT NULL AND status IN ('queued', 'running') DO NOTHING
    RETURNING 1)
  SELECT count(*)::integer INTO n FROM queued;
  RETURN n;
END $$;

-- migrate:down
DROP INDEX routines_seeded_once;
CREATE OR REPLACE FUNCTION ops.enqueue_proactive_ticks() RETURNS integer
  LANGUAGE plpgsql VOLATILE SECURITY DEFINER SET search_path = pg_catalog AS $$
DECLARE n integer;
BEGIN
  INSERT INTO public.routines (id, tenant_id, created_by_kind, kind, title, mode, default_time, window_start,
                               window_end, weekdays)
  SELECT gen_random_uuid(), t.id, 'system', d.kind, d.title, d.mode, d.dt, d.ws, d.we, d.wd
    FROM public.tenants t
   CROSS JOIN (VALUES ('morning', 'Morning plan', 'adaptive', '08:00', '06:00', '10:00', '{}'::text[]),
                      ('evening', 'Evening review', 'adaptive', '21:00', '19:00', '22:00', '{}'::text[]),
                      ('weekly', 'Weekly review', 'fixed', '18:00', NULL, NULL, '{SU}'::text[]))
               AS d(kind, title, mode, dt, ws, we, wd)
   WHERE t.status = 'active' AND NOT EXISTS (SELECT 1 FROM public.routines r WHERE r.tenant_id = t.id);

  WITH u AS (
    SELECT u.tenant_id, u.id, (now() AT TIME ZONE u.timezone) AS lt,
           ((now() AT TIME ZONE u.timezone) - make_interval(hours => u.day_start_hour))::date AS day
      FROM public.users u JOIN public.tenants t ON t.id = u.tenant_id
     WHERE u.status = 'active' AND t.status = 'active'),
  due AS (
    SELECT u.tenant_id, u.id FROM u
     WHERE EXISTS (SELECT 1 FROM public.reminders r WHERE r.tenant_id = u.tenant_id AND r.deleted_at IS NULL
                     AND r.status = 'pending' AND r.due_at <= now())
        OR EXISTS (SELECT 1 FROM public.agent_followups f WHERE f.user_id = u.id AND f.deleted_at IS NULL
                     AND f.status = 'pending' AND f.due_at <= now())
        OR EXISTS (SELECT 1 FROM public.agent_triggers g WHERE g.user_id = u.id AND g.status = 'pending'
                     AND g.due_at <= now())
        OR EXISTS (SELECT 1 FROM public.events e WHERE e.tenant_id = u.tenant_id AND e.processed_at IS NULL)
        OR EXISTS (SELECT 1 FROM public.routines r WHERE r.tenant_id = u.tenant_id AND r.deleted_at IS NULL
                     AND r.enabled AND (r.last_fired_on IS NULL OR r.last_fired_on < u.day)
                     AND u.lt::time >= coalesce(r.window_start, r.default_time)::time)
        OR EXISTS (SELECT 1 FROM public.turns tu WHERE tu.user_id = u.id AND tu.status = 'manual'
                     AND tu.trigger = 'user_message' AND tu.created_at > now() - interval '7 days')
        OR extract(minute FROM now())::int % 10 = 0),
  queued AS (
    INSERT INTO ops.jobs (id, tenant_id, kind, payload, dedupe_key, priority)
    SELECT gen_random_uuid(), tenant_id, 'proactive.tick', jsonb_build_object('user_id', id), 'tick:' || id, 1
      FROM due
    ON CONFLICT (kind, dedupe_key) WHERE dedupe_key IS NOT NULL AND status IN ('queued', 'running') DO NOTHING
    RETURNING 1)
  SELECT count(*)::integer INTO n FROM queued;
  RETURN n;
END $$;
