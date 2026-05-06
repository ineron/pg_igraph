#!/bin/bash
# pg_igraph — performance benchmark
# Generates synthetic graphs and measures traversal speed.
#
# Usage:
#   ./benchmark.sh              — runs all benchmarks with defaults
#   ./benchmark.sh --env .env.prod
#   ./benchmark.sh --scale large  (small/medium/large/huge)

set -e

ENV_FILE=".env"
SCALE="medium"

while [[ $# -gt 0 ]]; do
  case $1 in
    --env)   ENV_FILE="$2"; shift 2 ;;
    --scale) SCALE="$2";    shift 2 ;;
    *) echo "Unknown: $1"; exit 1 ;;
  esac
done

# Load env
while IFS='=' read -r key value; do
  [[ -z "$key" || "$key" =~ ^[[:space:]]*# ]] && continue
  value="${value%"${value##*[![:space:]]}"}"
  export "$key=$value"
done < <(grep -E '^[A-Z_]+=' "$ENV_FILE")

PG_HOST="${PG_HOST:-localhost}"
PG_PORT="${PG_PORT:-5432}"
PG_USER="${PG_USER:-postgres}"
PG_DB="${PG_DB:?PG_DB not set}"
PG_SCHEMA="${PG_SCHEMA:-public}"
[[ -n "$PG_PASSWORD" ]] && export PGPASSWORD="$PG_PASSWORD"

PSQL="psql -h $PG_HOST -p $PG_PORT -U $PG_USER -d $PG_DB -v ON_ERROR_STOP=1"

# Scale parameters
case $SCALE in
  small)
    CHAIN_LEN=1000
    TREE_DEPTH=6; TREE_BRANCH=5    # 5^1+5^2+...+5^6 = ~20k nodes
    RANDOM_NODES=5000; RANDOM_EDGES=20000
    ;;
  medium)
    CHAIN_LEN=10000
    TREE_DEPTH=7; TREE_BRANCH=6    # ~55k nodes
    RANDOM_NODES=50000; RANDOM_EDGES=200000
    ;;
  large)
    CHAIN_LEN=100000
    TREE_DEPTH=8; TREE_BRANCH=7    # ~600k nodes
    RANDOM_NODES=500000; RANDOM_EDGES=2000000
    ;;
  huge)
    CHAIN_LEN=500000
    TREE_DEPTH=9; TREE_BRANCH=8
    RANDOM_NODES=2000000; RANDOM_EDGES=10000000
    ;;
  *) echo "Unknown scale: $SCALE (small/medium/large/huge)"; exit 1 ;;
