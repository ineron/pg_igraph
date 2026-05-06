# pg_igraph

A native graph traversal engine for PostgreSQL, implemented as a C extension.

BFS traversal and bidirectional shortest path over partitioned edge tables — no recursive CTEs, no external graph database, no separate infrastructure.

```sql
-- Traverse all nodes reachable from node 42 via FOLLOWS edges, depth 5
SELECT * FROM graph_traverse(42, 'FOLLOWS', true, 5);

-- Shortest path between two nodes
SELECT graph_shortest_path(42, 999, 'FOLLOWS');

-- Query language
SELECT igraph_query('MATCH (n:User)-[:FOLLOWS*1..3]->(m) RETURN m');
```

## Benchmark (medium scale, HDD server)

335K-node tree, 200K-edge random graph, 10K-node chain — all loaded simultaneously.

| Query | Time |
|-------|------|
| BFS full traversal, 335K-node tree | **227 ms** |
| Shortest path, 10K-node chain | **49 ms** |
| BFS depth=3, 335K-node tree | **3.6 ms** |
| BFS depth=100, chain | **16 ms** |
| Reverse BFS (find ancestors) | **6 ms** |
| Query language MATCH depth=10 | **3 ms** |

Hardware: 48-CPU server, 128GB RAM, HDD storage.

---

## How It Works

### Storage

Two partitioned tables:

```sql
nodes (id BIGSERIAL, label SMALLINT)

edges (from_id BIGINT, to_id BIGINT, rel_type SMALLINT, direction BOOL)
      PARTITION BY HASH(from_id)   -- 16 partitions by default
```

Both forward and reverse edges are stored explicitly. `direction = true` for outgoing, `direction = false` for incoming. This doubles write cost but makes both BFS directions and bidirectional shortest path use identical access patterns.

Three covering indexes per partition:

```sql
(from_id, rel_type, to_id) WHERE direction = TRUE   -- forward traversal
(from_id, rel_type, to_id) WHERE direction = FALSE  -- reverse traversal
(rel_type, direction) INCLUDE (from_id, to_id)      -- bulk load for adj list
```

### Traversal Strategy

pg_igraph uses an adaptive approach based on **frontier size**, not query depth:

**Phase 1 — per-level SPI** (starts here for every query):
- `frontier == 1` → single prepared statement, one index seek
- `frontier >= 2` → `LATERAL unnest()` batch, one round-trip for the whole level

**Switch trigger** — when `frontier_size > 200`:
- Load all edges for the rel_type into a C-level adjacency hash map (one sequential scan)
- Close SPI connection
- Continue BFS entirely in C — zero SQL calls during traversal

This means:
- Chains and ancestor lookups (frontier stays 1) → per-level, no preload cost
- Trees and dense graphs (frontier explodes) → load-all once, then pointer arithmetic

### Why Not Recursive CTE?

The obvious first approach is a recursive CTE:

```sql
WITH RECURSIVE bfs(node_id, d) AS (
  SELECT $1, 0
  UNION ALL
  SELECT e.to_id, b.d + 1 FROM bfs b
  JOIN edges e ON e.from_id = b.node_id WHERE b.d < $2
)
SELECT DISTINCT node_id FROM bfs;
```

PostgreSQL's recursive executor materializes intermediate results at each level, re-scans them, and repeats. It cannot maintain a visited set across iterations — `UNION ALL` produces duplicates that must be collapsed with `DISTINCT` after the fact. On a 335K-node tree this takes **47 seconds**. pg_igraph does it in **227ms**.

The C traversal framework maintains the visited HTAB across levels, never revisits a node, and during Phase 2 makes zero SQL calls. The recursive CTE approach has no equivalent optimization path.

---

## Installation

**Dependencies:**
- PostgreSQL 14+ (with development headers)
- `pg_ilib` extension (binary property serialization — install first)
- `flex` and `bison` (for the query language parser)

```bash
git clone https://github.com/ineron/pg_igraph.git
cd pg_igraph
make
sudo make install
```

Initialize the schema (reads config from `.env`):

```bash
cp .env.example .env
# edit: PG_HOST, PG_PORT, PG_DB, PG_USER, PG_SCHEMA, GRAPH_PARTITIONS
./init_graph.sh
```

In psql:

```sql
CREATE EXTENSION pg_ilib;
CREATE EXTENSION pg_igraph;
```

---

## API Reference

### Nodes and Edges

```sql
-- Add a node with a label
SELECT graph_add_node('User');           -- returns BIGINT node id

-- Add a directed edge
SELECT graph_add_edge(42, 99, 'FOLLOWS');

-- Delete a node (and all its edges)
SELECT graph_delete_node(42);
```

### Traversal

```sql
-- BFS: returns SETOF BIGINT
-- graph_traverse(start_id, rel_type, forward, max_depth)
SELECT * FROM graph_traverse(42, 'FOLLOWS', true, 5);
SELECT * FROM graph_traverse(42, 'FOLLOWS', false, 3);  -- reverse

-- Shortest path: returns BIGINT[] (node id sequence)
SELECT graph_shortest_path(42, 999, 'FOLLOWS');
```

### Properties

```sql
-- Set a property on a node (typed binary storage via pg_ilib)
SELECT graph_set_property(42, 'name', 'text', 'Alice');
SELECT graph_set_property(42, 'score', 'float8', '3.14');

-- Get a single property
SELECT graph_get_property(42, 'name');

-- Get all properties as JSONB
SELECT graph_get_node_properties(42);
```

### Query Language

```sql
SELECT igraph_query('MATCH (n:User)-[:FOLLOWS*1..5]->(m) WHERE n.id = 42 RETURN m');
SELECT igraph_query('PATH FROM 42 TO 999 VIA FOLLOWS');
```

Returns JSONB.

---

## Configuration

`init_graph.sh` reads from `.env`:

```bash
PG_HOST=localhost
PG_PORT=5432
PG_DB=mydb
PG_USER=postgres
PG_SCHEMA=public
GRAPH_PARTITIONS=16   # must be 8, 16, 32, or 64
```

More partitions = better parallelism for bulk loads, more index overhead per write. 16 is a reasonable default for most workloads.

---

## Running Benchmarks

```bash
./benchmark.sh --scale small    # 1K chain, 19K tree, 5K random
./benchmark.sh --scale medium   # 10K chain, 335K tree, 50K random
```

---

## Project Structure

```
pg_igraph.c          core: adjacency list, BFS, shortest path, CRUD
igraph_lexer.l       Flex lexer for query language
igraph_parser.y      Bison grammar, AST construction
igraph_exec.c        executor: AST → C calls → JSONB
igraph_query_func.c  igraph_query(TEXT) → JSONB entry point
igraph_query.h       shared AST type definitions
pg_igraph--1.0.sql   SQL function registration
init_graph.sh        schema initialization
benchmark.sh         test graph generation and timing
```

---

## Companion: pg_ilib

Node and edge properties are stored as typed binary in BYTEA columns via [`pg_ilib`](https://github.com/ineron/pg_ilib.git) — a separate extension providing a compact binary serialization format with typed get/set operations.

Install `pg_ilib` before `pg_igraph`.

---

## License

Apache 2.0
