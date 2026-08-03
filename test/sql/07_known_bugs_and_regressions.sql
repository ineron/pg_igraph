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
-- KNOWN BUG (unfiled, high severity): SET NODE <id> <prop> = <literal>
-- is broken for *every* literal type right now. exec_set_prop_ctx
-- (igraph_exec.c ~1344-1441) builds "SELECT graph_set_property($1,$2,$3,$4)"
-- binding $3 (a type-name string like "int64") and $4 (a decimal-string
-- value) as TEXT -- but the only registered graph_set_property overloads
-- take (SMALLINT primitive, BYTEA value); no (bigint,text,text,text)
-- overload exists. STRING/INT literals hit "function ... does not exist";
-- FLOAT/BOOL/NULL/&param fall through the type switch's unhandled branch
-- to "igraph SET: unsupported value type". There is currently no way to
-- reach a working call through the SET grammar at all.
-- ============================================================
SELECT graph_add_node('SetTest') AS st1 \gset
SELECT igraph_query(format('SET NODE %s greeting = ''hello''', :st1));
SELECT igraph_query(format('SET NODE %s age = 99', :st1));
SELECT igraph_query(format('SET NODE %s ratio = 3.5', :st1));
SELECT igraph_query(format('SET NODE %s active = true', :st1));

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
-- KNOWN BUG (unfiled, high severity): BIGINT/NUMERIC properties (op_id
-- 0x02) are silently unusable in WHERE/RETURN. graph_get_node_properties'
-- decode_node_properties_jsonb() intentionally returns them as a hex
-- string of the raw GMP payload (see 01_node_edge_crud.sql) -- but
-- get_node_property_value() (igraph_exec.c ~484-576), used by every WHERE
-- condition and RETURN field, re-parses *that hex string* with
-- strtoll()/strtod() as if it were a base-10 number. Whatever hex digits
-- happen to come out (a property value's own encoding, nothing to do with
-- its real magnitude) either fail to parse (silently become a STRING) or
-- parse into a fabricated, wrong INT/FLOAT that looks entirely plausible.
-- Concrete repro: age=30 stored via bigint_to_bytea -> raw bytes 0x20 0x1e
-- 0x00 -> decode_node_properties_jsonb gives the hex string "00" (1
-- trailing GMP byte) -> get_node_property_value's strtoll("00", 10)
-- succeeds fully, producing the *integer 0* -- not 30, not an error, no
-- hex-string tell -- just silently the wrong number. Every ordering
-- comparison against age is therefore comparing against 0, not the real
-- value.
-- ============================================================
SELECT graph_add_node('NumBug') AS nb1 \gset
SELECT graph_add_node('NumBug') AS nb2 \gset
SELECT graph_add_edge(:nb1, :nb2, 'has');
SELECT graph_set_property(:nb2, 'age', 1::smallint, bigint_to_bytea(30::bigint));
SELECT encode(graph_get_property(:nb2, 'age'), 'hex') AS raw_bytes_for_30;
SELECT graph_get_node_properties(:nb2);
SELECT igraph_query(format('MATCH (n:NumBug)-[:has]->(m:NumBug) WHERE n.id = %s RETURN m.age', :nb1));
SELECT igraph_query(format('MATCH (n:NumBug)-[:has]->(m:NumBug) WHERE n.id = %s AND m.age = 30 RETURN m.age', :nb1));

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
