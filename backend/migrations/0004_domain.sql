-- migrate:up
-- Stage 1 domain resources (backend_lld.md §6.3–6.4): work, skills, entertainment, tasks and calendar.
-- Column names match contract field names exactly (generic CRUD layer; checked by the isolation suite).
-- Every table carries the common resource columns of §5 and is set up with app.setup_tenant_table.
--
-- Errors raised for the client use SQLSTATE class TL: TL422 (invalid input), TL409 (state conflict),
-- TL404 (not found). The backend maps them to problem responses and shows the message, so messages here must
-- be safe and readable.

-- UUIDv7 for rows created inside the database (detection events, claims). Postgres 18 has uuidv7().
CREATE FUNCTION app.uuid_v7() RETURNS uuid LANGUAGE sql VOLATILE AS $$
  SELECT encode(set_bit(set_bit(overlay(uuid_send(gen_random_uuid())
           PLACING substring(int8send(floor(extract(epoch FROM clock_timestamp()) * 1000)::bigint) FROM 3)
           FROM 1 FOR 6), 52, 1), 53, 1), 'hex')::uuid
$$;

-- ---------------------------------------------------------------- work
CREATE TABLE work_roles (
  id uuid PRIMARY KEY, tenant_id uuid NOT NULL REFERENCES tenants(id),
  created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now(),
  created_by_kind principal_kind NOT NULL, created_by_id uuid,
  origin origin_kind NOT NULL DEFAULT 'direct', source_message_id uuid,
  version integer NOT NULL DEFAULT 1, deleted_at timestamptz,
  title        text NOT NULL CHECK (length(title) BETWEEN 1 AND 200),
  organization text CHECK (length(organization) <= 200),
  description  text CHECK (length(description) <= 10000),
  active       boolean NOT NULL DEFAULT true,
  UNIQUE (tenant_id, id)
);
SELECT app.setup_tenant_table('work_roles', true);

CREATE TABLE work_responsibilities (
  id uuid PRIMARY KEY, tenant_id uuid NOT NULL REFERENCES tenants(id),
  created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now(),
  created_by_kind principal_kind NOT NULL, created_by_id uuid,
  origin origin_kind NOT NULL DEFAULT 'direct', source_message_id uuid,
  version integer NOT NULL DEFAULT 1, deleted_at timestamptz,
  role_id     uuid NOT NULL,
  title       text NOT NULL CHECK (length(title) BETWEEN 1 AND 200),
  description text CHECK (length(description) <= 10000),
  cadence     text CHECK (length(cadence) <= 500),     -- RRULE, validated by the backend (libical)
  active      boolean NOT NULL DEFAULT true,
  UNIQUE (tenant_id, id),
  FOREIGN KEY (tenant_id, role_id) REFERENCES work_roles (tenant_id, id)
);
SELECT app.setup_tenant_table('work_responsibilities', true);

CREATE TABLE work_projects (
  id uuid PRIMARY KEY, tenant_id uuid NOT NULL REFERENCES tenants(id),
  created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now(),
  created_by_kind principal_kind NOT NULL, created_by_id uuid,
  origin origin_kind NOT NULL DEFAULT 'direct', source_message_id uuid,
  version integer NOT NULL DEFAULT 1, deleted_at timestamptz,
  role_id     uuid,
  title       text NOT NULL CHECK (length(title) BETWEEN 1 AND 200),
  description text CHECK (length(description) <= 10000),
  status      text NOT NULL DEFAULT 'active' CHECK (status IN ('planned', 'active', 'on_hold', 'done', 'cancelled')),
  starts_on   date,
  due_on      date,
  UNIQUE (tenant_id, id),
  FOREIGN KEY (tenant_id, role_id) REFERENCES work_roles (tenant_id, id),
  CHECK (due_on IS NULL OR starts_on IS NULL OR due_on >= starts_on)
);
SELECT app.setup_tenant_table('work_projects', true);

