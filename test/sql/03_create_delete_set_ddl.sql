-- Query-language DDL statements over the default graph: CREATE node/edge,
-- DELETE NODE, DELETE EDGE, GET NODE PROPERTIES; plus the complex-type
-- registry (graph_add_complex_type/_field/get_complex_fields) and
-- graph_resolve_ref, both of which are independent of the parser.
--
-- SET NODE is deliberately NOT exercised here as a working case — see
-- 07_known_bugs_and_regressions.sql: exec_set_prop_ctx (igraph_exec.c
-- ~1344-1441) currently calls "SELECT graph_set_property($1,$2,$3,$4)"
-- with $3/$4 bound as TEXT (a type-name string, a decimal-string value),
-- but the only registered graph_set_property overloads take
-- (SMALLINT primitive, BYTEA value) — that 4-arg (bigint,text,text,text)
-- shape does not exist. Every SET NODE statement currently fails.

-- ============================================================
-- CREATE (n:Label) / CREATE (:Label) -- alias is parsed but unused
-- ============================================================
SELECT igraph_query('CREATE (n:Widget)');
SELECT igraph_query('CREATE (:Widget)');

-- ============================================================
-- CREATE (from)-[:REL]->(to) / DELETE EDGE FROM .. TO .. VIA ..
-- ============================================================
SELECT graph_add_node('Widget') AS w1 \gset
SELECT graph_add_node('Widget') AS w2 \gset

SELECT igraph_query(format('CREATE (%s)-[:PARENT_OF]->(%s)', :w1, :w2));
SELECT count(*) AS edge_exists FROM edges WHERE from_id = :w1 AND to_id = :w2;

SELECT igraph_query(format('DELETE EDGE FROM %s TO %s VIA PARENT_OF', :w1, :w2));
SELECT count(*) AS edge_gone FROM edges WHERE from_id = :w1 AND to_id = :w2;

-- DELETE requires the literal word EDGE (runtime strcasecmp, igraph_parser.y ~464)
SELECT igraph_query(format('DELETE BOGUS FROM %s TO %s VIA PARENT_OF', :w1, :w2));

-- ============================================================
-- GET NODE <id> PROPERTIES
-- ============================================================
SELECT igraph_query(format('GET NODE %s PROPERTIES', :w1));
SELECT graph_set_property(:w1, 'name', 2::smallint, str_to_bytea('Gadget'));
SELECT igraph_query(format('GET NODE %s PROPERTIES', :w1));

-- ============================================================
-- DELETE NODE <id>
-- ============================================================
SELECT igraph_query(format('DELETE NODE %s', :w2));
SELECT count(*) AS w2_gone FROM nodes WHERE id = :w2;

-- ============================================================
-- Complex-type registry: graph_add_complex_type / graph_add_complex_field /
-- graph_get_complex_fields. Never prefix-aware (pg_igraph.c:1812-1859) --
-- one shared registry regardless of which graph a node belongs to.
-- ============================================================
SELECT graph_add_complex_type('Money') AS money_type \gset
SELECT graph_add_complex_field(:money_type::smallint, 0::smallint, 'amount');
SELECT graph_add_complex_field(:money_type::smallint, 1::smallint, 'currency');
SELECT * FROM graph_get_complex_fields(:money_type::smallint);

-- registering the same type name again is an upsert: same id comes back
SELECT graph_add_complex_type('Money') = :money_type AS type_add_idempotent;

-- a field for an unregistered type_id is a hard error
SELECT graph_add_complex_field(999::smallint, 0::smallint, 'x');

-- ============================================================
-- graph_resolve_ref(uuid, ref_type) -> JSONB
--
-- Independent of the CREATE ... REF ... grammar clause (that clause is
-- parsed but never read by exec_create_node_ctx -- see
-- 07_known_bugs_and_regressions.sql). This is a separate, manual
-- convention: a property literally named 'ref_uuid' (primitive=3/UUID)
-- whose value is the *raw* uuid_send() bytes -- NOT pg_ilib's
-- uuid_to_bytea() header-wrapped form, since graph_resolve_ref's internal
-- lookup (pg_igraph.c ~2682-2698) compares np.value directly against
-- uuid_send() output.
--
-- Result shape is {"id":.., "type":..} only -- despite the function's own
-- doc comment (pg_igraph.c:2644) advertising {"id","label","properties"},
-- the actual code (pg_igraph.c:2721-2736) never adds "label" or
-- "properties" keys.
-- ============================================================
SELECT graph_add_node('Customer') AS cust_id \gset
SELECT graph_set_property(:cust_id, 'ref_uuid', 3::smallint,
       uuid_send('550e8400-e29b-41d4-a716-446655440000'::uuid), 'Customer');

SELECT graph_resolve_ref('550e8400-e29b-41d4-a716-446655440000'::uuid, 'Customer');
SELECT graph_resolve_ref('00000000-0000-0000-0000-000000000000'::uuid, 'Customer') IS NULL AS no_match_is_null;
SELECT graph_resolve_ref('550e8400-e29b-41d4-a716-446655440000'::uuid, 'NoSuchLabel') IS NULL AS unknown_ref_type_is_null;
