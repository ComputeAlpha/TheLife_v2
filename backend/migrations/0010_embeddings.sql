-- migrate:up
-- Semantic memory tier (agent_lld.md §6.2): bge-m3 vectors of messages, activities, summaries and facts, written by
-- the embeddings.sync job through thelife-embedder (backend_lld.md §2.1).
CREATE TABLE embeddings (
  id            uuid PRIMARY KEY,
  tenant_id     uuid NOT NULL REFERENCES tenants(id),
  owner_type    text NOT NULL CHECK (owner_type IN ('message', 'activity', 'summary', 'fact')),
  owner_id      uuid NOT NULL,
  chunk_ix      smallint NOT NULL DEFAULT 0 CHECK (chunk_ix BETWEEN 0 AND 15),
  content_hash  text NOT NULL,           -- md5 of the embedded text: a changed owner is embedded again
  model         text NOT NULL,
  model_version text NOT NULL,           -- a new model version re-embeds everything (spec 5.4)
  embedding     vector(1024) NOT NULL,
  created_at    timestamptz NOT NULL DEFAULT now(),
  UNIQUE (tenant_id, owner_type, owner_id, chunk_ix)
);
-- Cosine distance on unit vectors. With RLS the tenant filter runs after the index scan, so queries set
-- hnsw.iterative_scan = relaxed_order (pgvector ≥ 0.8) to keep scanning until enough rows pass (backend_lld §4.5).
CREATE INDEX embeddings_hnsw ON embeddings USING hnsw (embedding vector_cosine_ops);
CREATE INDEX embeddings_owner ON embeddings (owner_type, owner_id);
ALTER TABLE embeddings ENABLE ROW LEVEL SECURITY;
ALTER TABLE embeddings FORCE ROW LEVEL SECURITY;
CREATE POLICY tenant_isolation ON embeddings USING (tenant_id = app.current_tenant())
  WITH CHECK (tenant_id = app.current_tenant());
GRANT DELETE ON embeddings TO thelife_app;   -- re-embedding replaces an owner's chunks

-- migrate:down
DROP TABLE embeddings;
