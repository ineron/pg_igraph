-- pg_igraph--1.1--1.2.sql
-- Migration script from version 1.1 to 1.2
-- Adds new functionality while preserving all existing features

-- ────────────────────────────────────────────────
-- NEW in v1.2: SQL-callable schema provisioning
-- ────────────────────────────────────────────────
--
-- Creates the backing tables for a graph instance (bare or prefixed),
-- equivalent to what init_graph.sh does via psql+shell, but callable from
-- an ordinary SQL session that has no filesystem/shell access (e.g. a
-- portal-role connection). Mirrors build_table_name()'s contract exactly
-- (igraph_exec.c / pg_igraph.c): table_prefix is either '' (bare tables),
-- 'myprefix_' (prefixed tables in the current schema — include your own
-- trailing '_'), or 'myschema.myprefix_' (prefixed tables in a specific
-- schema, created if missing).
--
-- Idempotent: safe to call again on an existing prefix (tables/indexes/
-- partitions use IF NOT EXISTS and are left untouched).
--
-- Examples:
--   SELECT graph_provision_schema();                          -- bare tables
--   SELECT graph_provision_schema('social_');                  -- prefixed, current schema
--   SELECT graph_provision_schema('data_78f7d4ef.d4ef_');      -- prefixed, dedicated schema
--   SELECT graph_provision_schema('social_', 32);              -- 32 partitions
CREATE FUNCTION graph_provision_schema(
  table_prefix TEXT DEFAULT '',
  partitions   INT  DEFAULT 16
) RETURNS VOID
LANGUAGE plpgsql
AS $BODY$
DECLARE
  dot_pos     INT;
  schema_name TEXT;
  prefix_name TEXT;
  qschema     TEXT;
  i           INT;
