-- migrate:up
-- Telegram channel (agent_lld.md §7.2) and the cross-channel digest (§7.3).

CREATE TABLE telegram_links (
  id         uuid PRIMARY KEY,
  tenant_id  uuid NOT NULL REFERENCES tenants(id),
  user_id    uuid NOT NULL REFERENCES users(id),
  chat_id    bigint NOT NULL,
  username   text CHECK (length(username) <= 64),
  linked_at  timestamptz NOT NULL DEFAULT now(),
  revoked_at timestamptz
);
-- One live link per chat (across tenants) and per user.
CREATE UNIQUE INDEX telegram_links_chat ON telegram_links (chat_id) WHERE revoked_at IS NULL;
CREATE UNIQUE INDEX telegram_links_user ON telegram_links (user_id) WHERE revoked_at IS NULL;
ALTER TABLE telegram_links ENABLE ROW LEVEL SECURITY;
ALTER TABLE telegram_links FORCE ROW LEVEL SECURITY;
CREATE POLICY tenant_isolation ON telegram_links USING (tenant_id = app.current_tenant()) WITH CHECK (tenant_id = app.current_tenant());

-- One-time link codes, stored hashed; valid 10 minutes.
CREATE TABLE telegram_link_codes (
  code_hash  text PRIMARY KEY,
  tenant_id  uuid NOT NULL REFERENCES tenants(id),
  user_id    uuid NOT NULL REFERENCES users(id),
  created_at timestamptz NOT NULL DEFAULT now(),
  expires_at timestamptz NOT NULL DEFAULT now() + interval '10 minutes',
  used_at    timestamptz
);
ALTER TABLE telegram_link_codes ENABLE ROW LEVEL SECURITY;
ALTER TABLE telegram_link_codes FORCE ROW LEVEL SECURITY;
CREATE POLICY tenant_isolation ON telegram_link_codes USING (tenant_id = app.current_tenant()) WITH CHECK (tenant_id = app.current_tenant());

-- The long-poll position per bot (`bot`: a hash of the API base and token; a deployment normally has one).
CREATE TABLE ops.telegram_state (
  bot           text PRIMARY KEY CHECK (length(bot) <= 64),
  update_offset bigint NOT NULL DEFAULT 0,
  dropped       bigint NOT NULL DEFAULT 0,   -- updates from unlinked or non-private chats (counted, never answered)
  updated_at    timestamptz NOT NULL DEFAULT now()
);
GRANT SELECT, INSERT, UPDATE ON ops.telegram_state TO thelife_app;

-- Cross-tenant paths (backend_lld.md §4.3): an incoming chat id is not tied to a tenant until it is looked up.
-- Consumes a link code: returns the tenant and user it belongs to and links the chat (replacing earlier links of
-- that chat and that user), or nothing when the code is unknown, used or expired.
CREATE FUNCTION ops.telegram_link(p_code_hash text, p_chat_id bigint, p_username text)
  RETURNS TABLE (tenant_id uuid, user_id uuid)
  LANGUAGE plpgsql VOLATILE SECURITY DEFINER SET search_path = pg_catalog AS $$
DECLARE c record;
BEGIN
  UPDATE public.telegram_link_codes k SET used_at = now()
   WHERE k.code_hash = p_code_hash AND k.used_at IS NULL AND k.expires_at > now()
  RETURNING k.tenant_id, k.user_id INTO c;
  IF NOT FOUND THEN RETURN; END IF;
  UPDATE public.telegram_links l SET revoked_at = now()
   WHERE l.revoked_at IS NULL AND (l.chat_id = p_chat_id OR l.user_id = c.user_id);
  INSERT INTO public.telegram_links (id, tenant_id, user_id, chat_id, username)
  VALUES (gen_random_uuid(), c.tenant_id, c.user_id, p_chat_id, left(p_username, 64));
  tenant_id := c.tenant_id;
  user_id := c.user_id;
  RETURN NEXT;
END $$;

CREATE FUNCTION ops.telegram_chat_user(p_chat_id bigint)
  RETURNS TABLE (tenant_id uuid, user_id uuid)
  LANGUAGE sql STABLE SECURITY DEFINER SET search_path = pg_catalog AS $$
  SELECT l.tenant_id, l.user_id FROM public.telegram_links l
    JOIN public.users u ON u.id = l.user_id JOIN public.tenants t ON t.id = l.tenant_id
   WHERE l.chat_id = p_chat_id AND l.revoked_at IS NULL AND u.status = 'active' AND t.status = 'active'
$$;

GRANT SELECT, INSERT, UPDATE ON telegram_links, telegram_link_codes TO thelife_definer;
ALTER FUNCTION ops.telegram_link(text, bigint, text) OWNER TO thelife_definer;
ALTER FUNCTION ops.telegram_chat_user(bigint) OWNER TO thelife_definer;
REVOKE EXECUTE ON FUNCTION ops.telegram_link(text, bigint, text), ops.telegram_chat_user(bigint) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION ops.telegram_link(text, bigint, text), ops.telegram_chat_user(bigint) TO thelife_app;

-- Today's short digest of each channel, shown to the other channel's turns (§7.3).
CREATE TABLE channel_digests (
  tenant_id     uuid NOT NULL REFERENCES tenants(id),
  user_id       uuid NOT NULL REFERENCES users(id),
  channel       text NOT NULL CHECK (channel IN ('web', 'telegram')),
  local_day     date NOT NULL,
  content       text NOT NULL CHECK (length(content) <= 2000),
  messages_seen integer NOT NULL DEFAULT 0,
  updated_at    timestamptz NOT NULL DEFAULT now(),
  PRIMARY KEY (user_id, channel, local_day)
);
ALTER TABLE channel_digests ENABLE ROW LEVEL SECURITY;
ALTER TABLE channel_digests FORCE ROW LEVEL SECURITY;
CREATE POLICY tenant_isolation ON channel_digests USING (tenant_id = app.current_tenant()) WITH CHECK (tenant_id = app.current_tenant());

-- migrate:down
DROP TABLE channel_digests;
DROP FUNCTION ops.telegram_chat_user(bigint);
DROP FUNCTION ops.telegram_link(text, bigint, text);
DROP TABLE ops.telegram_state;
DROP TABLE telegram_link_codes;
DROP TABLE telegram_links;
