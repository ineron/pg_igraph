-- Anchored MATCH ( WHERE <alias>.id = <int|&param> is present, which is the
-- supported/working case -- see 07_known_bugs_and_regressions.sql for the
-- unanchored-MATCH pin) over the default graph: WHERE operators, RETURN
-- projection forms, result-shape contracts, rel-depth ranges, direction.
--
-- WHERE/RETURN against BIGINT/NUMERIC properties (e.g. an "age" property)
-- are deliberately NOT exercised here -- get_node_property_value()
-- (igraph_exec.c ~484-576) re-parses the hex-string NUMERIC/BIGINT decode
-- (see 01_node_edge_crud.sql's note) with strtoll()/strtod(), which silently
-- produces a garbage number for almost any real value (e.g. hex "1e00"
-- parses via strtod's scientific notation as 1.0). That's pinned as a
-- concrete known bug in 07_known_bugs_and_regressions.sql. Every WHERE/
-- RETURN case below therefore uses only "id" (special-cased, never goes
-- through property decode) and TEXT properties (decode correctly, per
-- 01_node_edge_crud.sql).
--
-- Fixture: alice -[:follows]-> bob -[:follows]-> carol -[:follows]-> dave
--          alice -[:follows]-> eve

SELECT graph_add_node('Person') AS alice \gset
SELECT graph_add_node('Person') AS bob \gset
SELECT graph_add_node('Person') AS carol \gset
SELECT graph_add_node('Person') AS dave \gset
SELECT graph_add_node('Person') AS eve \gset

SELECT graph_add_edge(:alice, :bob,   'follows');
SELECT graph_add_edge(:bob,   :carol, 'follows');
SELECT graph_add_edge(:carol, :dave,  'follows');
SELECT graph_add_edge(:alice, :eve,   'follows');

SELECT graph_set_property(:alice, 'name', 2::smallint, str_to_bytea('Alice'));
SELECT graph_set_property(:bob,   'name', 2::smallint, str_to_bytea('Bob'));
SELECT graph_set_property(:carol, 'name', 2::smallint, str_to_bytea('Carol'));
SELECT graph_set_property(:dave,  'name', 2::smallint, str_to_bytea('Dave'));
SELECT graph_set_property(:eve,   'name', 2::smallint, str_to_bytea('Eve'));

-- ============================================================
-- Default result shapes (no RETURN clause)
-- ============================================================
-- multiple matches: bare array of {"id","label"}
SELECT igraph_query(format('MATCH (n:Person)-[:follows]->(m:Person) WHERE n.id = %s', :alice));
-- no matches: bare {} object, *not* []
SELECT igraph_query(format('MATCH (n:Person)-[:follows]->(m:Person) WHERE n.id = %s', :dave));

-- ============================================================
-- WHERE: all 6 comparison operators, id-based (special-cased in
-- get_node_property_value -- "id" always returns the real node id,
-- bypassing property decode entirely) and TEXT-property based (decode
-- path correctly yields a string, per 01_node_edge_crud.sql)
-- ============================================================
SELECT igraph_query(format('MATCH (n:Person)-[:follows]->(m:Person) WHERE n.id = %s AND m.id > %s RETURN m.name', :alice, :bob));
SELECT igraph_query(format('MATCH (n:Person)-[:follows]->(m:Person) WHERE n.id = %s AND m.id >= %s RETURN m.name', :alice, :bob));
SELECT igraph_query(format('MATCH (n:Person)-[:follows]->(m:Person) WHERE n.id = %s AND m.id < %s RETURN m.name', :alice, :eve));
SELECT igraph_query(format('MATCH (n:Person)-[:follows]->(m:Person) WHERE n.id = %s AND m.id <= %s RETURN m.name', :alice, :bob));
SELECT igraph_query(format('MATCH (n:Person)-[:follows]->(m:Person) WHERE n.id = %s AND m.id != %s RETURN m.name', :alice, :bob));

SELECT igraph_query(format('MATCH (n:Person)-[:follows]->(m:Person) WHERE n.id = %s AND m.name = ''Bob'' RETURN m.name', :alice));
SELECT igraph_query(format('MATCH (n:Person)-[:follows]->(m:Person) WHERE n.id = %s AND m.name != ''Bob'' RETURN m.name', :alice));
SELECT igraph_query(format('MATCH (n:Person)-[:follows]->(m:Person) WHERE n.id = %s AND m.name > ''Bob'' RETURN m.name', :alice));
SELECT igraph_query(format('MATCH (n:Person)-[:follows]->(m:Person) WHERE n.id = %s AND m.name < ''Bob'' RETURN m.name', :alice));

-- AND-chaining three conditions
SELECT igraph_query(format(
  'MATCH (n:Person)-[:follows]->(m:Person) WHERE n.id = %s AND m.id != %s AND m.name != ''Zed'' RETURN m.name',
  :alice, :bob));

-- ============================================================
-- RETURN projection forms
-- ============================================================
SELECT igraph_query(format('MATCH (n:Person)-[:follows]->(m:Person) WHERE n.id = %s RETURN m', :alice));       -- bare alias -> just the id
SELECT igraph_query(format('MATCH (n:Person)-[:follows]->(m:Person) WHERE n.id = %s RETURN n', :alice));       -- bare anchor alias -> its own id
SELECT igraph_query(format('MATCH (n:Person)-[:follows]->(m:Person) WHERE n.id = %s RETURN m.id', :alice));
SELECT igraph_query(format('MATCH (n:Person)-[:follows]->(m:Person) WHERE n.id = %s RETURN m.name', :alice));
SELECT igraph_query(format('MATCH (n:Person)-[:follows]->(m:Person) WHERE n.id = %s RETURN n.name, m.name', :alice));

-- ============================================================
-- Rel-depth ranges -- only min=1 forms are exercised here (omitted,
-- *1..3, unbounded *): the executor only ever reads rel->max_depth
-- (igraph_exec.c:843) and never rel->min_depth, so any form requesting
-- min>1 (e.g. *2 meaning "exactly 2 hops") actually behaves like
-- *1..<max> -- pinned as a known bug in 07_known_bugs_and_regressions.sql.
-- ============================================================
SELECT igraph_query(format('MATCH (n:Person)-[:follows]->(m:Person) WHERE n.id = %s RETURN m.name', :alice));         -- omitted depth = *1..1
SELECT igraph_query(format('MATCH (n:Person)-[:follows*1..3]->(m:Person) WHERE n.id = %s RETURN m.name', :alice));
SELECT igraph_query(format('MATCH (n:Person)-[:follows*]->(m:Person) WHERE n.id = %s RETURN m.name', :alice));

-- ============================================================
-- Direction: <-[:REL]- walks the reverse-lookup rows
-- ============================================================
SELECT igraph_query(format('MATCH (n:Person)<-[:follows]-(m:Person) WHERE n.id = %s RETURN m.name', :bob));