BEGIN
  IF partitions NOT IN (8, 16, 32, 64) THEN
    RAISE EXCEPTION 'graph_provision_schema: partitions must be one of 8, 16, 32, 64 (got %)', partitions;
  END IF;

  dot_pos := strpos(table_prefix, '.');
  IF dot_pos > 0 THEN
    schema_name := substring(table_prefix FROM 1 FOR dot_pos - 1);
    prefix_name := substring(table_prefix FROM dot_pos + 1);
  ELSE
    schema_name := NULL;
    prefix_name := table_prefix;
  END IF;

  IF prefix_name <> '' AND prefix_name !~ '^[a-zA-Z_][a-zA-Z0-9_]*$' THEN
    RAISE EXCEPTION 'graph_provision_schema: invalid table_prefix ''%'' — must start with a letter/underscore and contain only letters, digits, underscores', prefix_name;
  END IF;
  IF schema_name IS NOT NULL AND schema_name !~ '^[a-zA-Z_][a-zA-Z0-9_]*$' THEN
    RAISE EXCEPTION 'graph_provision_schema: invalid schema name ''%'' in table_prefix', schema_name;
  END IF;

  IF schema_name IS NOT NULL THEN
    EXECUTE format('CREATE SCHEMA IF NOT EXISTS %I', schema_name);
    qschema := quote_ident(schema_name) || '.';
  ELSE
    qschema := '';
  END IF;

  -- ── Справочники ──────────────────────────────
  EXECUTE format(
    'CREATE TABLE IF NOT EXISTS %s (
       id    SMALLINT PRIMARY KEY GENERATED ALWAYS AS IDENTITY,
       name  TEXT NOT NULL UNIQUE
     )', format('%s%I', qschema, prefix_name || 'node_labels'));

  EXECUTE format(
    'CREATE TABLE IF NOT EXISTS %s (
       id    SMALLINT PRIMARY KEY GENERATED ALWAYS AS IDENTITY,
       name  TEXT NOT NULL UNIQUE
     )', format('%s%I', qschema, prefix_name || 'rel_types'));

  -- primitive: 1=bigint 2=text 3=uuid 4=timestamp 5=bool 6=numeric 7=jsonb
  EXECUTE format(
    'CREATE TABLE IF NOT EXISTS %s (
       id        SMALLINT PRIMARY KEY GENERATED ALWAYS AS IDENTITY,
       name      TEXT     NOT NULL UNIQUE,
       primitive SMALLINT NOT NULL CHECK (primitive BETWEEN 1 AND 7),
       ref_label SMALLINT REFERENCES %s(id)
     )',
    format('%s%I', qschema, prefix_name || 'property_types'),
    format('%s%I', qschema, prefix_name || 'node_labels'));

  -- ── Узлы ─────────────────────────────────────
  EXECUTE format(
    'CREATE TABLE IF NOT EXISTS %s (
       id    BIGINT   PRIMARY KEY GENERATED ALWAYS AS IDENTITY,
       label SMALLINT NOT NULL REFERENCES %s(id)
     )',
    format('%s%I', qschema, prefix_name || 'nodes'),
    format('%s%I', qschema, prefix_name || 'node_labels'));

  EXECUTE format('CREATE INDEX IF NOT EXISTS %I ON %s(label)',
    'idx_' || prefix_name || 'nodes_label',
    format('%s%I', qschema, prefix_name || 'nodes'));

  -- ── Свойства узлов ───────────────────────────
  EXECUTE format(
    'CREATE TABLE IF NOT EXISTS %s (
       node_id BIGINT   NOT NULL REFERENCES %s(id) ON DELETE CASCADE,
       prop_id SMALLINT NOT NULL REFERENCES %s(id),
       value   BYTEA    NOT NULL,
       PRIMARY KEY (node_id, prop_id)
     )',
    format('%s%I', qschema, prefix_name || 'node_properties'),
    format('%s%I', qschema, prefix_name || 'nodes'),
    format('%s%I', qschema, prefix_name || 'property_types'));

  EXECUTE format('CREATE INDEX IF NOT EXISTS %I ON %s(node_id)',
    'idx_' || prefix_name || 'node_props_node',
    format('%s%I', qschema, prefix_name || 'node_properties'));

  -- ── Рёбра — партиционированные по HASH(from_id) ──
  EXECUTE format(
    'CREATE TABLE IF NOT EXISTS %s (
       from_id   BIGINT   NOT NULL,
       to_id     BIGINT   NOT NULL,
       rel_type  SMALLINT NOT NULL REFERENCES %s(id),
       direction BOOL     NOT NULL DEFAULT TRUE,
       data      BYTEA
     ) PARTITION BY HASH (from_id)',
    format('%s%I', qschema, prefix_name || 'edges'),
    format('%s%I', qschema, prefix_name || 'rel_types'));

  FOR i IN 0..partitions - 1 LOOP
    EXECUTE format(
      'CREATE TABLE IF NOT EXISTS %s PARTITION OF %s FOR VALUES WITH (MODULUS %s, REMAINDER %s)',
      format('%s%I', qschema, format('%sedges_p%s', prefix_name, i)),
      format('%s%I', qschema, prefix_name || 'edges'),
      partitions, i);

    EXECUTE format('CREATE INDEX IF NOT EXISTS %I ON %s (from_id, rel_type, to_id) WHERE direction = TRUE',
      prefix_name || 'edges_p' || i || '_fwd_idx',
      format('%s%I', qschema, format('%sedges_p%s', prefix_name, i)));

    EXECUTE format('CREATE INDEX IF NOT EXISTS %I ON %s (from_id, rel_type, to_id) WHERE direction = FALSE',
      prefix_name || 'edges_p' || i || '_rev_idx',
      format('%s%I', qschema, format('%sedges_p%s', prefix_name, i)));

    -- Covering index for build_adj_list bulk load (see init_graph.sh for
    -- the full rationale: lets a rel_type+direction scan stay index-only
    -- per partition instead of a full-table scan across all partitions).
    EXECUTE format('CREATE INDEX IF NOT EXISTS %I ON %s (rel_type, direction) INCLUDE (from_id, to_id)',
      prefix_name || 'edges_p' || i || '_cover_idx',
      format('%s%I', qschema, format('%sedges_p%s', prefix_name, i)));
  END LOOP;

  -- ── Свойства рёбер — тоже партиционированные ──
  EXECUTE format(
    'CREATE TABLE IF NOT EXISTS %s (
       from_id   BIGINT   NOT NULL,
       to_id     BIGINT   NOT NULL,
       rel_type  SMALLINT NOT NULL,
       direction BOOL     NOT NULL,
       prop_id   SMALLINT NOT NULL REFERENCES %s(id),
       value     BYTEA    NOT NULL
     ) PARTITION BY HASH (from_id)',
    format('%s%I', qschema, prefix_name || 'edge_properties'),
    format('%s%I', qschema, prefix_name || 'property_types'));

  FOR i IN 0..partitions - 1 LOOP
    EXECUTE format(
      'CREATE TABLE IF NOT EXISTS %s PARTITION OF %s FOR VALUES WITH (MODULUS %s, REMAINDER %s)',
      format('%s%I', qschema, format('%sedge_properties_p%s', prefix_name, i)),
      format('%s%I', qschema, prefix_name || 'edge_properties'),
      partitions, i);

    EXECUTE format('CREATE INDEX IF NOT EXISTS %I ON %s (from_id, to_id, prop_id)',
      prefix_name || 'edge_properties_p' || i || '_idx',
      format('%s%I', qschema, format('%sedge_properties_p%s', prefix_name, i)));
  END LOOP;

  -- ── Комплексные типы ─────────────────────────
  EXECUTE format(
    'CREATE TABLE IF NOT EXISTS %s (
       id    SMALLINT PRIMARY KEY GENERATED ALWAYS AS IDENTITY,
       name  TEXT NOT NULL UNIQUE
     )', format('%s%I', qschema, prefix_name || 'complex_types'));

  EXECUTE format(
    'CREATE TABLE IF NOT EXISTS %s (
       type_id    SMALLINT NOT NULL REFERENCES %s(id) ON DELETE CASCADE,
       pos        SMALLINT NOT NULL CHECK (pos >= 0),
       field_name TEXT     NOT NULL,
       PRIMARY KEY (type_id, pos)
     )',
    format('%s%I', qschema, prefix_name || 'complex_type_fields'),
    format('%s%I', qschema, prefix_name || 'complex_types'));

  EXECUTE format('CREATE INDEX IF NOT EXISTS %I ON %s(type_id, pos)',
    'idx_' || prefix_name || 'complex_fields_type',
    format('%s%I', qschema, prefix_name || 'complex_type_fields'));

  -- ── Мета: параметры развёртывания ────────────
  EXECUTE format(
    'CREATE TABLE IF NOT EXISTS %s (
       key   TEXT PRIMARY KEY,
       value TEXT NOT NULL
     )', format('%s%I', qschema, prefix_name || 'graph_meta'));

  EXECUTE format(
    'INSERT INTO %s(key, value) VALUES
       (''version'',    ''1.2''),
       (''partitions'', %L),
       (''schema'',     %L),
       (''prefix'',     %L),
       (''created_at'', NOW()::TEXT)
     ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value',
    format('%s%I', qschema, prefix_name || 'graph_meta'),
    partitions::TEXT,
    COALESCE(schema_name, current_schema()),
    prefix_name);
END;
$BODY$;

COMMENT ON FUNCTION graph_provision_schema(TEXT, INT) IS
  'Provisions (idempotently) the backing tables for a bare or prefixed graph instance — the SQL-callable equivalent of init_graph.sh, for sessions with no filesystem/shell access.';
