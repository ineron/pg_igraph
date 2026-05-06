-- pg_igraph--1.0.sql
-- Native graph traversal extension for PostgreSQL
-- Do not edit manually — managed by init_graph.sh

-- ────────────────────────────────────────────────
-- Node operations
-- ────────────────────────────────────────────────

CREATE FUNCTION graph_add_node(label_name TEXT)
  RETURNS BIGINT
  AS 'pg_igraph', 'graph_add_node'
  LANGUAGE C STRICT;

-- ────────────────────────────────────────────────
-- Edge operations
-- ────────────────────────────────────────────────

CREATE FUNCTION graph_add_edge(
  from_id   BIGINT,
  to_id     BIGINT,
  rel_name  TEXT
) RETURNS VOID
  AS 'pg_igraph', 'graph_add_edge'
  LANGUAGE C STRICT;

-- ────────────────────────────────────────────────
-- Traversal
-- ────────────────────────────────────────────────

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

-- ────────────────────────────────────────────────
-- INT convenience aliases
-- PostgreSQL treats integer literals as INT4, not INT8.
-- These wrappers allow calling functions without explicit ::bigint casts.
-- ────────────────────────────────────────────────

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

CREATE FUNCTION graph_add_edge(
  from_id  INT,
  to_id    INT,
  rel_name TEXT
) RETURNS VOID AS $$
  SELECT graph_add_edge(from_id::bigint, to_id::bigint, rel_name)
$$ LANGUAGE SQL STRICT;

CREATE FUNCTION graph_get_property(
  node_id   INT,
  prop_name TEXT
) RETURNS BYTEA AS $$
  SELECT graph_get_property(node_id::bigint, prop_name)
$$ LANGUAGE SQL STRICT;

CREATE FUNCTION graph_set_property(
  node_id    INT,
  prop_name  TEXT,
  primitive  SMALLINT,
  value      BYTEA,
  ref_label  TEXT DEFAULT NULL
) RETURNS VOID AS $$
  SELECT graph_set_property(node_id::bigint, prop_name, primitive, value, ref_label)
$$ LANGUAGE SQL;

CREATE FUNCTION graph_get_node_properties(
  node_id INT
) RETURNS JSONB AS $$
  SELECT graph_get_node_properties(node_id::bigint)
$$ LANGUAGE SQL STRICT;

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

-- ────────────────────────────────────────────────
-- Properties
-- ────────────────────────────────────────────────

-- primitive типы:
--   1 = bigint
--   2 = text
--   3 = uuid
--   4 = timestamp
--   5 = bool
--   6 = numeric
--   7 = jsonb

-- Записать свойство узла
-- prop_name  — имя свойства (создаётся автоматически)
-- primitive  — тип значения (1-7)
-- value      — значение в бинарном формате (BYTEA)
-- ref_label  — если это typed ref: имя label на который ссылаемся
--              передать NULL если обычное свойство
CREATE FUNCTION graph_set_property(
  node_id    BIGINT,
  prop_name  TEXT,
  primitive  SMALLINT,
  value      BYTEA,
  ref_label  TEXT DEFAULT NULL
) RETURNS VOID
  AS 'pg_igraph', 'graph_set_property'
  LANGUAGE C;

-- Прочитать свойство узла
-- Возвращает BYTEA — декодируется на стороне клиента
-- или через graph_get_property_text / graph_get_property_int
CREATE FUNCTION graph_get_property(
  node_id   BIGINT,
  prop_name TEXT
) RETURNS BYTEA
  AS 'pg_igraph', 'graph_get_property'
  LANGUAGE C STRICT;

-- Удалить свойство узла
CREATE FUNCTION graph_delete_property(
  node_id   BIGINT,
  prop_name TEXT
) RETURNS VOID
  AS 'pg_igraph', 'graph_delete_property'
  LANGUAGE C STRICT;

-- Получить все свойства узла как JSONB
CREATE FUNCTION graph_get_node_properties(
  node_id BIGINT
) RETURNS JSONB
  AS 'pg_igraph', 'graph_get_node_properties'
  LANGUAGE C STRICT;

-- ────────────────────────────────────────────────
-- Node deletion
-- ────────────────────────────────────────────────

-- Delete a node and all its edges, properties atomically.
-- Also removes dangling reverse edges from neighbours.
CREATE FUNCTION graph_delete_node(node_id BIGINT)
  RETURNS VOID
  AS 'pg_igraph', 'graph_delete_node'
  LANGUAGE C STRICT;

-- INT alias
CREATE FUNCTION graph_delete_node(node_id INT)
  RETURNS VOID AS $$
  SELECT graph_delete_node(node_id::bigint)
$$ LANGUAGE SQL STRICT;

-- ────────────────────────────────────────────────
-- Query language entry point
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
