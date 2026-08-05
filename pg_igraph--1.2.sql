-- pg_igraph--1.2.sql
-- Native graph traversal extension for PostgreSQL v1.2
-- Includes all v1.0 and v1.1 functionality plus new features:
-- - Table prefix support for core API functions (from v1.1)
-- - SQL-callable schema provisioning via graph_provision_schema (NEW in v1.2)
-- - Backward compatibility maintained

-- ────────────────────────────────────────────────
-- Include all v1.0 functionality
-- ────────────────────────────────────────────────

-- Node operations (now with optional table_prefix support)
-- Legacy signature removed in favor of single overloaded function

-- Edge operations
-- Legacy graph_add_edge signature removed in favor of overloaded function with table_prefix

-- Traversal
-- BFS обход графа. direction: TRUE=прямой FALSE=обратный
CREATE FUNCTION graph_traverse(
  start_id  BIGINT,
  rel_name  TEXT,
  direction BOOL,
  max_depth INT
) RETURNS SETOF BIGINT
  AS 'pg_igraph', 'graph_traverse'
  LANGUAGE C STRICT;

-- Кратчайший путь между двумя узлами (двунаправленный BFS)
-- Возвращает массив node_id или NULL если пути нет
CREATE FUNCTION graph_shortest_path(
  start_id  BIGINT,
  end_id    BIGINT,
  rel_name  TEXT
) RETURNS BIGINT[]
  AS 'pg_igraph', 'graph_shortest_path'
  LANGUAGE C STRICT;

-- INT convenience aliases
-- PostgreSQL treats integer literals as INT4, not INT8.
-- These wrappers allow calling functions without explicit ::bigint casts.
CREATE FUNCTION graph_traverse(
  start_id  INT,
  rel_name  TEXT,
  direction BOOL,
  max_depth INT
) RETURNS SETOF BIGINT AS $$
  SELECT graph_traverse(start_id::bigint, rel_name, direction, max_depth)
$$ LANGUAGE SQL STRICT;

CREATE FUNCTION graph_shortest_path(
  start_id  INT,
  end_id    INT,
  rel_name  TEXT
) RETURNS BIGINT[] AS $$
  SELECT graph_shortest_path(start_id::bigint, end_id::bigint, rel_name)
$$ LANGUAGE SQL STRICT;

-- graph_add_edge/graph_get_property/graph_set_property/graph_get_node_properties
-- INT aliases removed here: each collides with its own table_prefix-defaulted
-- overload declared further below (e.g. graph_get_property(BIGINT, TEXT,
-- TEXT DEFAULT '')) -- calling with the short arg list is ambiguous between
-- "this N-arg function" and "that (N+1)-arg function using its default",
-- since both match the given arguments with zero casts. The defaulted
-- overload already accepts the short call, so the dedicated INT/BIGINT-only
-- version is redundant, not a distinct capability. See task #8.

CREATE FUNCTION graph_delete_property(
  node_id   INT,
  prop_name TEXT
) RETURNS VOID AS $$
  SELECT graph_delete_property(node_id::bigint, prop_name)
$$ LANGUAGE SQL STRICT;

-- Register a new complex type (e.g. 'Money', 'Address')
-- Returns the type id — store in BYTEA header: op_id=0x0E, params=id
CREATE FUNCTION graph_add_complex_type(
  type_name TEXT
) RETURNS SMALLINT
  AS 'pg_igraph', 'graph_add_complex_type'
  LANGUAGE C STRICT;

-- Add a field name to a complex type at given position
CREATE FUNCTION graph_add_complex_field(
  type_id    SMALLINT,
  pos        SMALLINT,
  field_name TEXT
) RETURNS VOID
  AS 'pg_igraph', 'graph_add_complex_field'
  LANGUAGE C STRICT;

-- Get field names ordered by position (used by decoder to build JSON)
CREATE FUNCTION graph_get_complex_fields(
  type_id SMALLINT
) RETURNS TABLE(pos SMALLINT, field_name TEXT)
  AS 'pg_igraph', 'graph_get_complex_fields'
  LANGUAGE C STRICT;