CREATE TABLE work_deliverables (
  id uuid PRIMARY KEY, tenant_id uuid NOT NULL REFERENCES tenants(id),
  created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now(),
  created_by_kind principal_kind NOT NULL, created_by_id uuid,
  origin origin_kind NOT NULL DEFAULT 'direct', source_message_id uuid,
  version integer NOT NULL DEFAULT 1, deleted_at timestamptz,
  project_id   uuid NOT NULL,
  title        text NOT NULL CHECK (length(title) BETWEEN 1 AND 200),
  description  text CHECK (length(description) <= 10000),
  due_at       timestamptz,
  status       text NOT NULL DEFAULT 'open' CHECK (status IN ('open', 'in_progress', 'delivered', 'cancelled')),
  delivered_at timestamptz,
  UNIQUE (tenant_id, id),
  FOREIGN KEY (tenant_id, project_id) REFERENCES work_projects (tenant_id, id)
);
SELECT app.setup_tenant_table('work_deliverables', true);

CREATE FUNCTION app.stamp_delivered() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  IF NEW.status = 'delivered' THEN
    IF TG_OP = 'INSERT' OR OLD.status <> 'delivered' THEN NEW.delivered_at := now(); END IF;
  ELSE
    NEW.delivered_at := NULL;
  END IF;
  RETURN NEW;
END $$;
CREATE TRIGGER stamp_delivered BEFORE INSERT OR UPDATE OF status ON work_deliverables
  FOR EACH ROW EXECUTE FUNCTION app.stamp_delivered();

-- ---------------------------------------------------------------- planning links
-- goals and plans exist since 0003; tasks below reference goals and projects.

-- ---------------------------------------------------------------- skills
CREATE TABLE skill_resources (
  id uuid PRIMARY KEY, tenant_id uuid NOT NULL REFERENCES tenants(id),
  created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now(),
  created_by_kind principal_kind NOT NULL, created_by_id uuid,
  origin origin_kind NOT NULL DEFAULT 'direct', source_message_id uuid,
  version integer NOT NULL DEFAULT 1, deleted_at timestamptz,
  kind             text NOT NULL CHECK (kind IN ('book', 'course')),
  title            text NOT NULL CHECK (length(title) BETWEEN 1 AND 200),
  author           text CHECK (length(author) <= 200),
  url              text CHECK (length(url) <= 2000),
  total_pages      integer CHECK (total_pages >= 1),
  total_duration_s integer CHECK (total_duration_s >= 1),
  goal_id          uuid,
  status           text NOT NULL DEFAULT 'planned' CHECK (status IN ('planned', 'active', 'paused', 'done', 'dropped')),
  progress         jsonb NOT NULL DEFAULT '{"pages_done": null, "position_s": null, "percent": null}',  -- derived
  UNIQUE (tenant_id, id),
  FOREIGN KEY (tenant_id, goal_id) REFERENCES goals (tenant_id, id),
  CHECK (kind = 'book' OR total_pages IS NULL),
  CHECK (kind = 'course' OR total_duration_s IS NULL)
);
SELECT app.setup_tenant_table('skill_resources', true);

CREATE TABLE skill_sections (
  id uuid PRIMARY KEY, tenant_id uuid NOT NULL REFERENCES tenants(id),
  created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now(),
  created_by_kind principal_kind NOT NULL, created_by_id uuid,
  origin origin_kind NOT NULL DEFAULT 'direct', source_message_id uuid,
  version integer NOT NULL DEFAULT 1, deleted_at timestamptz,
  resource_id uuid NOT NULL,
  ordinal     integer NOT NULL CHECK (ordinal >= 0),
  title       text NOT NULL CHECK (length(title) BETWEEN 1 AND 200),
  duration_s  integer CHECK (duration_s >= 1),
  UNIQUE (tenant_id, id),
  UNIQUE (tenant_id, resource_id, id),
  FOREIGN KEY (tenant_id, resource_id) REFERENCES skill_resources (tenant_id, id)
);
SELECT app.setup_tenant_table('skill_sections', true);
CREATE UNIQUE INDEX skill_sections_ordinal_idx ON skill_sections (tenant_id, resource_id, ordinal)
  WHERE deleted_at IS NULL;

