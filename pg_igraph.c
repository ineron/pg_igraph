/*
 * pg_igraph.c
 * Native graph traversal engine for PostgreSQL
 *
 * Tables expected (created by init_graph.sh):
 *   node_labels    — registry of node types
 *   rel_types      — registry of relationship types
 *   property_types — registry of property types with primitive codes
 *   nodes          — graph nodes
 *   node_properties— node property values (bytea)
 *   edges          — graph edges, hash-partitioned
 */

#include "postgres.h"
#include "fmgr.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/array.h"
#include "utils/jsonb.h"
#include "catalog/pg_type.h"

#include "igraph_query.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

/* ================================================================
 * Primitive type codes (mirrors property_types.primitive)
 * ================================================================ */
#define PRIM_BIGINT     1
#define PRIM_TEXT       2
#define PRIM_UUID       3
#define PRIM_TIMESTAMP  4
#define PRIM_BOOL       5
#define PRIM_NUMERIC    6
#define PRIM_JSONB      7

/* ================================================================
 * Cached prepared plans
 * SPI_keepplan — survive SPI_finish within session
 * ================================================================ */
static SPIPlanPtr plan_get_rel_id        = NULL;
static SPIPlanPtr plan_get_label_id      = NULL;
static SPIPlanPtr plan_insert_label      = NULL;
static SPIPlanPtr plan_insert_rel        = NULL;
static SPIPlanPtr plan_insert_node       = NULL;
static SPIPlanPtr plan_insert_edge       = NULL;
static SPIPlanPtr plan_get_neighbors       = NULL;
static SPIPlanPtr plan_get_neighbors_batch = NULL; /* batch: from_id ANY($1) */
static SPIPlanPtr plan_load_all_edges      = NULL; /* bulk load: all edges for rel_type+dir */
static SPIPlanPtr plan_get_prop_type_id  = NULL;
static SPIPlanPtr plan_insert_prop_type  = NULL;
static SPIPlanPtr plan_upsert_prop       = NULL;
static SPIPlanPtr plan_get_prop          = NULL;
static SPIPlanPtr plan_delete_prop       = NULL;
static SPIPlanPtr plan_get_all_props     = NULL;
static SPIPlanPtr plan_delete_node_edges = NULL;
static SPIPlanPtr plan_delete_node_rev   = NULL;
static SPIPlanPtr plan_delete_node_props = NULL;
static SPIPlanPtr plan_delete_node_row   = NULL;

static void
prepare_plans(void)
{
    if (plan_get_neighbors != NULL && plan_get_neighbors_batch != NULL &&
        plan_load_all_edges != NULL)
        return;

    /* SELECT single neighbour — kept for compatibility / fallback */
    {
        Oid t[] = { INT8OID, INT2OID, BOOLOID };
        plan_get_neighbors = SPI_prepare(
            "SELECT to_id FROM edges"
            " WHERE from_id = $1 AND rel_type = $2 AND direction = $3",
            3, t);
        if (!plan_get_neighbors)
            elog(ERROR, "pg_igraph: failed to prepare plan_get_neighbors");
        SPI_keepplan(plan_get_neighbors);
    }

    /*
     * Batch neighbours — fetches ALL (from_id, to_id) pairs for an entire
     * BFS frontier level in a single round-trip.
     *
     * $1 = bigint[]  (array of frontier node IDs)
     * $2 = smallint  (rel_type id)
     * $3 = bool      (direction)
     * Returns (from_id bigint, to_id bigint) so callers can track parents.
     *
     * WHY LATERAL instead of ANY($1):
     *   ANY($1) on a HASH-partitioned table prevents runtime partition pruning
     *   — PostgreSQL cannot prune partitions for an array parameter at
     *   planning time, so it scans all 16 partitions per batch query.
     *   At frontier depth 6 (15 625 nodes) this is far slower than 15 625
     *   targeted index lookups.
     *
     *   LATERAL with unnest() forces a Nested Loop: for each element of the
     *   frontier array the planner generates one index seek into edges with
     *   full partition pruning, exactly matching the old per-node behaviour
     *   but with zero SPI call overhead between nodes.
     *
     *   Net effect: O(D) SPI calls (one per BFS depth level) with O(N)
     *   individual index lookups inside a single query — best of both worlds.
     */
    {
        Oid t[] = { INT8ARRAYOID, INT2OID, BOOLOID };
        plan_get_neighbors_batch = SPI_prepare(
            "SELECT f.id, e.to_id"
            " FROM unnest($1::bigint[]) AS f(id)"
            " JOIN LATERAL ("
            "   SELECT to_id FROM edges"
            "   WHERE from_id = f.id AND rel_type = $2 AND direction = $3"
            " ) AS e ON true",
            3, t);
        if (!plan_get_neighbors_batch)
            elog(ERROR, "pg_igraph: failed to prepare plan_get_neighbors_batch");
        SPI_keepplan(plan_get_neighbors_batch);
    }

    /*
     * Bulk edge loader — loads ALL edges for a given (rel_type, direction) pair.
     * Used by graph_traverse and igraph_shortest_path_internal to build an
     * in-memory adjacency list, enabling pure-C BFS with ZERO per-level SPI calls.
     *
     * One sequential scan of all partitions — cache-friendly, fast even on HDD.
     * $1 = smallint rel_type id,  $2 = bool direction
     */
    if (plan_load_all_edges == NULL)
    {
        Oid t[] = { INT2OID, BOOLOID };
        plan_load_all_edges = SPI_prepare(
            "SELECT from_id, to_id FROM edges WHERE rel_type = $1 AND direction = $2",
            2, t);
        if (!plan_load_all_edges)
            elog(ERROR, "pg_igraph: failed to prepare plan_load_all_edges");
        SPI_keepplan(plan_load_all_edges);
    }


    /* SELECT rel_type id by name */
    {
        Oid t[] = { TEXTOID };
        plan_get_rel_id = SPI_prepare(
            "SELECT id FROM rel_types WHERE name = $1", 1, t);
        if (!plan_get_rel_id)
            elog(ERROR, "pg_igraph: failed to prepare plan_get_rel_id");
        SPI_keepplan(plan_get_rel_id);
    }

    /* SELECT node_label id by name */
    {
        Oid t[] = { TEXTOID };
        plan_get_label_id = SPI_prepare(
            "SELECT id FROM node_labels WHERE name = $1", 1, t);
        if (!plan_get_label_id)
            elog(ERROR, "pg_igraph: failed to prepare plan_get_label_id");
        SPI_keepplan(plan_get_label_id);
    }

    /* UPSERT node_labels */
    {
        Oid t[] = { TEXTOID };
        plan_insert_label = SPI_prepare(
            "INSERT INTO node_labels(name) VALUES($1)"
            " ON CONFLICT (name) DO UPDATE SET name = EXCLUDED.name"
            " RETURNING id",
            1, t);
        if (!plan_insert_label)
            elog(ERROR, "pg_igraph: failed to prepare plan_insert_label");
        SPI_keepplan(plan_insert_label);
    }

    /* UPSERT rel_types */
    {
        Oid t[] = { TEXTOID };
        plan_insert_rel = SPI_prepare(
            "INSERT INTO rel_types(name) VALUES($1)"
            " ON CONFLICT (name) DO UPDATE SET name = EXCLUDED.name"
            " RETURNING id",
            1, t);
        if (!plan_insert_rel)
            elog(ERROR, "pg_igraph: failed to prepare plan_insert_rel");
        SPI_keepplan(plan_insert_rel);
    }

    /* INSERT node */
    {
        Oid t[] = { INT2OID };
        plan_insert_node = SPI_prepare(
            "INSERT INTO nodes(label) VALUES($1) RETURNING id", 1, t);
        if (!plan_insert_node)
            elog(ERROR, "pg_igraph: failed to prepare plan_insert_node");
        SPI_keepplan(plan_insert_node);
    }

    /* INSERT edge (used for both forward and reverse) */
    {
        Oid t[] = { INT8OID, INT8OID, INT2OID, BOOLOID };
        plan_insert_edge = SPI_prepare(
            "INSERT INTO edges(from_id, to_id, rel_type, direction)"
            " VALUES($1, $2, $3, $4) ON CONFLICT DO NOTHING",
            4, t);
        if (!plan_insert_edge)
            elog(ERROR, "pg_igraph: failed to prepare plan_insert_edge");
        SPI_keepplan(plan_insert_edge);
    }

    /* SELECT property_type id by name */
    {
        Oid t[] = { TEXTOID };
        plan_get_prop_type_id = SPI_prepare(
            "SELECT id FROM property_types WHERE name = $1", 1, t);
        if (!plan_get_prop_type_id)
            elog(ERROR, "pg_igraph: failed to prepare plan_get_prop_type_id");
        SPI_keepplan(plan_get_prop_type_id);
    }

    /* INSERT property_type — $1=name $2=primitive $3=ref_label (nullable) */
    {
        Oid t[] = { TEXTOID, INT2OID, INT2OID };
        plan_insert_prop_type = SPI_prepare(
            "INSERT INTO property_types(name, primitive, ref_label)"
            " VALUES($1, $2, NULLIF($3, 0))"
            " ON CONFLICT (name) DO UPDATE"
            "   SET primitive = EXCLUDED.primitive,"
            "       ref_label = EXCLUDED.ref_label"
            " RETURNING id",
            3, t);
        if (!plan_insert_prop_type)
            elog(ERROR, "pg_igraph: failed to prepare plan_insert_prop_type");
        SPI_keepplan(plan_insert_prop_type);
    }

    /* UPSERT node property value */
    {
        Oid t[] = { INT8OID, INT2OID, BYTEAOID };
        plan_upsert_prop = SPI_prepare(
            "INSERT INTO node_properties(node_id, prop_id, value)"
            " VALUES($1, $2, $3)"
            " ON CONFLICT (node_id, prop_id)"
            " DO UPDATE SET value = EXCLUDED.value",
            3, t);
        if (!plan_upsert_prop)
            elog(ERROR, "pg_igraph: failed to prepare plan_upsert_prop");
        SPI_keepplan(plan_upsert_prop);
    }

    /* SELECT property value */
    {
        Oid t[] = { INT8OID, TEXTOID };
        plan_get_prop = SPI_prepare(
            "SELECT np.value"
            " FROM node_properties np"
            " JOIN property_types pt ON pt.id = np.prop_id"
            " WHERE np.node_id = $1 AND pt.name = $2",
            2, t);
        if (!plan_get_prop)
            elog(ERROR, "pg_igraph: failed to prepare plan_get_prop");
        SPI_keepplan(plan_get_prop);
    }

    /* DELETE property */
    {
        Oid t[] = { INT8OID, TEXTOID };
        plan_delete_prop = SPI_prepare(
            "DELETE FROM node_properties np"
            " USING property_types pt"
            " WHERE np.prop_id = pt.id"
            "   AND np.node_id = $1 AND pt.name = $2",
            2, t);
        if (!plan_delete_prop)
            elog(ERROR, "pg_igraph: failed to prepare plan_delete_prop");
        SPI_keepplan(plan_delete_prop);
    }

    /* SELECT all properties of a node as (name, primitive, value) */
    {
        Oid t[] = { INT8OID };
        plan_get_all_props = SPI_prepare(
            "SELECT pt.name, pt.primitive, np.value"
            " FROM node_properties np"
            " JOIN property_types pt ON pt.id = np.prop_id"
            " WHERE np.node_id = $1"
            " ORDER BY pt.name",
            1, t);
        if (!plan_get_all_props)
            elog(ERROR, "pg_igraph: failed to prepare plan_get_all_props");
        SPI_keepplan(plan_get_all_props);
    }

    /* ── graph_delete_node plans ── */
    if (plan_delete_node_edges == NULL)
    {
        /* Delete all forward edges FROM this node */
        Oid t[] = { INT8OID };
        plan_delete_node_edges = SPI_prepare(
            "DELETE FROM edges WHERE from_id = $1", 1, t);
        if (!plan_delete_node_edges)
            elog(ERROR, "pg_igraph: failed to prepare plan_delete_node_edges");
        SPI_keepplan(plan_delete_node_edges);
    }

    if (plan_delete_node_rev == NULL)
    {
        /* Delete reverse edges pointing TO this node from neighbours */
        Oid t[] = { INT8OID };
        plan_delete_node_rev = SPI_prepare(
            "DELETE FROM edges WHERE to_id = $1", 1, t);
        if (!plan_delete_node_rev)
            elog(ERROR, "pg_igraph: failed to prepare plan_delete_node_rev");
        SPI_keepplan(plan_delete_node_rev);
    }

    if (plan_delete_node_props == NULL)
    {
        /* Delete all properties of this node */
        Oid t[] = { INT8OID };
        plan_delete_node_props = SPI_prepare(
            "DELETE FROM node_properties WHERE node_id = $1", 1, t);
        if (!plan_delete_node_props)
            elog(ERROR, "pg_igraph: failed to prepare plan_delete_node_props");
        SPI_keepplan(plan_delete_node_props);
    }

    if (plan_delete_node_row == NULL)
    {
        /* Delete the node itself */
        Oid t[] = { INT8OID };
        plan_delete_node_row = SPI_prepare(
            "DELETE FROM nodes WHERE id = $1", 1, t);
        if (!plan_delete_node_row)
            elog(ERROR, "pg_igraph: failed to prepare plan_delete_node_row");
        SPI_keepplan(plan_delete_node_row);
    }
}

