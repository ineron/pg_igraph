-- Migration: add (rel_type, direction) covering index for build_adj_list
-- Run once on existing installations. Safe to run multiple times (IF NOT EXISTS).
--
-- Why: build_adj_list queries edges WHERE rel_type=$1 AND direction=$2.
-- The table is HASH-partitioned on from_id, so without this index every
-- bulk load scans ALL partitions and ALL rel_types, then filters.
-- With this index: index-only scan touches only the requested rel_type rows.
--
-- Expected improvement:
--   CHAIN  (10K edges):  full-table ~150ms → index scan ~5ms
--   RANDOM (200K edges): full-table ~200ms → index scan ~30ms
--   TREE   (335K edges): full-table ~280ms → index scan ~50ms

DO $$
DECLARE
  tbl TEXT;
  idx TEXT;
BEGIN
  FOR tbl IN
    SELECT c.relname FROM pg_class c
    JOIN pg_inherits i ON i.inhrelid = c.oid
    JOIN pg_class p ON p.oid = i.inhparent
    WHERE p.relname = 'edges'
    ORDER BY c.relname
  LOOP
    idx := format('%s_rel_type_direction_idx', tbl);
    IF NOT EXISTS (
      SELECT 1 FROM pg_class WHERE relname = idx
    ) THEN
      EXECUTE format(
        'CREATE INDEX %I ON %I (rel_type, direction) INCLUDE (from_id, to_id)',
        idx, tbl
      );
      RAISE NOTICE 'Created index % on %', idx, tbl;
    ELSE
      RAISE NOTICE 'Index % already exists, skipping', idx;
    END IF;
  END LOOP;
END;
$$;
