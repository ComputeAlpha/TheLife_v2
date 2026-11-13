-- migrate:up
-- Proactivity (agent_lld.md §4, §7.4, §8): event outbox, triggers, reminders, agent follow-ups, routines, and the
-- scheduling function that queues one proactive.tick per user with something due.

-- Domain changes that may start an agent turn, written in the same transaction as the change (§4.2).
CREATE TABLE events (
  id           uuid PRIMARY KEY,
  tenant_id    uuid NOT NULL REFERENCES tenants(id),
  user_id      uuid REFERENCES users(id),
  kind         text NOT NULL CHECK (kind ~ '^[a-z0-9_.]{1,64}$'),
  entity_type  text,
  entity_id    uuid,
  payload      jsonb NOT NULL DEFAULT '{}'::jsonb,
  created_at   timestamptz NOT NULL DEFAULT now(),
  processed_at timestamptz
);
CREATE INDEX events_pending ON events (tenant_id, created_at) WHERE processed_at IS NULL;
ALTER TABLE events ENABLE ROW LEVEL SECURITY;
ALTER TABLE events FORCE ROW LEVEL SECURITY;
CREATE POLICY tenant_isolation ON events USING (tenant_id = app.current_tenant()) WITH CHECK (tenant_id = app.current_tenant());

CREATE TABLE agent_triggers (
  id          uuid PRIMARY KEY,
  tenant_id   uuid NOT NULL REFERENCES tenants(id),
  user_id     uuid NOT NULL REFERENCES users(id),
  kind        text NOT NULL CHECK (kind ~ '^[a-z0-9_.]{1,64}$'),
  dedupe_key  text NOT NULL,
  due_at      timestamptz NOT NULL DEFAULT now(),
  payload     jsonb NOT NULL DEFAULT '{}'::jsonb,
  status      text NOT NULL DEFAULT 'pending' CHECK (status IN ('pending', 'fired', 'cancelled')),
  turn_id     uuid,
  created_at  timestamptz NOT NULL DEFAULT now(),
  fired_at    timestamptz,
  UNIQUE (tenant_id, user_id, dedupe_key)   -- a burst coalesces into one trigger, ever
);
ALTER TABLE agent_triggers ENABLE ROW LEVEL SECURITY;
ALTER TABLE agent_triggers FORCE ROW LEVEL SECURITY;
CREATE POLICY tenant_isolation ON agent_triggers USING (tenant_id = app.current_tenant()) WITH CHECK (tenant_id = app.current_tenant());

CREATE TABLE reminders (
  id uuid PRIMARY KEY, tenant_id uuid NOT NULL REFERENCES tenants(id),
  created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now(),
  version integer NOT NULL DEFAULT 1, created_by_kind principal_kind NOT NULL, created_by_id uuid,
  origin origin_kind NOT NULL DEFAULT 'direct', source_message_id uuid, deleted_at timestamptz,
  text         text NOT NULL CHECK (length(text) BETWEEN 1 AND 300),
  due_at       timestamptz NOT NULL,
  rrule        text CHECK (length(rrule) <= 500),
  channel_pref text CHECK (channel_pref IN ('web', 'telegram', 'desktop')),
  status       text NOT NULL DEFAULT 'pending' CHECK (status IN ('pending', 'sent', 'done', 'cancelled')),
  link_type    text CHECK (link_type IN ('task', 'goal', 'plan', 'skill_resource', 'project', 'deliverable', 'entertainment_item')),
  link_id      uuid,
  last_sent_at timestamptz,
  CHECK ((link_type IS NULL) = (link_id IS NULL))
);
CREATE INDEX reminders_due ON reminders (tenant_id, due_at) WHERE status = 'pending' AND deleted_at IS NULL;
SELECT app.setup_tenant_table('reminders', true);

CREATE TABLE agent_followups (
  id uuid PRIMARY KEY, tenant_id uuid NOT NULL REFERENCES tenants(id),
  created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now(),
  version integer NOT NULL DEFAULT 1, created_by_kind principal_kind NOT NULL DEFAULT 'agent', created_by_id uuid,
  origin origin_kind NOT NULL DEFAULT 'inferred', source_message_id uuid, deleted_at timestamptz,
  user_id         uuid NOT NULL REFERENCES users(id),
  conversation_id uuid REFERENCES conversations(id),
  channel         text NOT NULL DEFAULT 'web' CHECK (channel IN ('web', 'telegram')),
  due_at          timestamptz NOT NULL,
  reason          text NOT NULL CHECK (length(reason) BETWEEN 1 AND 300),
  status          text NOT NULL DEFAULT 'pending' CHECK (status IN ('pending', 'fired', 'cancelled')),
  fired_at        timestamptz
);
SELECT app.setup_tenant_table('agent_followups', true);