esac

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  pg_igraph benchmark — scale: $SCALE"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  Chain:  $CHAIN_LEN nodes"
echo "  Tree:   depth=$TREE_DEPTH branches=$TREE_BRANCH"
echo "  Random: ${RANDOM_NODES} nodes, ${RANDOM_EDGES} edges"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# ── Benchmark helper ──────────────────────────────────────────────
run_benchmark() {
  local label="$1"
  local sql="$2"
  echo -n "  $label ... "
  local result
  result=$($PSQL -t -c "
    SET search_path = $PG_SCHEMA;
    \\timing off
    SELECT
      extract(milliseconds from (clock_timestamp() - clock_timestamp()))::text
    ;
    " 2>/dev/null)

  # Use \timing via psql
  local timing
  timing=$($PSQL -c "
    SET search_path = $PG_SCHEMA;
    \\timing on
    $sql
  " 2>&1 | grep "Time:" | tail -1)
  echo "$timing"
}

# ── Phase 1: Generate chain graph ────────────────────────────────
echo "▶ Phase 1: Chain graph ($CHAIN_LEN nodes)"
echo "  Generating..."

$PSQL -q << EOF
SET search_path = $PG_SCHEMA;

-- Clean up previous benchmark data
DELETE FROM edges WHERE rel_type IN (
  SELECT id FROM rel_types WHERE name IN ('CHAIN','TREE_EDGE','RANDOM')
);
DELETE FROM nodes WHERE label IN (
  SELECT id FROM node_labels WHERE name IN ('ChainNode','TreeNode','RandomNode')
);
DELETE FROM rel_types   WHERE name IN ('CHAIN','TREE_EDGE','RANDOM');
DELETE FROM node_labels WHERE name IN ('ChainNode','TreeNode','RandomNode');

-- Generate chain: node1→node2→...→nodeN
DO \$\$
DECLARE
  i       INT;
  prev_id BIGINT;
  cur_id  BIGINT;
  t0      TIMESTAMPTZ := clock_timestamp();
BEGIN
  prev_id := graph_add_node('ChainNode');

  FOR i IN 2..$CHAIN_LEN LOOP
    cur_id  := graph_add_node('ChainNode');
    PERFORM graph_add_edge(prev_id, cur_id, 'CHAIN');
    prev_id := cur_id;

    IF i % 10000 = 0 THEN
      RAISE NOTICE 'Chain: % of % nodes (% sec)',
        i, $CHAIN_LEN,
        round(extract(epoch from clock_timestamp() - t0)::numeric, 2);
    END IF;
  END LOOP;

  RAISE NOTICE 'Chain done: % nodes in % sec',
    $CHAIN_LEN,
    round(extract(epoch from clock_timestamp() - t0)::numeric, 2);
END;
\$\$;
EOF

echo "  Done."
echo ""

# ── Phase 2: Generate tree graph ─────────────────────────────────
echo "▶ Phase 2: Tree graph (depth=$TREE_DEPTH, branch=$TREE_BRANCH)"
echo "  Generating..."

$PSQL -q << EOF
SET search_path = $PG_SCHEMA;

DO \$\$
DECLARE
  depth     INT := $TREE_DEPTH;
  branching INT := $TREE_BRANCH;
  root_id   BIGINT;
  t0        TIMESTAMPTZ := clock_timestamp();
  total     INT := 0;
  cur_level BIGINT[];
  nxt_level BIGINT[];
  parent_id BIGINT;
  child_id  BIGINT;
  cur_depth INT;
  i         INT;
BEGIN
  -- Create root
  root_id := graph_add_node('TreeNode');
  cur_level := ARRAY[root_id];
  cur_depth := 0;

  -- Iterative BFS level expansion
  WHILE cur_depth < depth AND array_length(cur_level, 1) > 0 LOOP
    nxt_level := ARRAY[]::BIGINT[];

    FOREACH parent_id IN ARRAY cur_level LOOP
      FOR i IN 1..branching LOOP
        child_id := graph_add_node('TreeNode');
        PERFORM graph_add_edge(parent_id, child_id, 'TREE_EDGE');
        nxt_level := nxt_level || child_id;
      END LOOP;
    END LOOP;

    cur_level := nxt_level;
    cur_depth := cur_depth + 1;

    RAISE NOTICE 'Tree level %: % nodes so far (% sec)',
      cur_depth,
      (SELECT count(*) FROM nodes
       WHERE label=(SELECT id FROM node_labels WHERE name='TreeNode')),
      round(extract(epoch from clock_timestamp() - t0)::numeric, 2);
  END LOOP;

  SELECT count(*) INTO total
  FROM nodes WHERE label=(SELECT id FROM node_labels WHERE name='TreeNode');

  RAISE NOTICE 'Tree done: % nodes in % sec',
    total, round(extract(epoch from clock_timestamp() - t0)::numeric, 2);
END;
\$\$;
EOF

echo "  Done."
echo ""

# ── Phase 3: Generate random graph ───────────────────────────────
echo "▶ Phase 3: Random graph (${RANDOM_NODES} nodes, ${RANDOM_EDGES} edges)"
echo "  Generating nodes..."

$PSQL -q << EOF
SET search_path = $PG_SCHEMA;

-- Bulk insert nodes via generate_series (much faster than row-by-row)
DO \$\$
DECLARE
  label_id SMALLINT;
  t0       TIMESTAMPTZ := clock_timestamp();
BEGIN
  -- Ensure label exists
  INSERT INTO node_labels(name) VALUES('RandomNode')
    ON CONFLICT (name) DO NOTHING;
  SELECT id INTO label_id FROM node_labels WHERE name='RandomNode';

  -- Bulk insert nodes
  INSERT INTO nodes(label)
  SELECT label_id FROM generate_series(1, $RANDOM_NODES);

  RAISE NOTICE 'Random nodes: % inserted in % sec',
    $RANDOM_NODES, round(extract(epoch from clock_timestamp() - t0)::numeric, 2);
END;
\$\$;
EOF

echo "  Generating edges..."

$PSQL -q << EOF
SET search_path = $PG_SCHEMA;

DO \$\$
DECLARE
  rel_id   SMALLINT;
  label_id SMALLINT;
  min_id   BIGINT;
  max_id   BIGINT;
  t0       TIMESTAMPTZ := clock_timestamp();
BEGIN
  INSERT INTO rel_types(name) VALUES('RANDOM')
    ON CONFLICT (name) DO NOTHING;
  SELECT id INTO rel_id   FROM rel_types   WHERE name = 'RANDOM';
  SELECT id INTO label_id FROM node_labels WHERE name = 'RandomNode';
  SELECT min(id), max(id) INTO min_id, max_id
    FROM nodes WHERE label = label_id;

  -- Bulk insert forward edges
  INSERT INTO edges(from_id, to_id, rel_type, direction)
  SELECT
    min_id + (random() * (max_id - min_id))::bigint,
    min_id + (random() * (max_id - min_id))::bigint,
    rel_id,
    TRUE
  FROM generate_series(1, $RANDOM_EDGES)
  ON CONFLICT DO NOTHING;

  -- Bulk insert reverse edges
  INSERT INTO edges(from_id, to_id, rel_type, direction)
  SELECT to_id, from_id, rel_type, FALSE
  FROM edges
  WHERE rel_type = rel_id AND direction = TRUE
  ON CONFLICT DO NOTHING;

  RAISE NOTICE 'Random edges done in % sec',
    round(extract(epoch from clock_timestamp() - t0)::numeric, 2);
END;
\$\$;
EOF

echo "  Done."
echo ""

# ── Phase 4: Run benchmarks ───────────────────────────────────────
echo "▶ Phase 4: Benchmarks"
echo ""

$PSQL << EOF
SET search_path = $PG_SCHEMA;
\timing on

-- ── Chain traversals ─────────────────────────────────────────────
\echo '--- BFS chain: depth 100 ---'
SELECT count(*) FROM graph_traverse(
  (SELECT min(id) FROM nodes
   WHERE label = (SELECT id FROM node_labels WHERE name='ChainNode')),
  'CHAIN', TRUE, 100
);

\echo '--- BFS chain: full traversal ---'
SELECT count(*) FROM graph_traverse(
  (SELECT min(id) FROM nodes
   WHERE label = (SELECT id FROM node_labels WHERE name='ChainNode')),
  'CHAIN', TRUE, 9999999
);

\echo '--- Shortest path: chain start→end ---'
SELECT array_length(
  graph_shortest_path(
    (SELECT min(id) FROM nodes
     WHERE label=(SELECT id FROM node_labels WHERE name='ChainNode')),
    (SELECT max(id) FROM nodes
     WHERE label=(SELECT id FROM node_labels WHERE name='ChainNode')),
    'CHAIN'
  ), 1
) AS path_len;

-- ── Tree traversals ───────────────────────────────────────────────
\echo '--- BFS tree: full traversal from root ---'
SELECT count(*) FROM graph_traverse(
  (SELECT min(id) FROM nodes
   WHERE label=(SELECT id FROM node_labels WHERE name='TreeNode')),
  'TREE_EDGE', TRUE, 99
);

\echo '--- BFS tree: depth 3 only ---'
SELECT count(*) FROM graph_traverse(
  (SELECT min(id) FROM nodes
   WHERE label=(SELECT id FROM node_labels WHERE name='TreeNode')),
  'TREE_EDGE', TRUE, 3
);

\echo '--- Reverse BFS: find ancestors of a leaf ---'
SELECT count(*) FROM graph_traverse(
  (SELECT max(id) FROM nodes
   WHERE label=(SELECT id FROM node_labels WHERE name='TreeNode')),
  'TREE_EDGE', FALSE, 99
);

-- ── Random graph traversals ───────────────────────────────────────
\echo '--- BFS random: depth 3 (from most-connected node) ---'
SELECT count(*) FROM graph_traverse(
  (SELECT e.from_id FROM edges e
   JOIN rel_types r ON r.id = e.rel_type
   WHERE r.name = 'RANDOM' AND e.direction = TRUE
   GROUP BY e.from_id ORDER BY count(*) DESC LIMIT 1),
  'RANDOM', TRUE, 3
);

\echo '--- BFS random: depth 5 (from most-connected node) ---'
SELECT count(*) FROM graph_traverse(
  (SELECT e.from_id FROM edges e
   JOIN rel_types r ON r.id = e.rel_type
   WHERE r.name = 'RANDOM' AND e.direction = TRUE
   GROUP BY e.from_id ORDER BY count(*) DESC LIMIT 1),
  'RANDOM', TRUE, 5
);

-- ── igraph_query language tests ───────────────────────────────────
\echo '--- igraph_query: MATCH chain depth 10 ---'
SELECT igraph_query(
  'MATCH (n:ChainNode)-[:CHAIN*1..10]->(m:ChainNode) WHERE n.id = '
  || (SELECT min(id) FROM nodes WHERE label=(SELECT id FROM node_labels WHERE name='ChainNode'))
  || ' RETURN m.id'
);

\echo '--- igraph_query: PATH ---'
SELECT igraph_query(
  'PATH FROM '
  || (SELECT min(id) FROM nodes
      WHERE label=(SELECT id FROM node_labels WHERE name='ChainNode'))
  || ' TO '
  || (SELECT max(id) FROM nodes
      WHERE label=(SELECT id FROM node_labels WHERE name='ChainNode'))
  || ' VIA CHAIN'
);

\timing off

-- ── Summary stats ─────────────────────────────────────────────────
\echo ''
\echo '--- Graph statistics ---'
SELECT
  nl.name                AS label,
  count(n.id)            AS node_count
FROM nodes n
JOIN node_labels nl ON nl.id = n.label
WHERE nl.name IN ('ChainNode','TreeNode','RandomNode')
GROUP BY nl.name
ORDER BY nl.name;

SELECT
  rt.name                AS rel_type,
  count(*)               AS edge_count
FROM edges e
JOIN rel_types rt ON rt.id = e.rel_type
WHERE rt.name IN ('CHAIN','TREE_EDGE','RANDOM')
  AND e.direction = TRUE
GROUP BY rt.name
ORDER BY rt.name;

\echo ''
\echo '--- Index usage ---'
SELECT
  schemaname,
  relname       AS tablename,
  indexrelname  AS indexname,
  idx_scan,
  idx_tup_read
FROM pg_stat_user_indexes
WHERE relname LIKE 'edges_p%'
ORDER BY idx_scan DESC
LIMIT 10;

EOF

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  Benchmark complete"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

unset PGPASSWORD