/* ================================================================
 * Helper: lookup or create a SMALLINT id by name
 * ================================================================ */
static int16
lookup_or_create_id(SPIPlanPtr plan_lookup,
                    SPIPlanPtr plan_insert,
                    const char *name,
                    const char *entity)
{
    Datum args[1] = { CStringGetTextDatum(name) };
    bool  isnull;
    int   ret;

    ret = SPI_execute_plan(plan_lookup, args, NULL, true, 1);
    if (ret != SPI_OK_SELECT)
        elog(ERROR, "pg_igraph: lookup failed for %s '%s'", entity, name);

    if (SPI_processed > 0)
        return DatumGetInt16(
            SPI_getbinval(SPI_tuptable->vals[0],
                          SPI_tuptable->tupdesc, 1, &isnull));

    ret = SPI_execute_plan(plan_insert, args, NULL, false, 1);
    if (ret != SPI_OK_INSERT_RETURNING || SPI_processed == 0)
        elog(ERROR, "pg_igraph: insert failed for %s '%s'", entity, name);

    return DatumGetInt16(
        SPI_getbinval(SPI_tuptable->vals[0],
                      SPI_tuptable->tupdesc, 1, &isnull));
}

/* ================================================================
 * graph_add_node(label_name TEXT) → BIGINT
 * ================================================================ */
PG_FUNCTION_INFO_V1(graph_add_node);
Datum graph_add_node(PG_FUNCTION_ARGS)
{
    text  *label_name = PG_GETARG_TEXT_PP(0);
    char  *label_str  = text_to_cstring(label_name);
    bool   isnull;
    int64  node_id;

    SPI_connect();
    prepare_plans();

    int16 label_id = lookup_or_create_id(
        plan_get_label_id, plan_insert_label, label_str, "node_label");

    Datum args[] = { Int16GetDatum(label_id) };
    int ret = SPI_execute_plan(plan_insert_node, args, NULL, false, 1);

    if (ret != SPI_OK_INSERT_RETURNING || SPI_processed == 0)
        elog(ERROR, "graph_add_node: failed to insert node");

    node_id = DatumGetInt64(
        SPI_getbinval(SPI_tuptable->vals[0],
                      SPI_tuptable->tupdesc, 1, &isnull));

    SPI_finish();
    PG_RETURN_INT64(node_id);
}

/* ================================================================
 * graph_add_edge(from_id BIGINT, to_id BIGINT, rel_name TEXT) → VOID
 * Inserts forward + reverse edges atomically
 * ================================================================ */
PG_FUNCTION_INFO_V1(graph_add_edge);
Datum graph_add_edge(PG_FUNCTION_ARGS)
{
    int64  from_id  = PG_GETARG_INT64(0);
    int64  to_id    = PG_GETARG_INT64(1);
    text  *rel_name = PG_GETARG_TEXT_PP(2);
    char  *rel_str  = text_to_cstring(rel_name);

    SPI_connect();
    prepare_plans();

    int16 rel_id = lookup_or_create_id(
        plan_get_rel_id, plan_insert_rel, rel_str, "rel_type");

    /* Forward edge */
    Datum fwd[] = {
        Int64GetDatum(from_id), Int64GetDatum(to_id),
        Int16GetDatum(rel_id),  BoolGetDatum(true)
    };
    if (SPI_execute_plan(plan_insert_edge, fwd, NULL, false, 0) != SPI_OK_INSERT)
        elog(ERROR, "graph_add_edge: failed to insert forward edge (SPI ret check)");

    /* Reverse edge */
    Datum rev[] = {
        Int64GetDatum(to_id),   Int64GetDatum(from_id),
        Int16GetDatum(rel_id),  BoolGetDatum(false)
    };
    if (SPI_execute_plan(plan_insert_edge, rev, NULL, false, 0) != SPI_OK_INSERT)
        elog(ERROR, "graph_add_edge: failed to insert reverse edge (SPI ret check)");

    SPI_finish();
    PG_RETURN_VOID();
}

/* ================================================================
 * In-memory adjacency list — the traversal framework core.
 *
 * Architecture:
 *   1. ONE SPI call loads all edges for (rel_type, direction) into a
 *      C-level hash map (HTAB from_id → int64[] neighbors).
 *   2. BFS / shortest-path run entirely in C over this structure.
 *   3. Zero SQL calls during traversal — no per-level overhead.
 *
 * Why this is faster than any SPI-per-level approach:
 *   - Chain 10K:  old=10K calls×0.04ms=400ms, new=1 scan+C BFS≈5ms
 *   - Tree 335K:  old=47s (tuptable overhead), new=1 scan+C BFS≈50ms
 *   - The sequential scan of edges is cache-friendly (HDD: ~50-200ms
 *     for a few MB); random index lookups per BFS level are not.
 * ================================================================ */

/* One entry in the adjacency hash map: from_id → dynamic array of to_ids */
typedef struct {
    int64   from_id;      /* HTAB key — must be first */
    int64  *neighbors;    /* palloc'd in adj_ctx       */
    int     n;            /* used count                */
    int     cap;          /* allocated capacity        */
} AdjNode;

/* Adjacency list — owns all memory in adj_ctx */
typedef struct {
    HTAB         *htab;
    MemoryContext ctx;
    int           n_nodes;  /* total distinct from_ids  */
} AdjList;

/*
 * build_adj_list — load all (from_id, to_id) pairs for (rel_id, direction)
 * from the edges table into an in-memory adjacency hash map.
 *
 * Precondition: SPI is connected, CurrentMemoryContext may be anything.
 * Postcondition: returns AdjList allocated in `ctx`; SPI tuptable still valid.
 * Caller must SPI_finish() after this to free the tuptable.
 */