CREATE TABLE routines (
  id uuid PRIMARY KEY, tenant_id uuid NOT NULL REFERENCES tenants(id),
  created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now(),
  version integer NOT NULL DEFAULT 1, created_by_kind principal_kind NOT NULL, created_by_id uuid,
  origin origin_kind NOT NULL DEFAULT 'direct', source_message_id uuid, deleted_at timestamptz,
  kind          text NOT NULL CHECK (kind IN ('morning', 'evening', 'weekly', 'custom')),
  title         text NOT NULL CHECK (length(title) BETWEEN 1 AND 200),
  mode          text NOT NULL DEFAULT 'adaptive' CHECK (mode IN ('adaptive', 'fixed')),
  default_time  text NOT NULL CHECK (default_time ~ '^([01][0-9]|2[0-3]):[0-5][0-9]$'),
  window_start  text CHECK (window_start ~ '^([01][0-9]|2[0-3]):[0-5][0-9]$'),
  window_end    text CHECK (window_end ~ '^([01][0-9]|2[0-3]):[0-5][0-9]$'),
  weekdays      text[] NOT NULL DEFAULT '{}' CHECK (weekdays <@ ARRAY['MO','TU','WE','TH','FR','SA','SU']),
  day_profiles  text[] NOT NULL DEFAULT '{}',
  enabled       boolean NOT NULL DEFAULT true,
  instructions  text CHECK (length(instructions) <= 10000),
  learned_time  text CHECK (learned_time ~ '^([01][0-9]|2[0-3]):[0-5][0-9]$'),
  last_fired_on date,
  skip_on       date
);
SELECT app.setup_tenant_table('routines', true);

ALTER TABLE notifications ADD COLUMN proactive boolean NOT NULL DEFAULT false;
-- The user-facing view (resource columns for the generic list/get path, plus the deliveries).
CREATE VIEW notification_feed WITH (security_invoker = true, security_barrier = true) AS
SELECT n.id, n.tenant_id, n.created_at, n.created_at AS updated_at, 1 AS version,
       'system'::principal_kind AS created_by_kind, NULL::uuid AS created_by_id, 'direct'::origin_kind AS origin,
       NULL::uuid AS source_message_id, NULL::timestamptz AS deleted_at, n.user_id, n.kind, n.urgency, n.title,
       n.body, n.action, n.ref_type, n.ref_id, n.conversation_id, n.resolved_at,
       coalesce((SELECT jsonb_agg(jsonb_build_object('channel', d.channel, 'status', d.status, 'at', d.at) ORDER BY d.at)
                   FROM notification_deliveries d WHERE d.notification_id = n.id), '[]'::jsonb) AS deliveries
  FROM notifications n;
GRANT SELECT ON notification_feed TO thelife_app;

ALTER TABLE turns DROP CONSTRAINT turns_trigger_check;
ALTER TABLE turns ADD CONSTRAINT turns_trigger_check
  CHECK (trigger IN ('user_message', 'approval', 'proactive', 'deferred_reply'));
ALTER TABLE turns ADD COLUMN trigger_kind text;

-- Seeds the default routines of tenants that have none, and queues proactive.tick for every user with something
-- due: a reminder, follow-up or trigger, unprocessed events, a routine whose time or window has come, a reply
-- deferred while the assistant was offline, or the periodic scan (every 10 minutes).
CREATE FUNCTION ops.enqueue_proactive_ticks() RETURNS integer
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
GRANT SELECT ON reminders, agent_followups, agent_triggers, events, turns TO thelife_definer;
GRANT SELECT, INSERT ON routines TO thelife_definer;
ALTER FUNCTION ops.enqueue_proactive_ticks() OWNER TO thelife_definer;
REVOKE EXECUTE ON FUNCTION ops.enqueue_proactive_ticks() FROM PUBLIC;
GRANT EXECUTE ON FUNCTION ops.enqueue_proactive_ticks() TO thelife_app;

-- migrate:down
DROP FUNCTION ops.enqueue_proactive_ticks();
REVOKE SELECT ON reminders, agent_followups, agent_triggers, events FROM thelife_definer;
ALTER TABLE turns DROP COLUMN trigger_kind;
ALTER TABLE turns DROP CONSTRAINT turns_trigger_check;
ALTER TABLE turns ADD CONSTRAINT turns_trigger_check CHECK (trigger IN ('user_message', 'approval'));
DROP VIEW notification_feed;
ALTER TABLE notifications DROP COLUMN proactive;
DROP TABLE routines;
DROP TABLE agent_followups;
DROP TABLE reminders;
DROP TABLE agent_triggers;
DROP TABLE events;
