-- Bare single-node MATCH (task #14, filed from ledgyx-admin-ui,
-- 2026-08-02): before this fixture, the grammar required a full
-- src-rel-dst triple for every MATCH -- "MATCH (n) RETURN n" and
-- "MATCH (n:Label) RETURN n" had no rule at all. Covers the new
-- exec_match_bare_ctx() path (igraph_exec.c), which enumerates
-- candidates directly (by WHERE id=, by label, or -- with neither --
-- every node) instead of walking an edge from an anchor.
--
-- Fixture: two BareA nodes (Ann, Bo), one BareB node (Cy)

SELECT MAX(id) AS max_before FROM nodes \gset

SELECT graph_add_node('BareA') AS ann \gset
SELECT graph_add_node('BareA') AS bo  \gset
SELECT graph_add_node('BareB') AS cy  \gset

SELECT graph_set_property(:ann, 'name', 2::smallint, str_to_bytea('Ann'));
SELECT graph_set_property(:bo,  'name', 2::smallint, str_to_bytea('Bo'));
SELECT graph_set_property(:cy,  'name', 2::smallint, str_to_bytea('Cy'));

-- ============================================================
-- Label filter + RETURN projection (id, prop)
-- ============================================================
SELECT igraph_query('MATCH (n:BareA) RETURN n.id, n.name');

-- bare alias with no prop -> just the id
SELECT igraph_query('MATCH (n:BareA) RETURN n');

-- ============================================================
-- WHERE n.id = <int> narrows enumeration to a single node
-- ============================================================
SELECT igraph_query(format('MATCH (n:BareA) WHERE n.id = %s RETURN n.name', :ann));

-- id filter + non-matching label -> no rows
SELECT igraph_query(format('MATCH (n:BareB) WHERE n.id = %s RETURN n.name', :ann));

-- ============================================================
-- No RETURN clause -- default shape {"id","label"}
-- ============================================================
SELECT igraph_query(format('MATCH (n:BareB) WHERE n.id = %s', :cy));

-- ============================================================
-- Label with no matching nodes -> bare {} object, not []
-- ============================================================
SELECT igraph_query('MATCH (n:NoSuchBareLabel) RETURN n');

-- ============================================================
-- WHERE on a non-id property (name), resolved per-candidate
-- ============================================================
SELECT igraph_query('MATCH (n:BareA) WHERE n.name = ''Bo'' RETURN n.id, n.name');

-- ============================================================
-- No label, no id-eq WHERE: enumerates every node in the graph, then
-- applies the WHERE filter generically per-candidate (id > a fixture-
-- relative baseline keeps this deterministic across the whole suite)
-- ============================================================
SELECT igraph_query(format('MATCH (n) WHERE n.id > %s RETURN n.id, n.name', COALESCE(:max_before, 0)));