-- Properties
-- primitive типы:
--   1 = bigint
--   2 = text
--   3 = uuid
--   4 = timestamp
--   5 = bool
--   6 = numeric
--   7 = jsonb

-- graph_set_property(node_id BIGINT, prop_name, primitive, value, ref_label)
-- and graph_get_node_properties(node_id BIGINT) removed here (were the
-- non-prefixed C-level base functions) -- each collides with its own
-- table_prefix-defaulted overload below the same way the INT aliases did
-- above; see the note there and task #8. graph_set_property_extended /
-- graph_get_node_properties_extended behave identically for table_prefix=''
-- (build_table_name() returns the bare table name for an empty prefix), so
-- nothing is lost by keeping only the defaulted overload.

-- Удалить свойство узла
CREATE FUNCTION graph_delete_property(
  node_id   BIGINT,
  prop_name TEXT
) RETURNS VOID
  AS 'pg_igraph', 'graph_delete_property'
  LANGUAGE C STRICT;

-- graph_get_property(node_id BIGINT, prop_name TEXT) removed here for the
-- same reason -- see note above.

-- graph_delete_node(node_id BIGINT) and its INT alias removed here for the
-- same reason -- see note above; graph_delete_node_extended with
-- table_prefix='' targets the same default tables.

-- ────────────────────────────────────────────────
-- Query language entry point (v1.0 compatibility)
-- ────────────────────────────────────────────────

-- Execute an igraph query string, returns JSONB result.
--
-- Examples:
--   SELECT igraph_query('MATCH (n:Category)-[:PARENT_OF*1..5]->(m:Product) WHERE n.id = 1 RETURN m.id, m.name');
--   SELECT igraph_query('PATH FROM 1 TO 99 VIA PARENT_OF');
--   SELECT igraph_query('CREATE (n:Product)');
--   SELECT igraph_query('CREATE (1)-[:PARENT_OF]->(2)');
--   SELECT igraph_query('DELETE NODE 42');
--   SELECT igraph_query('SET NODE 3 name = ''Galaxy S24''');
--   SELECT igraph_query('GET NODE 3 PROPERTIES');
CREATE FUNCTION igraph_query(query TEXT)
  RETURNS JSONB
  AS 'pg_igraph', 'igraph_query'
  LANGUAGE C STRICT;

-- ────────────────────────────────────────────────
-- NEW in v1.1: Extended query language with table prefixes and JSON parameters
-- ────────────────────────────────────────────────

-- Execute an igraph query string with table prefix and JSON parameters support
-- This allows working with multiple graph datasets in the same database and
-- passing dynamic parameters to queries.
--
-- Parameters:
--   table_prefix - Prefix for table names (e.g., 'users' creates 'users_nodes', 'users_edges', etc.)
--                  If empty string, uses default tables (nodes, edges, etc.)
--   query        - Query string with optional &data.param syntax for JSON parameter references
--   json_params  - JSONB object with parameters (e.g., '{"data":{"threshold":123}}'::jsonb)
--
-- Examples:
--   SELECT igraph_query('nodes', 'MATCH (n:User)-[:follows]->(m:User) WHERE m.influence > &data.threshold RETURN n.name', '{"data":{"threshold":100}}'::jsonb);
--   SELECT igraph_query('products', 'PATH FROM &data.start TO &data.end VIA RELATED', '{"data":{"start":1,"end":99}}'::jsonb);
--   SELECT igraph_query('social', 'CREATE (n:User)', NULL);
--   SELECT igraph_query('', 'PATH FROM 1 TO 2 VIA follows', NULL); -- Use default tables
-- Version with table prefix and JSON parameters (full version)
CREATE FUNCTION igraph_query(table_prefix TEXT, query TEXT, json_params JSONB)
  RETURNS JSONB
  AS 'pg_igraph', 'igraph_query_extended'
  LANGUAGE C;

-- Note: Single-parameter version already declared above

-- ────────────────────────────────────────────────
-- REF Resolution Functions (Ledgyx SQL extensions)
-- ────────────────────────────────────────────────