CREATE TABLE skill_sessions (
  id uuid PRIMARY KEY, tenant_id uuid NOT NULL REFERENCES tenants(id),
  created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now(),
  created_by_kind principal_kind NOT NULL, created_by_id uuid,
  origin origin_kind NOT NULL DEFAULT 'direct', source_message_id uuid,
  version integer NOT NULL DEFAULT 1, deleted_at timestamptz,
  resource_id uuid NOT NULL,
  started_at  timestamptz NOT NULL,
  ended_at    timestamptz NOT NULL,
  page_from   integer CHECK (page_from >= 1),
  page_to     integer CHECK (page_to >= 1),
  section_id  uuid,
  pos_from_s  integer CHECK (pos_from_s >= 0),
  pos_to_s    integer CHECK (pos_to_s >= 0),
  notes       text CHECK (length(notes) <= 10000),
  source      text NOT NULL DEFAULT 'manual' CHECK (source IN ('manual', 'detection', 'agent')),
  UNIQUE (tenant_id, id),
  FOREIGN KEY (tenant_id, resource_id) REFERENCES skill_resources (tenant_id, id),
  -- a session's section must belong to the session's resource
  FOREIGN KEY (tenant_id, resource_id, section_id) REFERENCES skill_sections (tenant_id, resource_id, id),
  CHECK (ended_at >= started_at),
  CHECK (page_to IS NULL OR page_from IS NULL OR page_to >= page_from),
  CHECK (pos_to_s IS NULL OR pos_from_s IS NULL OR pos_to_s >= pos_from_s)
);
SELECT app.setup_tenant_table('skill_sessions', true);
CREATE INDEX skill_sessions_resource_idx ON skill_sessions (tenant_id, resource_id) WHERE deleted_at IS NULL;
CREATE INDEX skill_sessions_time_idx ON skill_sessions (tenant_id, started_at);

-- Books are tracked by pages, courses by sections and timeline positions.
CREATE FUNCTION app.check_skill_child() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE k text;
BEGIN
  SELECT r.kind INTO k FROM skill_resources r WHERE r.id = NEW.resource_id AND r.deleted_at IS NULL;
  IF k IS NULL THEN
    RAISE EXCEPTION 'resource_id does not refer to an existing skill resource' USING ERRCODE = 'TL422';
  END IF;
  IF TG_TABLE_NAME = 'skill_sections' THEN
    IF k <> 'course' THEN RAISE EXCEPTION 'only courses have sections' USING ERRCODE = 'TL422'; END IF;
  ELSIF k = 'book' AND (NEW.section_id IS NOT NULL OR NEW.pos_from_s IS NOT NULL OR NEW.pos_to_s IS NOT NULL) THEN
    RAISE EXCEPTION 'a book session records pages, not sections or positions' USING ERRCODE = 'TL422';
  ELSIF k = 'course' AND (NEW.page_from IS NOT NULL OR NEW.page_to IS NOT NULL) THEN
    RAISE EXCEPTION 'a course session records sections and positions, not pages' USING ERRCODE = 'TL422';
  END IF;
  RETURN NEW;
END $$;
CREATE TRIGGER check_kind BEFORE INSERT OR UPDATE ON skill_sections
  FOR EACH ROW EXECUTE FUNCTION app.check_skill_child();
CREATE TRIGGER check_kind BEFORE INSERT OR UPDATE ON skill_sessions
  FOR EACH ROW EXECUTE FUNCTION app.check_skill_child();

