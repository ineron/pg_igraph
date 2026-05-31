-- pg_igraph--1.0--1.1.sql
-- Migration script from version 1.0 to 1.1
-- Adds new functionality while preserving all existing features

-- ────────────────────────────────────────────────
-- NEW in v1.1: Extended query language with table prefixes and JSON parameters
-- ────────────────────────────────────────────────

-- Execute an igraph query string with table prefix and JSON parameters support.
-- This allows working with multiple graph datasets in the same database and
-- passing dynamic parameters to queries.
--
-- Parameters:
--   table_prefix - Prefix for table names (e.g., 'users' creates 'users_nodes', 'users_edges', etc.)
--   query        - Query string with optional &data.param syntax for JSON parameter references
--   json_params  - JSON object with parameters (e.g., '{"data":{"threshold":123}}')
--
-- Examples:
--   SELECT igraph_query('nodes', 'MATCH (n:User)-[:follows]->(m:User) WHERE m.influence > &data.threshold RETURN n.name', '{"data":{"threshold":100}}');
--   SELECT igraph_query('products', 'PATH FROM &data.start TO &data.end VIA RELATED', '{"data":{"start":1,"end":99}}');
--   SELECT igraph_query('social', 'CREATE (n:User)', NULL);
--   SELECT igraph_query('social', 'SET NODE &data.id name = &data.name', '{"data":{"id":42,"name":"Alice"}}');
CREATE FUNCTION igraph_query(table_prefix TEXT, query TEXT, json_params TEXT DEFAULT NULL)
  RETURNS JSONB
  AS 'pg_igraph', 'igraph_query_extended'
  LANGUAGE C;