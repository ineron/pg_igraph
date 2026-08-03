-- Known-bug pins: this file intentionally encodes *current, broken*
-- behavior with a comment explaining what's wrong. The point is a loud,
-- specific diff the day someone fixes the underlying code -- not a
-- permanently-red suite (see decision recorded when this suite was built:
-- pin behavior + comment, don't leave bugs uncovered or assert the fix
-- before it exists).
--
-- Second half of the file is regression anchors: one query per bug fixed
-- in the last several commits, so a future change that reintroduces any
-- of them fails loudly here instead of waiting to be rediscovered by hand.

-- ============================================================
-- KNOWN BUG (task #15): PATH FROM/TO/VIA over a prefixed graph is a
-- hardcoded stub -- exec_path_ctx (igraph_exec.c ~1127-1149) never runs
-- the real BFS for a non-empty table_prefix; it always returns this
-- exact object regardless of whether a path actually exists.
-- ============================================================
SELECT graph_add_node('Customer', 'shop_') AS pc1 \gset
SELECT graph_add_node('Customer', 'shop_') AS pc2 \gset
SELECT graph_add_edge(:pc1, :pc2, 'refers', 'shop_');
SELECT igraph_query('shop_', format('PATH FROM %s TO %s VIA refers', :pc1, :pc2), NULL::jsonb);

-- ============================================================
-- KNOWN BUG (task #17): MATCH with no WHERE anchor doesn't traverse from
-- each label-matching node at all -- exec_match_ctx's start_id<0 branch
-- (igraph_exec.c ~916-931) does a label scan and treats every matching
-- node as *both* n and m directly (no actual edge walk), and dst-alias
-- WHERE/RETURN fields silently resolve to null/unenforced (resolve_
-- match_alias_id, igraph_exec.c ~751-752). Both rows below show n and m
-- as the *same* node (U1/U1, U2/U2) instead of the real U1->U2 edge, and
-- m.name is null even though a WHERE m.name filter is present.
-- ============================================================
SELECT graph_add_node('Unanchored') AS u1 \gset
SELECT graph_add_node('Unanchored') AS u2 \gset
SELECT graph_add_edge(:u1, :u2, 'link');
SELECT graph_set_property(:u1, 'name', 2::smallint, str_to_bytea('U1'));
SELECT graph_set_property(:u2, 'name', 2::smallint, str_to_bytea('U2'));
SELECT igraph_query('MATCH (n:Unanchored)-[:link]->(m:Unanchored) RETURN n.name, m.name');
SELECT igraph_query('MATCH (n:Unanchored)-[:link]->(m:Unanchored) WHERE m.name = ''U2'' RETURN n.name, m.name');

-- ============================================================
-- KNOWN BUG (unfiled): relationship-alias syntax -[e:REL]-> does not
-- parse -- rel_pattern (igraph_parser.y ~292-315) has no alias slot,
-- despite docs/HOTFIX_v1.1.1.md claiming it was fixed and several sql/
-- fixtures assuming it works.
-- ============================================================
SELECT igraph_query('MATCH (n:Param)-[e:rel]->(m:Param) WHERE n.id = 1 RETURN e');

-- ============================================================
-- KNOWN BUG (unfiled): CREATE (n:Label REF Type = <literal>) parses --
-- has_ref/ref_type/ref_value are populated on the AST (igraph_parser.y
-- ~411-421) -- but exec_create_node_ctx (igraph_exec.c ~1152-1198) never
-- reads any of those fields; it always runs plain graph_add_node(label).
-- The REF clause is silently a no-op: the node is created with no
-- properties at all, no error, no ref data recorded anywhere.
-- ============================================================
SELECT igraph_query('CREATE (n:RefTest REF Order = ''abc-123'')');
SELECT (igraph_query('CREATE (n:RefTest REF Order = ''xyz-789'')') ->> 'node_id')::bigint AS reftest_id \gset
SELECT graph_get_node_properties(:reftest_id);

-- ============================================================
-- KNOWN BUG (unfiled): PATH FROM/TO only accepts a literal TK_INTEGER
-- (igraph_parser.y node_id rule ~518) -- a &param there is a hard parse
-- error, despite pg_igraph--1.1.sql's own doc comment (line ~168) and
-- pg_igraph--1.0--1.1.sql (line ~20) advertising parameterized PATH.
-- ============================================================
SELECT igraph_query('PATH FROM &data.a TO &data.b VIA rel');

-- ============================================================
-- KNOWN BUG (unfiled): relationship depth minimum bound is parsed but
-- never enforced -- exec_match_ctx only ever reads rel->max_depth
-- (igraph_exec.c:843), never rel->min_depth. *2 (parsed as min=2,max=2,
-- "exactly 2 hops") actually returns *every* node from 1 hop up to 2,
-- identical to *1..2. Fixture: d1 -[:chain]-> d2 -[:chain]-> d3; *2 from
-- d1 should return only d3 (2 hops) but returns both d2 (1 hop) and d3.
-- ============================================================
SELECT graph_add_node('Depth') AS d1 \gset
SELECT graph_add_node('Depth') AS d2 \gset
SELECT graph_add_node('Depth') AS d3 \gset
SELECT graph_add_edge(:d1, :d2, 'chain');
SELECT graph_add_edge(:d2, :d3, 'chain');
SELECT graph_set_property(:d2, 'name', 2::smallint, str_to_bytea('D2'));
SELECT graph_set_property(:d3, 'name', 2::smallint, str_to_bytea('D3'));
SELECT igraph_query(format('MATCH (n:Depth)-[:chain*2]->(m:Depth) WHERE n.id = %s RETURN m.name', :d1));

-- ============================================================
-- KNOWN BUG (task #9): a jsonb-typed property (pg_ilib's jsonb_to_bytea())
-- always decodes to an empty object -- pg_igraph looks up the complex-type
-- registry using params=0 as if it were a real complex_types.id, finds no
-- fields, and "correctly" (per the code's own comment) returns {}. This is
-- a protocol mismatch between pg_ilib and pg_igraph's complex-type system,
-- not something fixable by changing a constant.
-- ============================================================
SELECT graph_add_node('JsonbTest') AS j1 \gset
SELECT graph_set_property(:j1, 'meta', 7::smallint, jsonb_to_bytea('{"a":1,"b":"x"}'::jsonb));
SELECT graph_get_node_properties(:j1);

-- ============================================================
-- KNOWN BUG (task #19, upstream): bigint_to_bytea() in pg_ilib 1.5 does
-- not match its own documented header/encoding convention -- it writes
-- only a 1-byte header (hardcoded 0x20, silently dropping the `scale`
-- argument entirely) followed by an off-by-one magnitude loop that
-- leaves one trailing byte uninitialized, unlike numeric_to_bytea()
-- (used for both INT and FLOAT properties everywhere else in this
-- suite), which correctly writes a real 2-byte op_id/params header. The
-- two encoders are mutually inconsistent: even pg_ilib's own
-- bytea_to_bigint()/bytea_to_numeric() cannot reliably decode
-- bigint_to_bytea()'s output (confirmed directly:
-- `SELECT bytea_to_bigint(bigint_to_bytea(30::bigint))` throws
-- "numeric scale 30 is impossible for 1 payload byte(s)" in a bare
-- psql session against pg_ilib alone, nothing pg_igraph-specific). This
-- is a pg_ilib defect, filed there (not fixable from this repo) --
-- pg_igraph's own fix below (see REGRESSION ANCHORS) at least turns the
-- previous *silent wrong number* into a loud, specific error for data
-- written this way, rather than pretending to support it.
-- ============================================================
SELECT graph_add_node('NumBug') AS nb1 \gset
SELECT graph_add_node('NumBug') AS nb2 \gset
SELECT graph_add_edge(:nb1, :nb2, 'has');
SELECT graph_set_property(:nb2, 'age', 1::smallint, bigint_to_bytea(30::bigint));
SELECT encode(graph_get_property(:nb2, 'age'), 'hex') AS raw_bytes_for_30;
SELECT graph_get_node_properties(:nb2);
SELECT igraph_query(format('MATCH (n:NumBug)-[:has]->(m:NumBug) WHERE n.id = %s RETURN m.age', :nb1));

-- ============================================================
-- REGRESSION ANCHORS -- confirmed-fixed bugs from the last several
-- commits. A future change that reintroduces any of these must fail here.
-- ============================================================

-- fe7e1fd: property-decoder OOB reads / ILIB_OP_BOOL 0x05->0x03 / NUMERIC
-- prefix confusion -- covered by 01_node_edge_crud.sql's full-primitive
-- round trip (text/bool/uuid/timestamp all decode correctly, no crash).

-- f172a5b: RETURN projection was a dead TODO; graph_traverse's depth-0
-- self-row leaked into MATCH's "m" results; WHERE conditions always
-- evaluated against src regardless of alias -- covered by
-- 04_match_where_return.sql (RETURN projection, dst-alias WHERE) and
-- 02_traversal_and_path.sql (graph_traverse's own depth-0 contract).

-- f68907e: CStringGetDatum instead of CStringGetTextDatum for a TEXTOID
-- SPI param in get_node_property_value -> SIGSEGV on RETURN <alias>.<prop>.
-- The exact historically-crashing shape: anchored MATCH, RETURN of a
-- dst-alias TEXT property.
SELECT graph_add_node('CrashRegress') AS cr1 \gset
SELECT graph_add_node('CrashRegress') AS cr2 \gset
SELECT graph_add_edge(:cr1, :cr2, 'follows');
SELECT graph_set_property(:cr2, 'name', 2::smallint, str_to_bytea('Bob'));
SELECT igraph_query(format('MATCH (n:CrashRegress)-[:follows]->(m:CrashRegress) WHERE n.id = %s RETURN m.name', :cr1));

-- f68907e (second half): graph_get_property's BYTEA return used to be read
-- via TextDatumGetCString() as if already-decoded text, producing garbage
-- instead of crashing. graph_get_node_properties must still give the real
-- decoded value.
SELECT graph_get_node_properties(:cr2) ->> 'name' = 'Bob' AS name_decodes_correctly;

-- f68907e (overload ambiguity): the duplicate int/bigint overloads for
-- graph_add_edge/graph_get_property/graph_set_property/
-- graph_get_node_properties/graph_delete_node were removed. Confirm the
-- BIGINT-only call sites used throughout this suite are unambiguous.
SELECT count(*) AS graph_add_edge_overload_count
  FROM pg_proc WHERE proname = 'graph_add_edge';

-- task #19: get_node_property_value() (igraph_exec.c) used to re-parse
-- graph_get_node_properties()'s hex-of-raw-GMP-bytes string as base-10,
-- silently turning any NUMERIC/BIGINT property (age=30 encoded via
-- numeric_to_bytea(30,0)) into a plausible-looking wrong number (0) in
-- both RETURN and WHERE. Fixed by decoding NUMERIC-op properties via
-- pg_ilib's own bytea_to_numeric() instead, keyed off the property's
-- real header byte rather than its already-hex-rendered text form.
SELECT graph_add_node('NumFixed') AS nf1 \gset
SELECT graph_add_node('NumFixed') AS nf2 \gset
SELECT graph_add_edge(:nf1, :nf2, 'has');
SELECT graph_set_property(:nf2, 'age', 6::smallint, numeric_to_bytea(30::numeric, 0));
SELECT graph_set_property(:nf2, 'score', 6::smallint, numeric_to_bytea(3.14::numeric, 2));
SELECT igraph_query(format('MATCH (n:NumFixed)-[:has]->(m:NumFixed) WHERE n.id = %s RETURN m.age, m.score', :nf1));
SELECT igraph_query(format('MATCH (n:NumFixed)-[:has]->(m:NumFixed) WHERE n.id = %s AND m.age = 30 RETURN m.age', :nf1));
SELECT igraph_query(format('MATCH (n:NumFixed)-[:has]->(m:NumFixed) WHERE n.id = %s AND m.age > 29 AND m.age < 31 RETURN m.age', :nf1));

-- task #18: exec_set_prop_ctx (igraph_exec.c) built
-- "SELECT graph_set_property($1,$2,$3,$4)" binding a type-name string and
-- a decimal-string value as TEXT, but the only registered overloads take
-- (SMALLINT primitive, BYTEA value) -- every literal type failed. Fixed
-- by encoding through pg_ilib's own str_to_bytea/numeric_to_bytea/
-- bool_to_bytea and calling the real overload. FLOAT went through a
-- second bug on the way: a hardcoded scale=2 in the header mismatched the
-- GMP payload's actual digit count and silently turned 3.5 into 0.35 on
-- decode -- fixed by deriving the header's scale from scale($1::numeric)
-- instead of guessing a fixed value.
SELECT graph_add_node('SetFixed') AS sf1 \gset
SELECT igraph_query(format('SET NODE %s greeting = ''hello''', :sf1));
SELECT igraph_query(format('SET NODE %s age = 99', :sf1));
SELECT igraph_query(format('SET NODE %s ratio = 3.14159', :sf1));
SELECT igraph_query(format('SET NODE %s active = true', :sf1));
SELECT pt.name, bytea_to_numeric(np.value) AS decoded
  FROM node_properties np JOIN property_types pt ON pt.id = np.prop_id
  WHERE np.node_id = :sf1 AND pt.name IN ('age', 'ratio')
  ORDER BY pt.name;
SELECT graph_get_node_properties(:sf1) ->> 'greeting' AS greeting,
       (graph_get_node_properties(:sf1) ->> 'active')::boolean AS active;
SELECT igraph_query(format('SET NODE %s greeting = NULL', :sf1));
SELECT graph_get_node_properties(:sf1) - 'age' - 'ratio' - 'active' AS greeting_deleted;