-- Progress derived from live sessions: books use the furthest page, courses the furthest timeline position
-- (section offset = sum of the durations of earlier sections, plus pos_to_s within the section).
CREATE FUNCTION app.skill_progress(p_resource uuid, p_kind text, p_pages integer, p_duration integer)
  RETURNS jsonb LANGUAGE sql STABLE AS $$
  WITH sec AS (
    SELECT s.id, coalesce(sum(coalesce(s.duration_s, 0)) OVER (ORDER BY s.ordinal
                   ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING), 0) AS offset_s
      FROM skill_sections s WHERE s.resource_id = p_resource AND s.deleted_at IS NULL
  ), agg AS (
    SELECT max(ss.page_to) AS pages_done,
           max(CASE WHEN ss.section_id IS NULL THEN ss.pos_to_s ELSE sec.offset_s + ss.pos_to_s END) AS position_s
      FROM skill_sessions ss LEFT JOIN sec ON sec.id = ss.section_id
     WHERE ss.resource_id = p_resource AND ss.deleted_at IS NULL
  )
  SELECT jsonb_build_object(
    'pages_done', CASE WHEN p_kind = 'book' THEN pages_done END,
    'position_s', CASE WHEN p_kind = 'course' THEN position_s END,
    'percent', CASE
      WHEN p_kind = 'book' AND p_pages IS NOT NULL AND pages_done IS NOT NULL
        THEN round(least(100, pages_done * 100.0 / p_pages), 1)
      WHEN p_kind = 'course' AND p_duration IS NOT NULL AND position_s IS NOT NULL
        THEN round(least(100, position_s * 100.0 / p_duration), 1)
    END)
  FROM agg
$$;

CREATE FUNCTION app.refresh_skill_progress() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  IF TG_TABLE_NAME = 'skill_resources' THEN
    NEW.progress := app.skill_progress(NEW.id, NEW.kind, NEW.total_pages, NEW.total_duration_s);
    RETURN NEW;
  END IF;
  -- sessions or sections changed: recompute the parent (the BEFORE trigger above does the work)
  UPDATE skill_resources SET progress = progress
   WHERE id IN (NEW.resource_id, CASE WHEN TG_OP = 'UPDATE' THEN OLD.resource_id END);
  RETURN NULL;
END $$;
CREATE TRIGGER progress BEFORE INSERT OR UPDATE ON skill_resources
  FOR EACH ROW EXECUTE FUNCTION app.refresh_skill_progress();
CREATE TRIGGER progress AFTER INSERT OR UPDATE ON skill_sessions
  FOR EACH ROW EXECUTE FUNCTION app.refresh_skill_progress();
CREATE TRIGGER progress AFTER INSERT OR UPDATE ON skill_sections
  FOR EACH ROW EXECUTE FUNCTION app.refresh_skill_progress();

-- ---------------------------------------------------------------- entertainment
CREATE TABLE entertainment_items (
  id uuid PRIMARY KEY, tenant_id uuid NOT NULL REFERENCES tenants(id),
  created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now(),
  created_by_kind principal_kind NOT NULL, created_by_id uuid,
  origin origin_kind NOT NULL DEFAULT 'direct', source_message_id uuid,
  version integer NOT NULL DEFAULT 1, deleted_at timestamptz,
  kind   text NOT NULL CHECK (kind IN ('game', 'movie', 'series')),
  title  text NOT NULL CHECK (length(title) BETWEEN 1 AND 200),
  meta   jsonb CHECK (meta IS NULL OR jsonb_typeof(meta) = 'object'),
  status text NOT NULL DEFAULT 'wishlist'
         CHECK (status IN ('wishlist', 'playing', 'watching', 'paused', 'finished', 'dropped')),
  UNIQUE (tenant_id, id)
);
SELECT app.setup_tenant_table('entertainment_items', true);

CREATE TABLE entertainment_slots (
  id uuid PRIMARY KEY, tenant_id uuid NOT NULL REFERENCES tenants(id),
  created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now(),
  created_by_kind principal_kind NOT NULL, created_by_id uuid,
  origin origin_kind NOT NULL DEFAULT 'direct', source_message_id uuid,
  version integer NOT NULL DEFAULT 1, deleted_at timestamptz,
  item_id   uuid,
  starts_at timestamptz NOT NULL,
  ends_at   timestamptz NOT NULL,
  consumed  boolean NOT NULL DEFAULT false,
  notes     text CHECK (length(notes) <= 10000),
  UNIQUE (tenant_id, id),
  FOREIGN KEY (tenant_id, item_id) REFERENCES entertainment_items (tenant_id, id),
  CHECK (ends_at > starts_at)
);
SELECT app.setup_tenant_table('entertainment_slots', true);
CREATE INDEX entertainment_slots_time_idx ON entertainment_slots (tenant_id, starts_at);

