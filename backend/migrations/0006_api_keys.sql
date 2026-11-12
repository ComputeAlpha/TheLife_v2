-- migrate:up
-- API keys are credentials of kind api_key (0002). This view gives them the resource shape of the contract
-- (api/resources/api_keys.yaml) so the generic list/get path serves them. security_invoker keeps the forced
-- RLS policy of credentials in force for whoever queries the view; token_hash is never exposed.
CREATE VIEW api_keys WITH (security_invoker = true, security_barrier = true) AS
SELECT c.id, c.tenant_id, c.created_at, c.updated_at, c.version,
       'user'::principal_kind AS created_by_kind, c.user_id AS created_by_id,
       'direct'::origin_kind AS origin, NULL::uuid AS source_message_id, NULL::timestamptz AS deleted_at,
       c.name, c.scopes, c.expires_at, c.token_prefix, c.last_used_at, c.revoked_at
  FROM credentials c
 WHERE c.kind = 'api_key';
REVOKE INSERT, UPDATE, DELETE ON api_keys FROM thelife_app;
GRANT SELECT ON api_keys TO thelife_app;

ALTER TABLE credentials ADD CONSTRAINT api_key_named CHECK (kind <> 'api_key' OR length(name) >= 1);

-- migrate:down
ALTER TABLE credentials DROP CONSTRAINT api_key_named;
DROP VIEW api_keys;
