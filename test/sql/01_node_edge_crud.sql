-- Base API: graph_add_node / graph_add_edge / graph_delete_node /
-- graph_set_property / graph_get_property / graph_get_node_properties /
-- graph_delete_property, over the default (unprefixed) graph only.
-- Prefixed-graph variants of these same calls are covered in
-- 05_match_prefixed.sql (which also exercises node/edge CRUD, not just MATCH).

-- ============================================================
-- graph_add_node / graph_add_edge
-- ============================================================
SELECT graph_add_node('User') AS alice_id \gset
SELECT graph_add_node('User') AS bob_id \gset
SELECT graph_add_node('Product') AS widget_id \gset

SELECT graph_add_edge(:alice_id, :bob_id, 'follows');
SELECT graph_add_edge(:alice_id, :widget_id, 'likes');

-- ============================================================
-- graph_set_property / graph_get_node_properties: all 6 primitives
--
-- NOTE on NUMERIC/BIGINT (op_id 0x02, pg_igraph.c ~1749/2139-2180):
-- both share one wire op and decode_node_properties_jsonb() currently
-- returns them as a *hex string* of the raw GMP payload bytes, not a JSON
-- number — this is documented as deliberate-for-now in the source ("client
-- decodes with pg_ilib bytea_to_numeric()"). Pinning that here rather than
-- asserting a JSON number, since that's what the code actually does today.
-- TIMESTAMP (op 0x04) by contrast *does* decode to a real JSON number
-- (epoch seconds) — an asymmetry worth knowing about, not a bug to fix here.
-- ============================================================
SELECT graph_set_property(:alice_id, 'name', 2::smallint, str_to_bytea('Alice'));
SELECT graph_set_property(:alice_id, 'age', 1::smallint, bigint_to_bytea(42::bigint));
SELECT graph_set_property(:alice_id, 'verified', 5::smallint, bool_to_bytea(true));
SELECT graph_set_property(:alice_id, 'score', 6::smallint, numeric_to_bytea(3.14::numeric));
SELECT graph_set_property(:alice_id, 'joined', 4::smallint,
       timestamp_to_bytea(extract(epoch from timestamp '2026-01-01 00:00:00')::bigint));
SELECT graph_set_property(:alice_id, 'ext_id', 3::smallint,
       uuid_to_bytea('550e8400-e29b-41d4-a716-446655440000'::uuid));

SELECT graph_get_node_properties(:alice_id);

-- graph_get_property: raw BYTEA, undecoded — exact equality against the
-- same encoder call proves it round-trips the bytes untouched.
SELECT graph_get_property(:alice_id, 'name') = str_to_bytea('Alice') AS name_bytes_roundtrip;

-- ============================================================
-- graph_delete_property
-- ============================================================
SELECT graph_delete_property(:alice_id, 'verified');
SELECT graph_get_node_properties(:alice_id) ? 'verified' AS verified_still_present;

-- ============================================================
-- graph_delete_node — cascades edges (both directions) and properties
-- ============================================================
SELECT graph_add_node('User') AS carol_id \gset
SELECT graph_add_edge(:carol_id, :alice_id, 'follows');
SELECT graph_set_property(:carol_id, 'name', 2::smallint, str_to_bytea('Carol'));

SELECT graph_delete_node(:carol_id);

SELECT count(*) AS carol_out_edges FROM edges WHERE from_id = :carol_id;
SELECT count(*) AS carol_in_edges  FROM edges WHERE to_id   = :carol_id;
SELECT count(*) AS carol_props     FROM node_properties WHERE node_id = :carol_id;
SELECT count(*) AS carol_node_row  FROM nodes WHERE id = :carol_id;

-- ============================================================
-- INT-overload wrapper forms (pg_igraph--1.1.sql casts int->bigint) —
-- bare integer literals are int4 already, so these dispatch to the
-- INT-arg wrapper overloads without any explicit cast.
-- ============================================================
SELECT graph_add_node('User') AS dave_id \gset
SELECT graph_add_node('User') AS erin_id \gset
-- dave_id/erin_id are session \gset bigint values; cast down to int4 to
-- actually exercise the INT overloads (they're small in a fresh test db).
SELECT graph_add_edge(:dave_id::int, :erin_id::int, 'follows');
SELECT graph_set_property(:dave_id::int, 'name', 2::smallint, str_to_bytea('Dave'));
SELECT graph_get_property(:dave_id::int, 'name');
SELECT graph_get_node_properties(:dave_id::int);
SELECT graph_delete_node(:erin_id::int);
SELECT count(*) AS erin_node_row FROM nodes WHERE id = :erin_id;