-- ---------------------------------------------------------------- tasks
CREATE TABLE tasks (
  id uuid PRIMARY KEY, tenant_id uuid NOT NULL REFERENCES tenants(id),
  created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now(),
  created_by_kind principal_kind NOT NULL, created_by_id uuid,
  origin origin_kind NOT NULL DEFAULT 'direct', source_message_id uuid,
  version integer NOT NULL DEFAULT 1, deleted_at timestamptz,
  title             text NOT NULL CHECK (length(title) BETWEEN 1 AND 200),
  notes             text CHECK (length(notes) <= 10000),
  area              area NOT NULL,
  goal_id           uuid,
  project_id        uuid,
  priority          integer NOT NULL DEFAULT 1 CHECK (priority BETWEEN 0 AND 3),
  estimate_min      integer CHECK (estimate_min BETWEEN 1 AND 1440),
  due_at            timestamptz,
  status            text NOT NULL DEFAULT 'open' CHECK (status IN ('open', 'done', 'cancelled')),
  rrule             text CHECK (length(rrule) <= 500),   -- validated by the backend (libical)
  rrule_dtstart     timestamptz,
  day_profiles      text[] NOT NULL DEFAULT '{}'
                    CHECK (array_position(day_profiles, NULL) IS NULL AND cardinality(day_profiles) <= 20),
  track_consistency boolean NOT NULL DEFAULT false,
  completed_at      timestamptz,
  UNIQUE (tenant_id, id),
  FOREIGN KEY (tenant_id, goal_id) REFERENCES goals (tenant_id, id),
  FOREIGN KEY (tenant_id, project_id) REFERENCES work_projects (tenant_id, id),
  CHECK (rrule IS NULL OR rrule_dtstart IS NOT NULL)
);
SELECT app.setup_tenant_table('tasks', true);
CREATE INDEX tasks_open_idx ON tasks (tenant_id, status) WHERE deleted_at IS NULL;

CREATE FUNCTION app.stamp_completed() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  IF NEW.status = 'done' THEN
    IF TG_OP = 'INSERT' OR OLD.status <> 'done' THEN NEW.completed_at := now(); END IF;
  ELSE
    NEW.completed_at := NULL;
  END IF;
  RETURN NEW;
END $$;
CREATE TRIGGER stamp_completed BEFORE INSERT OR UPDATE OF status ON tasks
  FOR EACH ROW EXECUTE FUNCTION app.stamp_completed();

-- A recurring task without an explicit anchor starts recurring now.
CREATE FUNCTION app.default_rrule_dtstart() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  IF NEW.rrule IS NOT NULL AND NEW.rrule_dtstart IS NULL THEN NEW.rrule_dtstart := now(); END IF;
  RETURN NEW;
END $$;
CREATE TRIGGER default_dtstart BEFORE INSERT OR UPDATE OF rrule, rrule_dtstart ON tasks
  FOR EACH ROW EXECUTE FUNCTION app.default_rrule_dtstart();

-- Only occurrences with state are stored (backend_lld.md §6.3); the rest are expanded from the rrule on read.
CREATE TABLE task_occurrences (
  id uuid PRIMARY KEY, tenant_id uuid NOT NULL REFERENCES tenants(id),
  created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now(),
  created_by_kind principal_kind NOT NULL, created_by_id uuid,
  origin origin_kind NOT NULL DEFAULT 'direct', source_message_id uuid,
  version integer NOT NULL DEFAULT 1, deleted_at timestamptz,
  task_id      uuid NOT NULL,
  occurs_on    date NOT NULL,
  status       text NOT NULL CHECK (status IN ('todo', 'done', 'skipped', 'missed')),
  completed_at timestamptz,
  skip_reason  text CHECK (length(skip_reason) <= 500),
  UNIQUE (tenant_id, task_id, occurs_on),
  FOREIGN KEY (tenant_id, task_id) REFERENCES tasks (tenant_id, id),
  CHECK ((status = 'done') = (completed_at IS NOT NULL))
);
SELECT app.setup_tenant_table('task_occurrences', true);