static AdjList *
build_adj_list(int16 rel_id, bool direction, MemoryContext ctx)
{
    Datum args[] = { Int16GetDatum(rel_id), BoolGetDatum(direction) };
    int   ret    = SPI_execute_plan(plan_load_all_edges, args, NULL, true, 0);

    if (ret != SPI_OK_SELECT)
        elog(ERROR, "pg_igraph: build_adj_list: failed to load edges (ret=%d)", ret);

    uint64  nrows = SPI_processed;

    /*
     * Copy raw (from_id, to_id) pairs out of SPI's tuptable BEFORE
     * switching to ctx, so SPI_getbinval has the correct context.
     */
    int64  *from_ids = NULL;
    int64  *to_ids   = NULL;

    if (nrows > 0)
    {
        from_ids = (int64 *) MemoryContextAlloc(ctx, nrows * sizeof(int64));
        to_ids   = (int64 *) MemoryContextAlloc(ctx, nrows * sizeof(int64));

        for (uint64 i = 0; i < nrows; i++)
        {
            bool isnull;
            from_ids[i] = DatumGetInt64(
                SPI_getbinval(SPI_tuptable->vals[i],
                              SPI_tuptable->tupdesc, 1, &isnull));
            to_ids[i]   = DatumGetInt64(
                SPI_getbinval(SPI_tuptable->vals[i],
                              SPI_tuptable->tupdesc, 2, &isnull));
        }
    }

    /* Build adjacency hash map in ctx */
    MemoryContext saved_ctx = MemoryContextSwitchTo(ctx);

    HASHCTL hctl;
    memset(&hctl, 0, sizeof(hctl));
    hctl.keysize   = sizeof(int64);
    hctl.entrysize = sizeof(AdjNode);
    hctl.hcxt      = ctx;

    HTAB *htab = hash_create("igraph_adj",
                              nrows > 0 ? (int)(nrows / 4 + 16) : 256,
                              &hctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

    for (uint64 i = 0; i < nrows; i++)
    {
        bool     found;
        AdjNode *node = hash_search(htab, &from_ids[i], HASH_ENTER, &found);
        if (!found)
        {
            node->n         = 0;
            node->cap       = 8;
            node->neighbors = (int64 *) palloc(8 * sizeof(int64));
        }
        if (node->n >= node->cap)
        {
            node->cap      *= 2;
            node->neighbors = (int64 *) repalloc(node->neighbors,
                                                  node->cap * sizeof(int64));
        }
        node->neighbors[node->n++] = to_ids[i];
    }

    AdjList *adj   = (AdjList *) palloc(sizeof(AdjList));
    adj->htab      = htab;
    adj->ctx       = ctx;
    adj->n_nodes   = (int) hash_get_num_entries(htab);

    MemoryContextSwitchTo(saved_ctx);
    return adj;
}

/* Get the neighbor array for node_id; returns NULL if node has no outgoing edges */
static inline int64 *
adj_get(AdjList *adj, int64 node_id, int *count)
{
    bool     found;
    AdjNode *node = hash_search(adj->htab, &node_id, HASH_FIND, &found);
    if (!found) { *count = 0; return NULL; }
    *count = node->n;
    return node->neighbors;
}

/* ================================================================
 * graph_traverse — BFS Set Returning Function
 *
 * Adaptive frontier-based strategy:
 *
 *   Phase 1 (per-level, always starts here):
 *     - frontier == 1 → plan_get_neighbors (single index seek, no array)
 *     - frontier >= 2 → plan_get_neighbors_batch (LATERAL unnest)
 *     No fixed preload cost. Optimal for: chains, ancestors, shallow.
 *
 *   Trigger: if frontier_size > ADAPTIVE_FRONTIER_THRESHOLD after a level,
 *     build adj list in-flight (one SPI call), then switch to Phase 2.
 *
 *   Phase 2 (pure-C BFS over in-memory adjacency list):
 *     Zero SPI calls during traversal. Optimal for: trees, random graphs,
 *     any traversal where the frontier eventually "explodes".
 *
 * This correctly handles all cases:
 *   Chain depth=100   → frontier stays 1 → per-level only   → ~15ms
 *   Reverse ancestors → frontier stays 1 → per-level only   → ~5ms
 *   Tree depth=3      → frontier < 200   → per-level only   → ~4ms
 *   Tree full depth=7 → frontier hits 9K → switches level 4 → ~260ms
 *   Random depth=5    → frontier hits 256→ switches level 4 → ~150ms
 * ================================================================ */

#define ADAPTIVE_FRONTIER_THRESHOLD 200

typedef struct {
    int64  *result;
    int     result_size;
    int     result_pos;
} BFSContext;

#define QUEUE_INIT_SIZE 1024

PG_FUNCTION_INFO_V1(graph_traverse);
Datum graph_traverse(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;
    BFSContext      *bfs;
    MemoryContext    oldctx;

    if (SRF_IS_FIRSTCALL())
    {
        int64  start_id  = PG_GETARG_INT64(0);
        text  *rel_name  = PG_GETARG_TEXT_PP(1);
        bool   direction = PG_GETARG_BOOL(2);
        int    max_depth = PG_GETARG_INT32(3);
        char  *rel_str   = text_to_cstring(rel_name);

        funcctx = SRF_FIRSTCALL_INIT();
        oldctx  = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
        bfs     = palloc0(sizeof(BFSContext));

        MemoryContext bfs_work_ctx =
            AllocSetContextCreate(funcctx->multi_call_memory_ctx,
                                  "igraph_bfs_work", ALLOCSET_DEFAULT_SIZES);

        SPI_connect();
        MemoryContextSwitchTo(bfs_work_ctx);
        prepare_plans();

        /* ── resolve rel_type → id ── */
        int16 rel_id;
        {
            bool  isnull;
            Datum rel_args[] = { CStringGetTextDatum(rel_str) };
            int   rret = SPI_execute_plan(plan_get_rel_id, rel_args, NULL, true, 1);
            MemoryContextSwitchTo(bfs_work_ctx);
            if (rret != SPI_OK_SELECT || SPI_processed == 0)
                elog(ERROR, "graph_traverse: unknown rel_type '%s'", rel_str);
            rel_id = DatumGetInt16(
                SPI_getbinval(SPI_tuptable->vals[0],
                              SPI_tuptable->tupdesc, 1, &isnull));
        }

        int    res_cap  = QUEUE_INIT_SIZE;
        int64 *result   = (int64 *) palloc(res_cap * sizeof(int64));
        int    res_size = 0;

        /* Shared visited HTAB used by both phases */
        HASHCTL hctl;
        memset(&hctl, 0, sizeof(hctl));
        hctl.keysize   = sizeof(int64);
        hctl.entrysize = sizeof(int64);
        hctl.hcxt      = bfs_work_ctx;
        HTAB *visited = hash_create("igraph_bfs_visited", QUEUE_INIT_SIZE,
                                    &hctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

        /* Current frontier */
        int    cur_cap   = QUEUE_INIT_SIZE;
        int64 *cur_level = (int64 *) palloc(cur_cap * sizeof(int64));
        int    cur_size  = 1;
        cur_level[0]     = start_id;
        { bool found; hash_search(visited, &start_id, HASH_ENTER, &found); }
        result[res_size++] = start_id;

        bool      switched  = false;  /* have we loaded the adj list? */
        AdjList  *adj       = NULL;
        int       cur_depth = 0;

        /* ── Phase 1: per-level BFS ── */
        while (cur_depth < max_depth && cur_size > 0)
        {
            int next_size = 0;

            if (cur_size == 1)
            {
                /* Single-node fast path: one index seek */
                Datum s_args[] = {
                    Int64GetDatum(cur_level[0]),
                    Int16GetDatum(rel_id),
                    BoolGetDatum(direction)
                };
                int sret = SPI_execute_plan(plan_get_neighbors, s_args, NULL, true, 0);
                MemoryContextSwitchTo(bfs_work_ctx);
                if (sret == SPI_OK_SELECT && SPI_processed > 0)
                {
                    uint64 nrows = SPI_processed;
                    if (cur_cap < (int) nrows)
                    {
                        pfree(cur_level);
                        cur_cap   = (int) nrows * 2;
                        cur_level = (int64 *) palloc(cur_cap * sizeof(int64));
                    }
                    for (uint64 j = 0; j < nrows; j++)
                    {
                        bool  inull;
                        int64 nbr = DatumGetInt64(
                            SPI_getbinval(SPI_tuptable->vals[j],
                                          SPI_tuptable->tupdesc, 1, &inull));
                        bool found;
                        hash_search(visited, &nbr, HASH_ENTER, &found);
                        if (found) continue;
                        cur_level[next_size++] = nbr;
                        if (res_size >= res_cap) { res_cap *= 2; result = (int64*) repalloc(result, res_cap * sizeof(int64)); }
                        result[res_size++] = nbr;
                    }
                }
            }
            else
            {
                /* Multi-node batch: LATERAL unnest — one round-trip */
                Datum *elems = (Datum *) palloc(cur_size * sizeof(Datum));
                for (int i = 0; i < cur_size; i++)
                    elems[i] = Int64GetDatum(cur_level[i]);
                ArrayType *arr = construct_array(elems, cur_size, INT8OID,
                                                 sizeof(int64), true, TYPALIGN_DOUBLE);
                pfree(elems);
                Datum b_args[] = { PointerGetDatum(arr), Int16GetDatum(rel_id), BoolGetDatum(direction) };
                int bret = SPI_execute_plan(plan_get_neighbors_batch, b_args, NULL, true, 0);
                MemoryContextSwitchTo(bfs_work_ctx);
                if (bret == SPI_OK_SELECT && SPI_processed > 0)
                {
                    uint64  nrows   = SPI_processed;
                    int64  *nbr_ids = (int64 *) palloc(nrows * sizeof(int64));
                    for (uint64 j = 0; j < nrows; j++)
                    {
                        bool inull;
                        nbr_ids[j] = DatumGetInt64(
                            SPI_getbinval(SPI_tuptable->vals[j],
                                          SPI_tuptable->tupdesc, 2, &inull));
                    }
                    if (cur_cap < (int) nrows) { pfree(cur_level); cur_cap = (int)nrows*2; cur_level = (int64*)palloc(cur_cap*sizeof(int64)); }
                    for (uint64 j = 0; j < nrows; j++)
                    {
                        int64 nbr = nbr_ids[j]; bool found;
                        hash_search(visited, &nbr, HASH_ENTER, &found);
                        if (found) continue;
                        cur_level[next_size++] = nbr;
                        if (res_size >= res_cap) { res_cap *= 2; result = (int64*)repalloc(result, res_cap*sizeof(int64)); }
                        result[res_size++] = nbr;
                    }
                    pfree(nbr_ids);
                }
                pfree(arr);
            }

            cur_size = next_size;
            cur_depth++;

            /*
             * Adaptive switch trigger: frontier has grown past the threshold.
             * Build the full adjacency list NOW (while SPI is still open),
             * then close SPI and continue in Phase 2 (pure-C BFS).
             *
             * This is the key insight: frontier size, not depth, predicts
             * whether load-all will be faster than continued per-level calls.
             *
             * Examples that trigger switch:
             *   Tree depth=7:   frontier ~7776 at level 5  → switch
             *   Random depth=5: frontier ~256  at level 4  → switch
             *
             * Examples that never trigger (frontier stays small):
             *   Chain depth=100: frontier = 1 always
             *   Reverse ancestors: frontier = 1 always
             *   Shallow depth=3 tree: frontier max = 216 > 200 → switches
             *     but adj already loaded, C BFS handles last chunk
             */
            if (cur_size > ADAPTIVE_FRONTIER_THRESHOLD && cur_depth < max_depth)
            {
                adj = build_adj_list(rel_id, direction, bfs_work_ctx);
                MemoryContextSwitchTo(bfs_work_ctx);
                SPI_finish();
                MemoryContextSwitchTo(bfs_work_ctx);
                switched = true;
                break;
            }
        }

        if (!switched)
            SPI_finish();

        /* ── Phase 2: pure-C BFS from current frontier (if switched) ── */
        if (switched && adj != NULL && cur_size > 0 && cur_depth < max_depth)
        {
            /*
             * BFS queue seeded with the current frontier at cur_depth.
             * All previously visited nodes are already in the `visited` HTAB.
             */
            int    q_cap    = cur_size > QUEUE_INIT_SIZE ? cur_size * 2 : QUEUE_INIT_SIZE;
            int64 *q_nodes  = (int64 *) palloc(q_cap * sizeof(int64));
            int   *q_depths = (int *)   palloc(q_cap * sizeof(int));
            int    q_head   = 0, q_tail = 0;

            for (int i = 0; i < cur_size; i++)
            {
                q_nodes[q_tail]  = cur_level[i];
                q_depths[q_tail] = cur_depth;
                q_tail++;
            }

            while (q_head < q_tail)
            {
                int64 cur_id    = q_nodes[q_head];
                int   depth_now = q_depths[q_head];
                q_head++;

                if (depth_now >= max_depth) continue;

                int    n_nbrs;
                int64 *nbrs = adj_get(adj, cur_id, &n_nbrs);

                for (int i = 0; i < n_nbrs; i++)
                {
                    int64 nbr = nbrs[i]; bool found;
                    hash_search(visited, &nbr, HASH_ENTER, &found);
                    if (found) continue;

                    if (q_tail >= q_cap)
                    {
                        q_cap   *= 2;
                        q_nodes  = (int64 *) repalloc(q_nodes,  q_cap * sizeof(int64));
                        q_depths = (int *)   repalloc(q_depths, q_cap * sizeof(int));
                    }
                    q_nodes[q_tail]  = nbr;
                    q_depths[q_tail] = depth_now + 1;
                    q_tail++;

                    if (res_size >= res_cap)
                    {
                        res_cap *= 2;
                        result   = (int64 *) repalloc(result, res_cap * sizeof(int64));
                    }
                    result[res_size++] = nbr;
                }
            }
        }

        /* Copy result to long-lived context, free all working data */
        MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
        bfs->result = (int64 *) palloc((res_size > 0 ? res_size : 1) * sizeof(int64));
        if (res_size > 0)
            memcpy(bfs->result, result, res_size * sizeof(int64));
        bfs->result_size = res_size;
        bfs->result_pos  = 0;

        MemoryContextDelete(bfs_work_ctx);
        funcctx->user_fctx = bfs;
        MemoryContextSwitchTo(oldctx);
    }

    funcctx = SRF_PERCALL_SETUP();
    bfs = (BFSContext *) funcctx->user_fctx;
    if (bfs->result_pos < bfs->result_size)
        SRF_RETURN_NEXT(funcctx, Int64GetDatum(bfs->result[bfs->result_pos++]));
    SRF_RETURN_DONE(funcctx);
}
/* ================================================================
 * Bidirectional BFS — shortest path
 * ================================================================ */
typedef struct {
    int64  id;
    int64  parent_id;   /* -1 for start nodes */
} BFSNode;

typedef struct {
    BFSNode *nodes;
    int      size;
    int      capacity;
    HTAB    *visited;   /* int64 node_id → VisitedEntry */
} Frontier;

typedef struct {
    int64 node_id;      /* hash key */
    int64 parent_id;
} VisitedEntry;

static Frontier *
frontier_create(MemoryContext ctx)
{
    Frontier *f    = MemoryContextAllocZero(ctx, sizeof(Frontier));
    f->capacity    = 256;
    f->nodes       = MemoryContextAlloc(ctx, 256 * sizeof(BFSNode));
    f->size        = 0;

    HASHCTL hctl;
    memset(&hctl, 0, sizeof(hctl));
    hctl.keysize   = sizeof(int64);
    hctl.entrysize = sizeof(VisitedEntry);
    hctl.hcxt      = ctx;
    f->visited = hash_create("igraph_bidir", 256, &hctl,
                             HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
    return f;
}

static void
frontier_add(Frontier *f, int64 node_id, int64 parent_id, MemoryContext ctx)
{
    bool found;
    VisitedEntry *e = hash_search(f->visited, &node_id, HASH_ENTER, &found);
    if (found)
        return; /* already in visited — skip duplicate in nodes[] */
    e->parent_id = parent_id;

    if (f->size >= f->capacity)
    {
        /*
         * repalloc uses the memory context of the original palloc.
         * After SPI_execute_plan CurrentMemoryContext may have changed.
         * Use MemoryContextAllocHuge with explicit ctx to be safe,
         * then copy and free the old buffer.
         */
        int       new_cap  = f->capacity * 2;
        BFSNode  *new_nodes = (BFSNode *) MemoryContextAlloc(
            ctx, new_cap * sizeof(BFSNode));
        memcpy(new_nodes, f->nodes, f->size * sizeof(BFSNode));
        pfree(f->nodes);
        f->nodes    = new_nodes;
        f->capacity = new_cap;
    }
    f->nodes[f->size].id        = node_id;
    f->nodes[f->size].parent_id = parent_id;
    f->size++;
}

static bool
frontier_has(Frontier *f, int64 node_id)
{
    bool found;
    hash_search(f->visited, &node_id, HASH_FIND, &found);
    return found;
}

static int64
frontier_parent(Frontier *f, int64 node_id)
{
    bool found;
    VisitedEntry *e = hash_search(f->visited, &node_id, HASH_FIND, &found);
    return found ? e->parent_id : -1;
}

static ArrayType *
reconstruct_path(int64 meet_node, Frontier *fwd, Frontier *bwd)
{
    /*
     * Use palloc instead of stack arrays — stack frames are limited
     * and 65536 * 8 bytes = 512KB would risk stack overflow.
     * Max path length is bounded by graph diameter; 65536 is generous.
     */
    int    max_path = 65536;
    int64 *path_fwd = palloc(max_path * sizeof(int64));
    int64 *path_bwd = palloc(max_path * sizeof(int64));
    int    fwd_len  = 0;
    int    bwd_len  = 0;
    int64  cur;

    /* Walk backward from meet_node to start via fwd visited */
    cur = meet_node;
    while (cur != -1)
    {
        if (fwd_len >= max_path)
            elog(ERROR, "graph_shortest_path: path too long (>%d)", max_path);
        path_fwd[fwd_len++] = cur;
        cur = frontier_parent(fwd, cur);
    }

    /* Reverse: path_fwd now goes start → meet_node */
    for (int i = 0; i < fwd_len / 2; i++)
    {
        int64 tmp             = path_fwd[i];
        path_fwd[i]           = path_fwd[fwd_len - 1 - i];
        path_fwd[fwd_len-1-i] = tmp;
    }

    /* Walk backward from meet_node to end via bwd visited
     * Skip meet_node itself — already included in fwd path */
    cur = frontier_parent(bwd, meet_node);
    while (cur != -1)
    {
        if (bwd_len >= max_path)
            elog(ERROR, "graph_shortest_path: path too long (>%d)", max_path);
        path_bwd[bwd_len++] = cur;
        cur = frontier_parent(bwd, cur);
    }

    int    total = fwd_len + bwd_len;
    Datum *elems = palloc(total * sizeof(Datum));

    for (int i = 0; i < fwd_len; i++)
        elems[i] = Int64GetDatum(path_fwd[i]);
    for (int i = 0; i < bwd_len; i++)
        elems[fwd_len + i] = Int64GetDatum(path_bwd[i]);

    /* path_fwd and path_bwd freed by memory context cleanup */

    return construct_array(elems, total, INT8OID,
                           sizeof(int64), true, TYPALIGN_DOUBLE);
}

/*
 * expand_frontier_batch
 *
 * Replaces the old per-node expand_frontier with a single batch query
 * for the entire frontier level.
 *
 * Old: O(frontier_size) SPI round-trips per level
 * New: O(1)             SPI round-trip  per level
 *
 * The batch query returns (from_id, to_id) pairs so we can correctly
 * record the parent of each discovered neighbor.
 */
static Frontier *
expand_frontier_batch(Frontier *current, Frontier *other,
                      int16 rel_id, bool direction,
                      int64 *meet_node, MemoryContext ctx)
{
    Frontier *next = frontier_create(ctx);
    *meet_node     = -1;

    if (current->size == 0)
        return next;

    if (current->size == 1)
    {
        /*
         * Single-node fast path — same plan as the original per-node BFS.
         * Avoids the unnest() overhead that makes LATERAL slow on chains.
         */
        int64  parent = current->nodes[0].id;
        Datum  s_args[] = {
            Int64GetDatum(parent),
            Int16GetDatum(rel_id),
            BoolGetDatum(direction)
        };
        int ret = SPI_execute_plan(plan_get_neighbors, s_args, NULL, true, 0);
        if (ret != SPI_OK_SELECT || SPI_processed == 0)
            return next;

        uint64  nrows   = SPI_processed;
        int64  *to_ids  = (int64 *) MemoryContextAlloc(ctx,
                                        nrows * sizeof(int64));
        for (uint64 j = 0; j < nrows; j++)
        {
            bool isnull;
            to_ids[j] = DatumGetInt64(
                SPI_getbinval(SPI_tuptable->vals[j],
                              SPI_tuptable->tupdesc, 1, &isnull));
        }
        for (uint64 j = 0; j < nrows; j++)
        {
            int64 neighbor = to_ids[j];
            if (frontier_has(current, neighbor)) continue;
            if (frontier_has(next,    neighbor)) continue;
            frontier_add(next, neighbor, parent, ctx);
            if (*meet_node == -1 && frontier_has(other, neighbor))
                *meet_node = neighbor;
        }
        pfree(to_ids);
        return next;
    }

    /*
     * Multi-node batch path — one LATERAL query for the whole frontier.
     * Build bigint[] of all frontier node IDs.
     * Temporarily switch to ctx so construct_array allocates there
     * (safe to pfree later regardless of CurrentMemoryContext).
     */
    MemoryContext saved_ctx = MemoryContextSwitchTo(ctx);

    Datum *elems = (Datum *) palloc(current->size * sizeof(Datum));
    for (int i = 0; i < current->size; i++)
        elems[i] = Int64GetDatum(current->nodes[i].id);

    ArrayType *arr = construct_array(elems, current->size, INT8OID,
                                     sizeof(int64), true, TYPALIGN_DOUBLE);
    pfree(elems);

    MemoryContextSwitchTo(saved_ctx); /* restore before SPI call */

    Datum args[] = {
        PointerGetDatum(arr),
        Int16GetDatum(rel_id),
        BoolGetDatum(direction)
    };

    int ret = SPI_execute_plan(plan_get_neighbors_batch, args, NULL, true, 0);

    if (ret != SPI_OK_SELECT || SPI_processed == 0)
    {
        pfree(arr);
        return next;
    }

    uint64  nrows    = SPI_processed;

    /*
     * Copy (from_id, to_id) pairs out of the SPI tuptable before any
     * further SPI calls (frontier_add does no SPI, but be safe).
     */
    int64  *from_ids = (int64 *) MemoryContextAlloc(ctx, nrows * sizeof(int64));
    int64  *to_ids   = (int64 *) MemoryContextAlloc(ctx, nrows * sizeof(int64));

    for (uint64 j = 0; j < nrows; j++)
    {
        bool isnull;
        from_ids[j] = DatumGetInt64(
            SPI_getbinval(SPI_tuptable->vals[j],
                          SPI_tuptable->tupdesc, 1, &isnull));
        to_ids[j]   = DatumGetInt64(
            SPI_getbinval(SPI_tuptable->vals[j],
                          SPI_tuptable->tupdesc, 2, &isnull));
    }

    pfree(arr);

    for (uint64 j = 0; j < nrows; j++)
    {
        int64 parent   = from_ids[j];
        int64 neighbor = to_ids[j];

        if (frontier_has(current, neighbor)) continue;
        if (frontier_has(next,    neighbor)) continue;

        frontier_add(next, neighbor, parent, ctx);

        if (*meet_node == -1 && frontier_has(other, neighbor))
            *meet_node = neighbor;
    }

    pfree(from_ids);
    pfree(to_ids);

    return next;
}

/* ================================================================
 * igraph_shortest_path_internal
 *
 * Bidirectional BFS over an in-memory adjacency list.
 * Replaces the old per-node SPI approach with:
 *   1. Two bulk-load SPI calls (forward edges + reverse edges)
 *   2. Pure-C bidirectional BFS — zero further SPI calls
 *
 * Precondition: SPI must be open (caller owns SPI connection).
 * Result is palloc'd in CurrentMemoryContext (caller_ctx).
 * Returns NULL if no path exists.
 * ================================================================ */
int64 *
igraph_shortest_path_internal(int64  start_id,
                               int64  end_id,
                               int16  rel_id,
                               int   *out_len)
{
    MemoryContext caller_ctx = CurrentMemoryContext;
    *out_len = 0;

    if (start_id == end_id)
    {
        int64 *r = (int64 *) palloc(sizeof(int64));
        r[0]     = start_id;
        *out_len = 1;
        return r;
    }

    /*
     * Work context for adjacency lists, frontiers, and visited maps.
     * All freed in one shot at the end.
     */
    MemoryContext work_ctx = AllocSetContextCreate(caller_ctx,
                                                   "igraph_path_work",
                                                   ALLOCSET_DEFAULT_SIZES);

    /*
     * Load forward and reverse adjacency lists from the edges table.
     * Forward:  direction=true  — for the forward BFS frontier
     * Backward: direction=false — for the backward BFS frontier
     *   (we stored both directions explicitly, so querying direction=false
     *    gives us the reverse edges for backward expansion)
     */
    SPI_connect();
    prepare_plans();

    AdjList *fwd_adj = build_adj_list(rel_id, true,  work_ctx);
    MemoryContextSwitchTo(work_ctx);
    AdjList *bwd_adj = build_adj_list(rel_id, false, work_ctx);
    MemoryContextSwitchTo(work_ctx);

    SPI_finish();
    MemoryContextSwitchTo(work_ctx);

    /* ── Pure-C bidirectional BFS ──────────────────────────────── */

    /* visited maps: from_id → parent_id (-1 for roots) */
    typedef struct { int64 node_id; int64 parent_id; } PathEntry;

    HASHCTL hctl;
    memset(&hctl, 0, sizeof(hctl));
    hctl.keysize   = sizeof(int64);
    hctl.entrysize = sizeof(PathEntry);
    hctl.hcxt      = work_ctx;

    HTAB *fwd_vis = hash_create("igraph_fwd_vis",
                                 fwd_adj->n_nodes + 16, &hctl,
                                 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
    HTAB *bwd_vis = hash_create("igraph_bwd_vis",
                                 bwd_adj->n_nodes + 16, &hctl,
                                 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

    /* Frontiers: simple dynamic int64 arrays */
    int    fwd_cap = 256, fwd_size = 1;
    int64 *fwd_cur = (int64 *) palloc(fwd_cap * sizeof(int64));
    int    bwd_cap = 256, bwd_size = 1;
    int64 *bwd_cur = (int64 *) palloc(bwd_cap * sizeof(int64));

    fwd_cur[0] = start_id;
    bwd_cur[0] = end_id;

    /* Seed visited maps */
    {
        bool found;
        PathEntry *e;
        e = hash_search(fwd_vis, &start_id, HASH_ENTER, &found);
        e->parent_id = -1;
        e = hash_search(bwd_vis, &end_id,   HASH_ENTER, &found);
        e->parent_id = -1;
    }

    int64  meet_node = -1;
    int64 *result    = NULL;

    while (fwd_size > 0 && bwd_size > 0 && meet_node == -1)
    {
        /* Expand the smaller frontier first */
        bool expand_fwd = (fwd_size <= bwd_size);

        int64  *cur_front = expand_fwd ? fwd_cur : bwd_cur;
        int     cur_size  = expand_fwd ? fwd_size : bwd_size;
        HTAB   *cur_vis   = expand_fwd ? fwd_vis  : bwd_vis;
        HTAB   *other_vis = expand_fwd ? bwd_vis  : fwd_vis;
        AdjList *adj      = expand_fwd ? fwd_adj  : bwd_adj;

        int    *new_cap_p  = expand_fwd ? &fwd_cap  : &bwd_cap;
        int    *new_size_p = expand_fwd ? &fwd_size : &bwd_size;
        int64 **new_front_p= expand_fwd ? &fwd_cur  : &bwd_cur;

        int new_cap  = *new_cap_p;
        int new_size = 0;
        int64 *next  = (int64 *) palloc(new_cap * sizeof(int64));

        for (int i = 0; i < cur_size && meet_node == -1; i++)
        {
            int64  from = cur_front[i];
            int    n_nbrs;
            int64 *nbrs = adj_get(adj, from, &n_nbrs);

            for (int j = 0; j < n_nbrs; j++)
            {
                int64 nbr = nbrs[j];
                bool  found;

                PathEntry *e = hash_search(cur_vis, &nbr, HASH_ENTER, &found);
                if (found) continue;   /* already in this side's visited */
                e->parent_id = from;

                /* Grow next frontier */
                if (new_size >= new_cap)
                {
                    new_cap  *= 2;
                    next = (int64 *) repalloc(next, new_cap * sizeof(int64));
                }
                next[new_size++] = nbr;

                /* Check for meeting point */
                hash_search(other_vis, &nbr, HASH_FIND, &found);
                if (found && meet_node == -1)
                    meet_node = nbr;
            }
        }

        pfree(cur_front);
        *new_front_p = next;
        *new_size_p  = new_size;
        *new_cap_p   = new_cap;
    }

    /* ── Reconstruct path if found ── */
    if (meet_node != -1)
    {
        /* Walk fwd_vis from meet_node back to start */
        int    fwd_len = 0, fwd_alloc = 1024;
        int64 *fwd_path = (int64 *) palloc(fwd_alloc * sizeof(int64));
        int64  cur = meet_node;
        while (cur != -1)
        {
            if (fwd_len >= fwd_alloc)
            {
                fwd_alloc *= 2;
                fwd_path = (int64 *) repalloc(fwd_path, fwd_alloc * sizeof(int64));
            }
            fwd_path[fwd_len++] = cur;
            bool found;
            PathEntry *e = hash_search(fwd_vis, &cur, HASH_FIND, &found);
            cur = found ? e->parent_id : -1;
        }
        /* Reverse: fwd_path now goes start → meet */
        for (int i = 0; i < fwd_len / 2; i++)
        {
            int64 tmp = fwd_path[i];
            fwd_path[i] = fwd_path[fwd_len - 1 - i];
            fwd_path[fwd_len - 1 - i] = tmp;
        }

        /* Walk bwd_vis from meet_node's parent back to end */
        int    bwd_len = 0, bwd_alloc = 1024;
        int64 *bwd_path = (int64 *) palloc(bwd_alloc * sizeof(int64));
        {
            bool found;
            PathEntry *e = hash_search(bwd_vis, &meet_node, HASH_FIND, &found);
            cur = found ? e->parent_id : -1;
        }
        while (cur != -1)
        {
            if (bwd_len >= bwd_alloc)
            {
                bwd_alloc *= 2;
                bwd_path = (int64 *) repalloc(bwd_path, bwd_alloc * sizeof(int64));
            }
            bwd_path[bwd_len++] = cur;
            bool found;
            PathEntry *e = hash_search(bwd_vis, &cur, HASH_FIND, &found);
            cur = found ? e->parent_id : -1;
        }

        int total = fwd_len + bwd_len;
        MemoryContextSwitchTo(caller_ctx);
        result = (int64 *) palloc(total * sizeof(int64));
        for (int i = 0; i < fwd_len; i++) result[i] = fwd_path[i];
        for (int i = 0; i < bwd_len; i++) result[fwd_len + i] = bwd_path[i];
        *out_len = total;
    }
    else
    {
        MemoryContextSwitchTo(caller_ctx);
    }

    MemoryContextDelete(work_ctx);
    return result;
}
PG_FUNCTION_INFO_V1(graph_shortest_path);
Datum graph_shortest_path(PG_FUNCTION_ARGS)
{
    int64  start_id = PG_GETARG_INT64(0);
    int64  end_id   = PG_GETARG_INT64(1);
    text  *rel_name = PG_GETARG_TEXT_PP(2);
    char  *rel_str  = text_to_cstring(rel_name);

    if (start_id == end_id)
    {
        Datum e[] = { Int64GetDatum(start_id) };
        PG_RETURN_ARRAYTYPE_P(
            construct_array(e, 1, INT8OID, sizeof(int64), true, TYPALIGN_DOUBLE));
    }

    MemoryContext work_ctx = AllocSetContextCreate(
        CurrentMemoryContext, "igraph_bidir_bfs", ALLOCSET_DEFAULT_SIZES);

    SPI_connect();
    prepare_plans();

    Datum rel_args[] = { CStringGetTextDatum(rel_str) };
    int ret = SPI_execute_plan(plan_get_rel_id, rel_args, NULL, true, 1);
    if (ret != SPI_OK_SELECT || SPI_processed == 0)
        elog(ERROR, "graph_shortest_path: unknown rel_type '%s'", rel_str);

    bool  isnull;
    int16 rel_id = DatumGetInt16(
        SPI_getbinval(SPI_tuptable->vals[0],
                      SPI_tuptable->tupdesc, 1, &isnull));

    /*
     * fwd_vis / bwd_vis — permanent visited maps for path reconstruction.
     * fwd_cur / bwd_cur — current level nodes (swapped each iteration).
     *
     * We keep visited separate from the current-level array so that
     * fwd->size is never modified while we iterate over fwd->nodes.
     * This was the source of the 1GB alloc bug — frontier_add() was
     * called on the same frontier we were iterating, growing ->size
     * mid-loop and reading garbage memory from the extended array.
     */
    Frontier *fwd_vis = frontier_create(work_ctx);  /* all visited fwd */
    Frontier *bwd_vis = frontier_create(work_ctx);  /* all visited bwd */
    Frontier *fwd_cur = frontier_create(work_ctx);  /* current fwd level */
    Frontier *bwd_cur = frontier_create(work_ctx);  /* current bwd level */

    /* Seed both sides */
    frontier_add(fwd_vis, start_id, -1, work_ctx);
    frontier_add(bwd_vis, end_id,   -1, work_ctx);
    frontier_add(fwd_cur, start_id, -1, work_ctx);
    frontier_add(bwd_cur, end_id,   -1, work_ctx);

    int64      meet_node = -1;
    ArrayType *result    = NULL;

    while (fwd_cur->size > 0 && bwd_cur->size > 0)
    {
        Frontier *next;
        int       saved_size;
        BFSNode  *saved_nodes;

        if (fwd_cur->size <= bwd_cur->size)
        {
            /* Expand forward level — direction TRUE */
            next = expand_frontier_batch(fwd_cur, bwd_vis, rel_id, true,
                                         &meet_node, work_ctx);

            /*
             * Commit OLD level into fwd_vis first.
             * Save size/pointer BEFORE loop — frontier_add() may repalloc.
             */
            saved_size  = fwd_cur->size;
            saved_nodes = fwd_cur->nodes;
            for (int i = 0; i < saved_size; i++)
                frontier_add(fwd_vis, saved_nodes[i].id,
                             saved_nodes[i].parent_id, work_ctx);

            /*
             * BUG FIX: also commit the NEW level (next) into fwd_vis.
             * meet_node is found inside expand_frontier and added to `next`
             * with its correct parent. If we reconstruct before committing
             * `next`, frontier_parent(fwd_vis, meet_node) returns -1 and
             * the path is just [meet_node] — length 1, hops 0.
             */
            saved_size  = next->size;
            saved_nodes = next->nodes;
            for (int i = 0; i < saved_size; i++)
                frontier_add(fwd_vis, saved_nodes[i].id,
                             saved_nodes[i].parent_id, work_ctx);

            fwd_cur = next;
        }
        else
        {
            /* Expand backward level — direction FALSE */
            next = expand_frontier_batch(bwd_cur, fwd_vis, rel_id, false,
                                         &meet_node, work_ctx);

            /* Commit OLD level */
            saved_size  = bwd_cur->size;
            saved_nodes = bwd_cur->nodes;
            for (int i = 0; i < saved_size; i++)
                frontier_add(bwd_vis, saved_nodes[i].id,
                             saved_nodes[i].parent_id, work_ctx);

            /* Commit NEW level — same fix for backward direction */
            saved_size  = next->size;
            saved_nodes = next->nodes;
            for (int i = 0; i < saved_size; i++)
                frontier_add(bwd_vis, saved_nodes[i].id,
                             saved_nodes[i].parent_id, work_ctx);

            bwd_cur = next;
        }

        if (meet_node != -1)
            break;
    }

    /*
     * SPI_finish() BEFORE reconstruct_path.
     * construct_array pallocs in CurrentMemoryContext.
     * While SPI is open that is SPI private context — freed on
     * SPI_finish causing SIGSEGV on return. Close SPI first so
     * allocation lands in the caller's context.
     * fwd_vis/bwd_vis live in work_ctx (child of caller) — safe.
     */
    SPI_finish();

    if (meet_node != -1)
        result = reconstruct_path(meet_node, fwd_vis, bwd_vis);

    MemoryContextDelete(work_ctx);

    if (result == NULL)
        PG_RETURN_NULL();

    PG_RETURN_ARRAYTYPE_P(result);
}

/* ================================================================
 * graph_delete_node(node_id BIGINT) → VOID
 *
 * Atomically removes:
 *   1. All forward edges from this node (from_id = node_id)
 *   2. All reverse edges pointing to this node (to_id = node_id)
 *      — these are the reverse entries stored at neighbours
 *   3. All node properties
 *   4. The node itself
 *
 * Runs inside a single SPI session so all deletes are atomic
 * within the caller's transaction.
 * ================================================================ */
PG_FUNCTION_INFO_V1(graph_delete_node);
Datum graph_delete_node(PG_FUNCTION_ARGS)
{
    int64 node_id = PG_GETARG_INT64(0);
    int   ret;

    SPI_connect();
    prepare_plans();

    Datum args[] = { Int64GetDatum(node_id) };

    /*
     * Step 1: delete all edges where this node is the source.
     * This covers both forward (direction=TRUE) and the reverse
     * entries that were stored at this node for its neighbours.
     */
    ret = SPI_execute_plan(plan_delete_node_edges, args, NULL, false, 0);
    if (ret != SPI_OK_DELETE)
        elog(ERROR, "graph_delete_node: failed to delete edges from node %ld"
             " (SPI ret=%d)", node_id, ret);

    /*
     * Step 2: delete reverse edges at neighbours that point back
     * to this node (stored as from_id=neighbour, to_id=node_id).
     */
    ret = SPI_execute_plan(plan_delete_node_rev, args, NULL, false, 0);
    if (ret != SPI_OK_DELETE)
        elog(ERROR, "graph_delete_node: failed to delete reverse edges to node %ld"
             " (SPI ret=%d)", node_id, ret);

    /*
     * Step 3: delete all properties.
     * node_properties has ON DELETE CASCADE but we delete explicitly
     * for clarity and to avoid relying on DDL-level behaviour.
     */
    ret = SPI_execute_plan(plan_delete_node_props, args, NULL, false, 0);
    if (ret != SPI_OK_DELETE)
        elog(ERROR, "graph_delete_node: failed to delete properties of node %ld"
             " (SPI ret=%d)", node_id, ret);

    /* Step 4: delete the node row */
    ret = SPI_execute_plan(plan_delete_node_row, args, NULL, false, 0);
    if (ret != SPI_OK_DELETE)
        elog(ERROR, "graph_delete_node: failed to delete node %ld"
             " (SPI ret=%d)", node_id, ret);

    if (SPI_processed == 0)
        elog(WARNING, "graph_delete_node: node %ld not found", node_id);

    SPI_finish();
    PG_RETURN_VOID();
}

/* ================================================================
 * BYTEA header layout (matches pg_ilib convention)
 *
 * Byte 0: [op_id: 4 bits][params_hi: 4 bits]
 * Byte 1: [params_lo: 8 bits]
 *
 * op_id codes (4 bits, max 16) — matches pg_ilib:
 *   0x01 = text             params = unused, payload = raw string bytes
 *   0x02 = numeric/bigint   params = scale (0=integer, >0=decimal)
 *                           standalone: GMP bytes fill rest of varlena
 *                           in complex payload: [gmp_len:1b][GMP bytes]
 *   0x04 = timestamp/date   params = timezone offset (minutes from UTC)
 *   0x05 = bool             params = unused, payload = 1 byte
 *   0x07 = uuid             params = unused, payload = 16 bytes
 *   0x0E = complex type     params = complex_types.id (up to 4096 types)
 *   0x0F = hex/binary       params = byte count
 *
 * For complex types the payload is a sequential concatenation of fields,
 * each with its own 2-byte header. Numeric inside complex adds a 1-byte
 * length prefix since varlena boundary is not available mid-payload.
 * Field names are read from complex_type_fields ordered by position.
 * ================================================================ */
#define ILIB_OP_TEXT     0x01   /* str_to_bytea → op_id=0x01, params=0 */
#define ILIB_OP_NUMERIC  0x02   /* numeric_to_bytea → op_id=0x02, params=scale */
#define ILIB_OP_DATE     0x04   /* date_to_bytea → op_id=0x04, params=tz_offset */
#define ILIB_OP_BOOL     0x05
#define ILIB_OP_UUID     0x07
#define ILIB_OP_COMPLEX  0x0E
#define ILIB_OP_HEX      0x0F

#define ILIB_OP(b0)      (((unsigned char)(b0) & 0xF0) >> 4)
#define ILIB_PARAMS(b0, b1) ((((unsigned char)(b0) & 0x0F) << 8) | (unsigned char)(b1))

/* ================================================================
 * COMPLEX TYPES
 * ================================================================ */

static SPIPlanPtr plan_insert_complex_type  = NULL;
static SPIPlanPtr plan_get_complex_type_id  = NULL;
static SPIPlanPtr plan_insert_complex_field = NULL;
static SPIPlanPtr plan_get_complex_fields_p = NULL;
static SPIPlanPtr plan_check_complex_type   = NULL;

/* graph_delete_node plans */

static void
prepare_complex_plans(void)
{
    if (plan_insert_complex_type != NULL)
        return;

    {
        Oid t[] = { TEXTOID };
        plan_insert_complex_type = SPI_prepare(
            "INSERT INTO complex_types(name) VALUES($1)"
            " ON CONFLICT (name) DO UPDATE SET name = EXCLUDED.name"
            " RETURNING id",
            1, t);
        if (!plan_insert_complex_type)
            elog(ERROR, "pg_igraph: failed to prepare plan_insert_complex_type");
        SPI_keepplan(plan_insert_complex_type);
    }

    {
        Oid t[] = { TEXTOID };
        plan_get_complex_type_id = SPI_prepare(
            "SELECT id FROM complex_types WHERE name = $1",
            1, t);
        if (!plan_get_complex_type_id)
            elog(ERROR, "pg_igraph: failed to prepare plan_get_complex_type_id");
        SPI_keepplan(plan_get_complex_type_id);
    }

    {
        Oid t[] = { INT2OID, INT2OID, TEXTOID };
        plan_insert_complex_field = SPI_prepare(
            "INSERT INTO complex_type_fields(type_id, pos, field_name)"
            " VALUES($1, $2, $3)"
            " ON CONFLICT (type_id, pos)"
            " DO UPDATE SET field_name = EXCLUDED.field_name",
            3, t);
        if (!plan_insert_complex_field)
            elog(ERROR, "pg_igraph: failed to prepare plan_insert_complex_field");
        SPI_keepplan(plan_insert_complex_field);
    }

    {
        Oid t[] = { INT2OID };
        plan_get_complex_fields_p = SPI_prepare(
            "SELECT pos, field_name FROM complex_type_fields"
            " WHERE type_id = $1 ORDER BY pos",
            1, t);
        if (!plan_get_complex_fields_p)
            elog(ERROR, "pg_igraph: failed to prepare plan_get_complex_fields_p");
        SPI_keepplan(plan_get_complex_fields_p);
    }

    /* Verify complex type exists before inserting fields */
    {
        Oid t[] = { INT2OID };
        plan_check_complex_type = SPI_prepare(
            "SELECT 1 FROM complex_types WHERE id = $1", 1, t);
        if (!plan_check_complex_type)
            elog(ERROR, "pg_igraph: failed to prepare plan_check_complex_type");
        SPI_keepplan(plan_check_complex_type);
    }
}

/*
 * graph_add_complex_type(type_name TEXT) → SMALLINT
 * Registers a complex type, returns its id.
 * Use this id in BYTEA header: op_id=0x0E, params=id
 */
PG_FUNCTION_INFO_V1(graph_add_complex_type);
Datum graph_add_complex_type(PG_FUNCTION_ARGS)
{
    text  *type_name = PG_GETARG_TEXT_PP(0);
    char  *name_str  = text_to_cstring(type_name);
    bool   isnull;
    int    ret;
    int16  type_id;

    SPI_connect();
    prepare_complex_plans();

    /*
     * Try INSERT first. If the name already exists ON CONFLICT
     * fires an UPDATE which still returns SPI_OK_INSERT_RETURNING.
     * However some PG versions return SPI_OK_INSERT_RETURNING only
     * on actual insert. We handle both cases: first try insert,
     * then fallback to SELECT if insert returns unexpected code.
     */
    Datum args[] = { CStringGetTextDatum(name_str) };
    ret = SPI_execute_plan(plan_insert_complex_type, args, NULL, false, 1);

    if ((ret == SPI_OK_INSERT_RETURNING || ret == SPI_OK_INSERT) && SPI_processed > 0)
    {
        type_id = DatumGetInt16(
            SPI_getbinval(SPI_tuptable->vals[0],
                          SPI_tuptable->tupdesc, 1, &isnull));
    }
    else
    {
        /* Fallback: SELECT the id directly */
        ret = SPI_execute_plan(plan_get_complex_type_id, args, NULL, true, 1);
        if (ret != SPI_OK_SELECT || SPI_processed == 0)
            elog(ERROR, "graph_add_complex_type: failed to find or create '%s'"
                 " (SPI ret=%d)", name_str, ret);
        type_id = DatumGetInt16(
            SPI_getbinval(SPI_tuptable->vals[0],
                          SPI_tuptable->tupdesc, 1, &isnull));
    }

    elog(DEBUG1, "graph_add_complex_type: '%s' → id=%d", name_str, type_id);

    SPI_finish();
    PG_RETURN_INT16(type_id);
}

/*
 * graph_add_complex_field(type_id, position, name) → VOID
 * Registers a field name at a given position within a complex type.
 * Position defines the order in the binary payload.
 */
PG_FUNCTION_INFO_V1(graph_add_complex_field);
Datum graph_add_complex_field(PG_FUNCTION_ARGS)
{
    int16  type_id    = PG_GETARG_INT16(0);
    int16  pos        = PG_GETARG_INT16(1);
    text  *field_name = PG_GETARG_TEXT_PP(2);
    char  *fname_str  = text_to_cstring(field_name);
    int    ret;

    SPI_connect();
    prepare_complex_plans();

    /* Verify type_id exists — uses cached prepared plan */
    {
        Datum chk_args[] = { Int16GetDatum(type_id) };
        ret = SPI_execute_plan(plan_check_complex_type, chk_args, NULL, true, 1);
        if (ret != SPI_OK_SELECT)
            elog(ERROR, "graph_add_complex_field: check query failed (SPI ret=%d)", ret);
        if (SPI_processed == 0)
            elog(ERROR,
                 "graph_add_complex_field: complex type id=%d not found "
                 "— call graph_add_complex_type() first", type_id);
    }

    Datum args[] = {
        Int16GetDatum(type_id),
        Int16GetDatum(pos),
        CStringGetTextDatum(fname_str)
    };
    char nulls[] = { ' ', ' ', ' ' };

    ret = SPI_execute_plan(plan_insert_complex_field, args, nulls, false, 0);
    if (ret != SPI_OK_INSERT)
        elog(ERROR, "graph_add_complex_field: insert failed for type_id=%d pos=%d"
             " (SPI ret=%d)", type_id, pos, ret);

    SPI_finish();
    PG_RETURN_VOID();
}

/*
 * graph_get_complex_fields(type_id) → TABLE(pos SMALLINT, field_name TEXT)
 *
 * Uses AttInMetadata so PostgreSQL knows the output tuple descriptor.
 * Each row is built from C strings via BuildTupleFromCStrings.
 */

typedef struct {
    int     nrows;
    int16  *pos_vals;
    char  **name_vals;
} ComplexFieldsResult;

PG_FUNCTION_INFO_V1(graph_get_complex_fields);
Datum graph_get_complex_fields(PG_FUNCTION_ARGS)
{
    FuncCallContext      *funcctx;
    ComplexFieldsResult  *res;

    if (SRF_IS_FIRSTCALL())
    {
        int16        type_id = PG_GETARG_INT16(0);
        MemoryContext oldctx;
        TupleDesc     tupdesc;

        funcctx = SRF_FIRSTCALL_INIT();
        oldctx  = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        /* Build output tuple descriptor matching RETURNS TABLE definition */
        tupdesc = CreateTemplateTupleDesc(2);
        TupleDescInitEntry(tupdesc, 1, "pos",        INT2OID, -1, 0);
        TupleDescInitEntry(tupdesc, 2, "field_name", TEXTOID, -1, 0);
        funcctx->tuple_desc = BlessTupleDesc(tupdesc);

        SPI_connect();
        prepare_complex_plans();

        Datum args[] = { Int16GetDatum(type_id) };
        int ret = SPI_execute_plan(plan_get_complex_fields_p, args, NULL, true, 0);
        if (ret != SPI_OK_SELECT)
            elog(ERROR, "graph_get_complex_fields: query failed (SPI ret=%d)", ret);

        uint64 nrows = SPI_processed;
        res = palloc(sizeof(ComplexFieldsResult));
        res->nrows     = (int) nrows;
        res->pos_vals  = palloc(nrows * sizeof(int16));
        res->name_vals = palloc(nrows * sizeof(char *));

        for (uint64 i = 0; i < nrows; i++)
        {
            bool isnull;
            res->pos_vals[i] = DatumGetInt16(
                SPI_getbinval(SPI_tuptable->vals[i],
                              SPI_tuptable->tupdesc, 1, &isnull));
            res->name_vals[i] = isnull ? pstrdup("")
                : TextDatumGetCString(
                    SPI_getbinval(SPI_tuptable->vals[i],
                                  SPI_tuptable->tupdesc, 2, &isnull));
        }

        funcctx->max_calls  = nrows;
        funcctx->user_fctx  = res;

        SPI_finish();
        MemoryContextSwitchTo(oldctx);
    }

    funcctx = SRF_PERCALL_SETUP();
    res     = (ComplexFieldsResult *) funcctx->user_fctx;

    if (funcctx->call_cntr < funcctx->max_calls)
    {
        uint64    i    = funcctx->call_cntr;
        Datum     vals[2];
        bool      nulls[2] = { false, false };
        HeapTuple tup;

        vals[0] = Int16GetDatum(res->pos_vals[i]);
        vals[1] = CStringGetTextDatum(res->name_vals[i]);

        tup = heap_form_tuple(funcctx->tuple_desc, vals, nulls);
        SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tup));
    }

    SRF_RETURN_DONE(funcctx);
}

/*
 * ilib_field_size — determine byte size of a single pg_ilib field
 * given its op_id, params, and a pointer to its payload start.
 *
 * Used by the complex type decoder to advance the read pointer.
 */
/*
 * ilib_field_size — byte size of a single pg_ilib field payload
 * (does NOT include the 2-byte op_id/params header).
 *
 * Numeric encoding inside complex types:
 *   After the standard 2-byte header the writer prepends one length byte:
 *     [0x02][scale][gmp_len[GMP bytes * gmp_len]
 *   At write time the GMP export size is known, so gmp_len is stored
 *   as a 1-byte prefix. This makes numeric self-describing at any
 *   position in the payload and supports arbitrarily large integers.
 *   Returned size includes the 1-byte length prefix.
 */
static size_t
ilib_field_size(uint8_t op_id, uint16_t params, const unsigned char *payload,
                const unsigned char *end)
{
    if (payload >= end)
        return 0;

    switch (op_id)
    {
        case ILIB_OP_TEXT:
            /*
             * str_to_bytea: header [0x10][0x00] then raw string bytes.
             * params=0, so we consume all remaining bytes up to end.
             * Inside complex payload this means text must not be
             * followed by other fields unless we know its length.
             * For now: consume to end (safe when text is last field,
             * or when used standalone).
             */
            return (size_t)(end - payload);

        case ILIB_OP_NUMERIC:
            /*
             * Inside complex payload: [gmp_len: 1 byte][GMP bytes].
             * Total = 1 + gmp_len.
             */
            return (size_t)(1 + payload[0]);

        case ILIB_OP_DATE:
            /* always 4 bytes (uint32 unix timestamp) */
            return 4;

        case ILIB_OP_BOOL:
            return 1;

        case ILIB_OP_UUID:
            return 16;

        case ILIB_OP_HEX:
            /* params = byte count */
            return (size_t) params;

        default:
            elog(ERROR, "pg_igraph: unknown op_id 0x%02x in complex field",
                 op_id);
            return 0;
    }
}

/*
 * Decode a single pg_ilib primitive field to a JsonbValue.
 * Caller provides op_id, params, payload pointer and size.
 */
static void
ilib_field_to_jsonb(uint8_t op_id, uint16_t params,
                    const unsigned char *payload, size_t size,
                    JsonbValue *out)
{
    switch (op_id)
    {
        case ILIB_OP_NUMERIC:
        {
            /*
             * Inside complex payload: [gmp_len: 1 byte][GMP bytes]
             * params = scale (0=integer, >0=decimal point position)
             * Return as hex string — client decodes with pg_ilib
             * bytea_to_numeric() which reconstructs via GMP.
             */
            uint8_t gmp_len = payload[0];
            const unsigned char *gmp = payload + 1;
            static const char hx[] = "0123456789abcdef";
            char *hex = palloc(gmp_len * 2 + 1);
            for (uint8_t k = 0; k < gmp_len; k++)
            {
                hex[k*2]   = hx[gmp[k] >> 4];
                hex[k*2+1] = hx[gmp[k] & 0x0f];
            }
            hex[gmp_len * 2]    = '\0';
            out->type           = jbvString;
            out->val.string.val = hex;
            out->val.string.len = (int)(gmp_len * 2);
            break;
        }

        case ILIB_OP_TEXT:
        {
            /*
             * str_to_bytea layout: [0x10][0x00][raw string bytes]
             * payload points after the 2-byte header, size = remaining bytes.
             * The string is not null-terminated in the BYTEA.
             */
            char *str = palloc(size + 1);
            memcpy(str, payload, size);
            str[size]             = '\0';
            out->type             = jbvString;
            out->val.string.val   = str;
            out->val.string.len   = (int) size;
            break;
        }

        case ILIB_OP_BOOL:
        {
            out->type         = jbvBool;
            out->val.boolean  = (payload[0] != 0);
            break;
        }

        case ILIB_OP_UUID:
        {
            /* Format as standard UUID string */
            char buf[37];
            snprintf(buf, sizeof(buf),
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x"
                "-%02x%02x%02x%02x%02x%02x",
                payload[0],  payload[1],  payload[2],  payload[3],
                payload[4],  payload[5],  payload[6],  payload[7],
                payload[8],  payload[9],  payload[10], payload[11],
                payload[12], payload[13], payload[14], payload[15]);
            out->type           = jbvString;
            out->val.string.val = pstrdup(buf);
            out->val.string.len = 36;
            break;
        }

        case ILIB_OP_DATE:
        {
            /* params = timezone offset in minutes; payload = uint32 unix ts */
            uint32_t ts = 0;
            for (size_t i = 0; i < 4; i++)
                ts = (ts << 8) | payload[i];
            /* Return as numeric unix timestamp for now */
            out->type = jbvNumeric;
            out->val.numeric = DatumGetNumeric(
                DirectFunctionCall1(int4_numeric, Int32GetDatum((int32) ts)));
            break;
        }

        case ILIB_OP_HEX:
        default:
        {
            /* Return as hex string — client interprets */
            static const char hxx[] = "0123456789abcdef";
            char *hex = palloc(size * 2 + 1);
            for (size_t i = 0; i < size; i++)
            {
                hex[i*2]   = hxx[payload[i] >> 4];
                hex[i*2+1] = hxx[payload[i] & 0x0f];
            }
            hex[size*2]         = '\0';
            out->type           = jbvString;
            out->val.string.val = hex;
            out->val.string.len = (int)(size * 2);
            break;
        }
    }
}

/* ================================================================
 * PROPERTIES
 * ================================================================
 *
 * graph_set_property(node_id, prop_name, primitive, value, ref_label)
 *
 * primitive codes:
 *   1=bigint  2=text  3=uuid  4=timestamp  5=bool  6=numeric  7=jsonb
 *
 * ref_label — pass NULL for plain properties.
 *             Pass the node label name when this property is a
 *             typed reference to another node (e.g. "User").
 *             The resolver can then auto-expand the BYTEA uuid
 *             into a full node object on read.
 */
PG_FUNCTION_INFO_V1(graph_set_property);
Datum graph_set_property(PG_FUNCTION_ARGS)
{
    int64  node_id   = PG_GETARG_INT64(0);
    text  *prop_name = PG_GETARG_TEXT_PP(1);
    int16  primitive = PG_GETARG_INT16(2);
    bytea *value     = PG_GETARG_BYTEA_PP(3);

    /* ref_label is optional — arg 4 may be NULL */
    char  *ref_label_str = NULL;
    int16  ref_label_id  = 0;   /* 0 = NULL in NULLIF($3, 0) */

    if (!PG_ARGISNULL(4))
        ref_label_str = text_to_cstring(PG_GETARG_TEXT_PP(4));

    char  *prop_str = text_to_cstring(prop_name);
    bool   isnull;

    if (primitive < 1 || primitive > 7)
        elog(ERROR, "graph_set_property: invalid primitive %d (must be 1-7)", primitive);

    SPI_connect();
    prepare_plans();

    /* Resolve ref_label → id if provided */
    if (ref_label_str != NULL)
    {
        Datum largs[] = { CStringGetTextDatum(ref_label_str) };
        int ret = SPI_execute_plan(plan_get_label_id, largs, NULL, true, 1);
        if (ret != SPI_OK_SELECT || SPI_processed == 0)
            elog(ERROR, "graph_set_property: unknown ref_label '%s'", ref_label_str);
        ref_label_id = DatumGetInt16(
            SPI_getbinval(SPI_tuptable->vals[0],
                          SPI_tuptable->tupdesc, 1, &isnull));
    }

    /* Upsert property_type — returns id */
    Datum pt_args[] = {
        CStringGetTextDatum(prop_str),
        Int16GetDatum(primitive),
        Int16GetDatum(ref_label_id)     /* 0 → NULLIF → NULL */
    };
    int ret = SPI_execute_plan(plan_insert_prop_type, pt_args, NULL, false, 1);
    if (ret != SPI_OK_INSERT_RETURNING || SPI_processed == 0)
        elog(ERROR, "graph_set_property: failed to upsert property_type '%s'",
             prop_str);

    int16 prop_id = DatumGetInt16(
        SPI_getbinval(SPI_tuptable->vals[0],
                      SPI_tuptable->tupdesc, 1, &isnull));

    /* Upsert the value */
    Datum v_args[] = {
        Int64GetDatum(node_id),
        Int16GetDatum(prop_id),
        PointerGetDatum(value)
    };
    ret = SPI_execute_plan(plan_upsert_prop, v_args, NULL, false, 0);
    if (ret != SPI_OK_INSERT)
        elog(ERROR, "graph_set_property: failed to upsert value for node %ld prop '%s'"
             " (SPI ret=%d)", node_id, prop_str, ret);

    SPI_finish();
    PG_RETURN_VOID();
}

/* ================================================================
 * graph_get_property(node_id BIGINT, prop_name TEXT) → BYTEA
 * Returns raw BYTEA — caller decodes with pg_ilib helpers
 * Returns NULL if property not found
 * ================================================================ */
PG_FUNCTION_INFO_V1(graph_get_property);
Datum graph_get_property(PG_FUNCTION_ARGS)
{
    int64  node_id   = PG_GETARG_INT64(0);
    text  *prop_name = PG_GETARG_TEXT_PP(1);
    char  *prop_str  = text_to_cstring(prop_name);
    bool   isnull;

    SPI_connect();
    prepare_plans();

    Datum args[] = {
        Int64GetDatum(node_id),
        CStringGetTextDatum(prop_str)
    };

    int ret = SPI_execute_plan(plan_get_prop, args, NULL, true, 1);
    if (ret != SPI_OK_SELECT)
        elog(ERROR, "graph_get_property: query failed");

    if (SPI_processed == 0)
    {
        SPI_finish();
        PG_RETURN_NULL();
    }

    Datum val = SPI_getbinval(SPI_tuptable->vals[0],
                              SPI_tuptable->tupdesc, 1, &isnull);
    if (isnull)
    {
        SPI_finish();
        PG_RETURN_NULL();
    }

    /* Copy result out of SPI memory context */
    bytea *result = DatumGetByteaPCopy(val);

    SPI_finish();
    PG_RETURN_BYTEA_P(result);
}

/* ================================================================
 * graph_delete_property(node_id BIGINT, prop_name TEXT) → VOID
 * ================================================================ */
PG_FUNCTION_INFO_V1(graph_delete_property);
Datum graph_delete_property(PG_FUNCTION_ARGS)
{
    int64  node_id   = PG_GETARG_INT64(0);
    text  *prop_name = PG_GETARG_TEXT_PP(1);
    char  *prop_str  = text_to_cstring(prop_name);

    SPI_connect();
    prepare_plans();

    Datum args[] = {
        Int64GetDatum(node_id),
        CStringGetTextDatum(prop_str)
    };

    int ret = SPI_execute_plan(plan_delete_prop, args, NULL, false, 0);
    if (ret != SPI_OK_DELETE)
        elog(ERROR, "graph_delete_property: failed for node %ld prop '%s'",
             node_id, prop_str);

    SPI_finish();
    PG_RETURN_VOID();
}

/* ================================================================
 * graph_get_node_properties(node_id BIGINT) → JSONB
 *
 * Returns all properties as JSONB:
 * {
 *   "name":  { "primitive": 2, "value": "<hex>" },
 *   "price": { "primitive": 6, "value": "<hex>" },
 *   "owner": { "primitive": 3, "value": "<hex>", "ref_label": "User" }
 * }
 *
 * The "value" is hex-encoded BYTEA — client decodes with pg_ilib.
 * "ref_label" present only for typed references.
 * ================================================================ */
PG_FUNCTION_INFO_V1(graph_get_node_properties);
Datum graph_get_node_properties(PG_FUNCTION_ARGS)
{
    int64 node_id = PG_GETARG_INT64(0);

    SPI_connect();
    prepare_plans();

    Datum args[] = { Int64GetDatum(node_id) };
    int ret = SPI_execute_plan(plan_get_all_props, args, NULL, true, 0);
    if (ret != SPI_OK_SELECT)
        elog(ERROR, "graph_get_node_properties: query failed");

    /*
     * Build JSONB manually using JsonbInState.
     * Structure: object → per-prop object → { primitive, value }
     */
    JsonbParseState *state = NULL;
    JsonbValue       jv;

    pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);

    for (uint64 i = 0; i < SPI_processed; i++)
    {
        bool      isnull;
        TupleDesc td = SPI_tuptable->tupdesc;
        HeapTuple ht = SPI_tuptable->vals[i];

        /* Column 1: prop name */
        Datum d_name = SPI_getbinval(ht, td, 1, &isnull);
        if (isnull) continue;
        char *prop_name = TextDatumGetCString(d_name);

        /* Column 3: raw bytea value */
        Datum  d_val = SPI_getbinval(ht, td, 3, &isnull);
        if (isnull) continue;

        bytea         *bv  = DatumGetByteaPCopy(d_val);
        unsigned char *raw = (unsigned char *) VARDATA_ANY(bv);
        int            blen = VARSIZE_ANY_EXHDR(bv);

        if (blen < 2) continue;  /* malformed */

        uint8_t  op_id  = ILIB_OP(raw[0]);
        uint16_t params = ILIB_PARAMS(raw[0], raw[1]);

        /* Emit JSON key */
        jv.type           = jbvString;
        jv.val.string.val = prop_name;
        jv.val.string.len = strlen(prop_name);
        pushJsonbValue(&state, WJB_KEY, &jv);

        if (op_id == ILIB_OP_COMPLEX)
        {
            /*
             * Complex type: params = complex_type_id
             * Payload starts at raw+2 and contains sequential fields,
             * each with its own 2-byte pg_ilib header.
             * Fetch field names from complex_type_fields, then decode.
             */
            uint16_t type_id = params;

            /* Fetch field names — need a nested SPI call */
            prepare_complex_plans();

            Datum cf_args[] = { Int16GetDatum((int16) type_id) };
            int cret = SPI_execute_plan(plan_get_complex_fields_p,
                                        cf_args, NULL, true, 0);
            if (cret != SPI_OK_SELECT)
                elog(ERROR, "graph_get_node_properties: failed to fetch "
                     "complex fields for type_id=%d", type_id);

            /* Copy field names out before next SPI call overwrites tuptable */
            uint64  nfields    = SPI_processed;
            char  **field_names = palloc(nfields * sizeof(char *));
            for (uint64 f = 0; f < nfields; f++)
            {
                bool fn;
                Datum dn = SPI_getbinval(SPI_tuptable->vals[f],
                                         SPI_tuptable->tupdesc, 2, &fn);
                field_names[f] = fn ? pstrdup("?") : TextDatumGetCString(dn);
            }

            /* Decode payload field by field */
            unsigned char *ptr = raw + 2;   /* skip complex header */
            unsigned char *end = raw + blen;

            pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);

            for (uint64 f = 0; f < nfields && ptr + 2 <= end; f++)
            {
                uint8_t  fop    = ILIB_OP(ptr[0]);
                uint16_t fmeta  = ILIB_PARAMS(ptr[0], ptr[1]);
                ptr += 2;

                size_t fsize = ilib_field_size(fop, fmeta, ptr, end);
                if (ptr + fsize > end)
                    fsize = (size_t)(end - ptr);  /* clamp to safe range */

                /* Emit field key */
                jv.type           = jbvString;
                jv.val.string.val = field_names[f];
                jv.val.string.len = strlen(field_names[f]);
                pushJsonbValue(&state, WJB_KEY, &jv);

                /* Decode and emit field value */
                JsonbValue fval;
                ilib_field_to_jsonb(fop, fmeta, ptr, fsize, &fval);
                pushJsonbValue(&state, WJB_VALUE, &fval);

                ptr += fsize;
            }

            pushJsonbValue(&state, WJB_END_OBJECT, NULL);
        }
        else
        {
            /*
             * Primitive type: decode payload (raw+2 onwards) directly.
             * Emit decoded value — no wrapper object, clean JSON value.
             */
            unsigned char *payload = raw + 2;
            size_t         psize   = (size_t)(blen - 2);

            JsonbValue pval;
            ilib_field_to_jsonb(op_id, params, payload, psize, &pval);
            pushJsonbValue(&state, WJB_VALUE, &pval);
        }
    }

    JsonbValue *result_jv = pushJsonbValue(&state, WJB_END_OBJECT, NULL);
    Jsonb      *result    = JsonbValueToJsonb(result_jv);

    SPI_finish();
    PG_RETURN_POINTER(result);
}
