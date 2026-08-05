-- graph_traverse / graph_shortest_path / PATH FROM..TO..VIA (query language),
-- over the default (unprefixed) graph only — graph_shortest_path() itself
-- is still not prefix-aware (pg_igraph--1.1.sql explicit TODO), but the
-- query-language PATH statement now has its own prefixed-graph CTE path
-- (task #15, exec_path_ctx); that behavior is covered separately in
-- 07_known_bugs_and_regressions.sql's REGRESSION ANCHORS section.
--
-- Fixture: a 4-node chain a -[follows]-> b -[follows]-> c -[follows]-> d

SELECT graph_add_node('User') AS a \gset
SELECT graph_add_node('User') AS b \gset
SELECT graph_add_node('User') AS c \gset
SELECT graph_add_node('User') AS d \gset
SELECT graph_add_edge(:a, :b, 'follows');
SELECT graph_add_edge(:b, :c, 'follows');
SELECT graph_add_edge(:c, :d, 'follows');

-- ============================================================
-- graph_traverse(start_id, rel_name, direction, max_depth) -> SETOF BIGINT
--
-- direction=true walks stored forward edges (direction column = true);
-- direction=false walks the reverse-lookup rows dual-storage keeps
-- alongside them. The result *includes the start node itself at depth 0*
-- -- that is graph_traverse's own contract, not a bug: exec_match_ctx
-- separately strips the anchor's own id back out when building MATCH's
-- "m" results (igraph_exec.c ~958), which is a different layer.
-- ============================================================
SELECT * FROM graph_traverse(:a, 'follows', true, 10) ORDER BY 1;
SELECT * FROM graph_traverse(:a, 'follows', true, 1) ORDER BY 1;
SELECT * FROM graph_traverse(:d, 'follows', false, 10) ORDER BY 1;
SELECT * FROM graph_traverse(:d, 'follows', true, 10) ORDER BY 1;

-- ============================================================
-- graph_shortest_path(start, end, rel) -> BIGINT[]
-- Returns NULL (not an empty array) when no path exists.
-- ============================================================
SELECT graph_shortest_path(:a, :d, 'follows') AS a_to_d;
SELECT graph_shortest_path(:a, :a, 'follows') AS trivial_self_path;
SELECT graph_shortest_path(:d, :a, 'follows') AS d_to_a_no_path;
SELECT graph_shortest_path(:d, :a, 'follows') IS NULL AS d_to_a_is_null;

-- unknown rel_type raises a hard SQL error, not a JSON/NULL result
SELECT graph_shortest_path(:a, :d, 'no_such_rel');

-- ============================================================
-- PATH FROM <int> TO <int> VIA <rel> (query language, default/no prefix)
-- ============================================================
SELECT igraph_query(format('PATH FROM %s TO %s VIA follows', :a, :d));
SELECT igraph_query(format('PATH FROM %s TO %s VIA follows', :d, :a));

-- ============================================================
-- INT-overload wrapper forms (pg_igraph--1.1.sql:42,51)
-- ============================================================
SELECT * FROM graph_traverse(:a::int, 'follows', true, 10) ORDER BY 1;
SELECT graph_shortest_path(:a::int, :d::int, 'follows');