-- ---------------------------------------------------------------- calendar
CREATE TABLE calendar_events (
  id uuid PRIMARY KEY, tenant_id uuid NOT NULL REFERENCES tenants(id),
  created_at timestamptz NOT NULL DEFAULT now(), updated_at timestamptz NOT NULL DEFAULT now(),
  created_by_kind principal_kind NOT NULL, created_by_id uuid,
  origin origin_kind NOT NULL DEFAULT 'direct', source_message_id uuid,
  version integer NOT NULL DEFAULT 1, deleted_at timestamptz,
  title     text NOT NULL CHECK (length(title) BETWEEN 1 AND 200),
  notes     text CHECK (length(notes) <= 10000),
  starts_at timestamptz NOT NULL,
  ends_at   timestamptz NOT NULL,
  all_day   boolean NOT NULL DEFAULT false,
  rrule     text CHECK (length(rrule) <= 500),
  location  text CHECK (length(location) <= 200),
  area      area,
  category  text CHECK (category ~ '^[a-z0-9_.-]{1,80}$'),
  link_type text CHECK (link_type IN ('task', 'goal', 'plan', 'skill_resource', 'project', 'deliverable',
                                      'entertainment_item')),
  link_id   uuid,
  UNIQUE (tenant_id, id),
  CHECK (ends_at >= starts_at),
  CHECK ((link_type IS NULL) = (link_id IS NULL))
);
SELECT app.setup_tenant_table('calendar_events', true);
CREATE INDEX calendar_events_time_idx ON calendar_events (tenant_id, starts_at) WHERE deleted_at IS NULL;

-- Polymorphic links (events, activities) must point at a live item of the same tenant. RLS hides other
-- tenants' rows, so a foreign id is indistinguishable from a missing one.
CREATE FUNCTION app.check_link() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE tbl text; ok boolean;
BEGIN
  IF NEW.link_type IS NULL THEN RETURN NEW; END IF;
  tbl := CASE NEW.link_type
    WHEN 'task' THEN 'tasks' WHEN 'goal' THEN 'goals' WHEN 'plan' THEN 'plans'
    WHEN 'skill_resource' THEN 'skill_resources' WHEN 'project' THEN 'work_projects'
    WHEN 'deliverable' THEN 'work_deliverables' WHEN 'entertainment_item' THEN 'entertainment_items' END;
  IF tbl IS NULL THEN RAISE EXCEPTION 'unknown link_type' USING ERRCODE = 'TL422'; END IF;
  EXECUTE format('SELECT EXISTS (SELECT 1 FROM public.%I WHERE id = $1 AND deleted_at IS NULL)', tbl)
    INTO ok USING NEW.link_id;
  IF NOT ok THEN
    RAISE EXCEPTION 'link_id does not refer to an existing %', NEW.link_type USING ERRCODE = 'TL422';
  END IF;
  RETURN NEW;
END $$;
CREATE TRIGGER check_link BEFORE INSERT OR UPDATE OF link_type, link_id ON calendar_events
  FOR EACH ROW EXECUTE FUNCTION app.check_link();

-- migrate:down
DROP TABLE calendar_events;
DROP FUNCTION app.check_link();
DROP TABLE task_occurrences;
DROP TABLE tasks;
DROP FUNCTION app.default_rrule_dtstart();
DROP FUNCTION app.stamp_completed();
DROP TABLE entertainment_slots;
DROP TABLE entertainment_items;
DROP TABLE skill_sessions;
DROP TABLE skill_sections;
DROP TABLE skill_resources;
DROP FUNCTION app.refresh_skill_progress();
DROP FUNCTION app.skill_progress(uuid, text, integer, integer);
DROP FUNCTION app.check_skill_child();
DROP TABLE work_deliverables;
DROP FUNCTION app.stamp_delivered();
DROP TABLE work_projects;
DROP TABLE work_responsibilities;
DROP TABLE work_roles;
DROP FUNCTION app.uuid_v7();