-- Resolve REF UUID to node data
-- Example: SELECT graph_resolve_ref('550e8400-e29b-41d4-a716-446655440000'::uuid, 'User');
CREATE FUNCTION graph_resolve_ref(ref_uuid UUID, ref_type TEXT)
  RETURNS JSONB
  AS 'pg_igraph', 'graph_resolve_ref'
  LANGUAGE C STRICT;

-- ────────────────────────────────────────────────
-- NEW in v1.2: Core API functions with table prefix support
-- ────────────────────────────────────────────────

-- Node operations with table prefix support
CREATE FUNCTION graph_add_node(label_name TEXT, table_prefix TEXT DEFAULT '')
  RETURNS BIGINT
  AS 'pg_igraph', 'graph_add_node_extended'
  LANGUAGE C STRICT;

CREATE FUNCTION graph_add_edge(
  from_id      BIGINT,
  to_id        BIGINT,
  rel_name     TEXT,
  table_prefix TEXT DEFAULT ''
) RETURNS VOID
  AS 'pg_igraph', 'graph_add_edge_extended'
  LANGUAGE C STRICT;

CREATE FUNCTION graph_delete_node(
  node_id      BIGINT,
  table_prefix TEXT DEFAULT ''
) RETURNS VOID
  AS 'pg_igraph', 'graph_delete_node_extended'
  LANGUAGE C STRICT;

-- Property functions with table prefix support
CREATE FUNCTION graph_set_property(
  node_id      BIGINT,
  prop_name    TEXT,
  primitive    SMALLINT,
  value        BYTEA,
  ref_label    TEXT DEFAULT NULL,
  table_prefix TEXT DEFAULT ''
) RETURNS VOID
  AS 'pg_igraph', 'graph_set_property_extended'
  LANGUAGE C;

CREATE FUNCTION graph_get_property(
  node_id      BIGINT,
  prop_name    TEXT,
  table_prefix TEXT DEFAULT ''
) RETURNS BYTEA
  AS 'pg_igraph', 'graph_get_property_extended'
  LANGUAGE C STRICT;

CREATE FUNCTION graph_get_node_properties(
  node_id      BIGINT,
  table_prefix TEXT DEFAULT ''
) RETURNS JSONB
  AS 'pg_igraph', 'graph_get_node_properties_extended'
  LANGUAGE C STRICT;

-- Note: Extended traversal functions are not yet implemented
-- TODO: Implement graph_traverse_extended, graph_shortest_path_extended

-- INT convenience aliases with table prefix support
CREATE FUNCTION graph_add_edge(
  from_id      INT,
  to_id        INT,
  rel_name     TEXT,
  table_prefix TEXT DEFAULT ''
) RETURNS VOID AS $$
  SELECT graph_add_edge(from_id::bigint, to_id::bigint, rel_name, table_prefix)
$$ LANGUAGE SQL STRICT;

CREATE FUNCTION graph_delete_node(
  node_id      INT,
  table_prefix TEXT DEFAULT ''
) RETURNS VOID AS $$
  SELECT graph_delete_node(node_id::bigint, table_prefix)
$$ LANGUAGE SQL STRICT;

-- INT convenience aliases with table prefix support
CREATE FUNCTION graph_get_property(
  node_id      INT,
  prop_name    TEXT,
  table_prefix TEXT DEFAULT ''
) RETURNS BYTEA AS $$
  SELECT graph_get_property(node_id::bigint, prop_name, table_prefix)
$$ LANGUAGE SQL STRICT;

CREATE FUNCTION graph_set_property(
  node_id      INT,
  prop_name    TEXT,
  primitive    SMALLINT,
  value        BYTEA,
  ref_label    TEXT DEFAULT NULL,
  table_prefix TEXT DEFAULT ''
) RETURNS VOID AS $$
  SELECT graph_set_property(node_id::bigint, prop_name, primitive, value, ref_label, table_prefix)
$$ LANGUAGE SQL;

CREATE FUNCTION graph_get_node_properties(
  node_id      INT,
  table_prefix TEXT DEFAULT ''
) RETURNS JSONB AS $$
  SELECT graph_get_node_properties(node_id::bigint, table_prefix)
$$ LANGUAGE SQL STRICT;


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
