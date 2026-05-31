/*
 * igraph_exec.c
 * Executor: walks the IgraphStmt AST and calls pg_igraph functions
 * via SPI, returning results as Jsonb.
 */

#include "postgres.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/memutils.h"
#include "lib/stringinfo.h"
#include "catalog/pg_type.h"
#include "utils/numeric.h"
#include "utils/array.h"

#include "igraph_query.h"

/* ── Forward declarations ──────────────────────── */
static Jsonb *exec_match_ctx(IgraphStmtMatch *m, IgraphExecContext *ctx);
static Jsonb *exec_path_ctx(IgraphStmtPath *p, IgraphExecContext *ctx);
static Jsonb *exec_create_node_ctx(IgraphStmtCreateNode *cn, IgraphExecContext *ctx);
static Jsonb *exec_create_edge_ctx(IgraphStmtCreateEdge *ce, IgraphExecContext *ctx);
static Jsonb *exec_delete_node_ctx(IgraphStmtDeleteNode *dn, IgraphExecContext *ctx);
static Jsonb *exec_delete_edge_ctx(IgraphStmtDeleteEdge *de, IgraphExecContext *ctx);
static Jsonb *exec_set_prop_ctx(IgraphStmtSetProp *sp, IgraphExecContext *ctx);
static Jsonb *exec_get_props_ctx(IgraphStmtGetProps *gp, IgraphExecContext *ctx);

/* ── JSONB helpers ─────────────────────────────── */

static void jb_begin_object(JsonbParseState **s)
{ pushJsonbValue(s, WJB_BEGIN_OBJECT, NULL); }

static void jb_end_object(JsonbParseState **s)
{ pushJsonbValue(s, WJB_END_OBJECT, NULL); }

static void jb_begin_array(JsonbParseState **s)
{ pushJsonbValue(s, WJB_BEGIN_ARRAY, NULL); }

static void jb_end_array(JsonbParseState **s)
{ pushJsonbValue(s, WJB_END_ARRAY, NULL); }

static void
jb_key(JsonbParseState **s, const char *key)
{
    JsonbValue jv;
    jv.type           = jbvString;
    jv.val.string.val = (char *) key;
    jv.val.string.len = strlen(key);
    pushJsonbValue(s, WJB_KEY, &jv);
}

static void
jb_str(JsonbParseState **s, const char *val)
{
    JsonbValue jv;
    jv.type           = jbvString;
    jv.val.string.val = (char *) val;
    jv.val.string.len = strlen(val);
    pushJsonbValue(s, WJB_VALUE, &jv);
}

static void
jb_int(JsonbParseState **s, int64 val)
{
    /*
     * Emit integer as JSON string "123" rather than numeric.
     *
     * Using jbvNumeric requires DirectFunctionCall(numeric_in/int8_numeric)
     * which sets up PostgreSQL function call infrastructure and may
     * switch CurrentMemoryContext — corrupting the JsonbParseState
     * when called 1000+ times in a tight loop (e.g. PATH result array).
     *
     * jbvString with pstrdup is a plain palloc — safe, no call stack,
     * no context switches. JSON consumers handle "123" as a number.
     * If strict numeric JSON is needed, callers can cast on the client.
     */
    JsonbValue jv;
    char       buf[32];
    int        len;

    len = snprintf(buf, sizeof(buf), INT64_FORMAT, val);

    jv.type           = jbvNumeric;
    jv.val.numeric    = DatumGetNumeric(
        DirectFunctionCall3(numeric_in,
                            CStringGetDatum(pstrdup(buf)),
                            ObjectIdGetDatum(InvalidOid),
                            Int32GetDatum(-1)));
    pushJsonbValue(s, WJB_VALUE, &jv);
    (void) len;
}

/* jb_int_str removed — unused */

/* jb_bool removed — unused */

static Jsonb *
jb_finalise(JsonbParseState **s)
{
    JsonbValue *res = pushJsonbValue(s, WJB_END_OBJECT, NULL);
    return JsonbValueToJsonb(res);
}

static Jsonb *
wrap_ok_simple(void)
{
    JsonbParseState *s = NULL;
    jb_begin_object(&s);
    jb_key(&s, "status"); jb_str(&s, "ok");
    return jb_finalise(&s);
}

static Jsonb *
wrap_node_id(int64 node_id)
{
    JsonbParseState *s = NULL;
    jb_begin_object(&s);
    jb_key(&s, "status");  jb_str(&s, "ok");
    jb_key(&s, "node_id"); jb_int(&s, node_id);
    return jb_finalise(&s);
}

/* ================================================================
 * exec_match
 * ================================================================ */
static Jsonb *
exec_match(IgraphStmtMatch *m)
{
    return exec_match_ctx(m, NULL);
}

/* ================================================================
 * exec_path
 * ================================================================ */
static Jsonb *
exec_path(IgraphStmtPath *p)
{
    return exec_path_ctx(p, NULL);
}

/* ================================================================
 * exec_create_node
 * ================================================================ */
static Jsonb *
exec_create_node(IgraphStmtCreateNode *cn)
{
    return exec_create_node_ctx(cn, NULL);
}

/* ================================================================
 * exec_create_edge
 * ================================================================ */
static Jsonb *
exec_create_edge(IgraphStmtCreateEdge *ce)
{
    return exec_create_edge_ctx(ce, NULL);
}

/* ================================================================
 * exec_delete_node
 * ================================================================ */
static Jsonb *
exec_delete_node(IgraphStmtDeleteNode *dn)
{
    Oid   argtypes[] = { INT8OID };
    Datum args[]     = { Int64GetDatum(dn->node_id) };
    int   ret;
    MemoryContext caller_ctx = CurrentMemoryContext;

    SPI_connect();
    MemoryContextSwitchTo(caller_ctx);
    ret = SPI_execute_with_args(
        "SELECT graph_delete_node($1)",
        1, argtypes, args, NULL, false, 0);
    SPI_finish();

    if (ret != SPI_OK_SELECT && ret != SPI_OK_UTILITY)
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("igraph DELETE NODE: failed")));

    return wrap_ok_simple();
}

/* ================================================================
 * exec_delete_edge
 * ================================================================ */
static Jsonb *
exec_delete_edge(IgraphStmtDeleteEdge *de)
{
    MemoryContext caller_ctx = CurrentMemoryContext;
    Oid   argtypes[] = { INT8OID, INT8OID, TEXTOID };
    Datum args[]     = {
        Int64GetDatum(de->from_id),
        Int64GetDatum(de->to_id),
        CStringGetTextDatum(de->rel_type)
    };
    int ret;

    SPI_connect();
    MemoryContextSwitchTo(caller_ctx);
    ret = SPI_execute_with_args(
        "DELETE FROM edges e "
        "USING rel_types r "
        "WHERE r.id = e.rel_type AND r.name = $3 "
        "  AND ((e.from_id=$1 AND e.to_id=$2)"
        "    OR (e.from_id=$2 AND e.to_id=$1))",
        3, argtypes, args, NULL, false, 0);

    SPI_finish();
    if (ret != SPI_OK_DELETE)
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("igraph DELETE EDGE: failed (ret=%d)", ret)));

    return wrap_ok_simple();
}

/* ================================================================
 * exec_set_prop
 * ================================================================ */
static Jsonb *
exec_set_prop(IgraphStmtSetProp *sp)
{
    int ret = 0;
    MemoryContext caller_ctx = CurrentMemoryContext;

    SPI_connect();
    MemoryContextSwitchTo(caller_ctx);
    switch (sp->val.type)
    {
        case IGRAPH_VAL_STRING:
        {
            Oid   ba[]    = { TEXTOID };
            Datum bargs[] = { CStringGetTextDatum(sp->val.sval) };
            bool  isnull;
            Datum bval;
            bytea *bv;
            Oid   at[] = { INT8OID, TEXTOID, INT2OID, BYTEAOID };
            Datum a[4];

            ret = SPI_execute_with_args(
                "SELECT str_to_bytea($1)", 1, ba, bargs, NULL, true, 1);
            if (ret != SPI_OK_SELECT || SPI_processed == 0)
                ereport(ERROR, (errmsg("igraph SET: str_to_bytea failed")));

            bval = SPI_getbinval(SPI_tuptable->vals[0],
                                 SPI_tuptable->tupdesc, 1, &isnull);
            bv   = DatumGetByteaPCopy(bval);
            a[0] = Int64GetDatum(sp->node_id);
            a[1] = CStringGetTextDatum(sp->prop_name);
            a[2] = Int16GetDatum(1);
            a[3] = PointerGetDatum(bv);
            ret  = SPI_execute_with_args(
                "SELECT graph_set_property($1,$2,$3::smallint,$4,NULL)",
                4, at, a, NULL, false, 0);
            break;
        }

        case IGRAPH_VAL_INT:
        case IGRAPH_VAL_FLOAT:
        {
            double dval  = (sp->val.type == IGRAPH_VAL_INT)
                           ? (double) sp->val.ival : sp->val.fval;
            int    scale = (sp->val.type == IGRAPH_VAL_INT) ? 0 : 2;
            Oid    ba[]  = { FLOAT8OID, INT4OID };
            Datum  bargs[] = { Float8GetDatum(dval), Int32GetDatum(scale) };
            bool   isnull;
            Datum  bval;
            bytea *bv;
            Oid    at[] = { INT8OID, TEXTOID, INT2OID, BYTEAOID };
            Datum  a[4];

            ret = SPI_execute_with_args(
                "SELECT numeric_to_bytea($1::numeric,$2)",
                2, ba, bargs, NULL, true, 1);
            if (ret != SPI_OK_SELECT || SPI_processed == 0)
                ereport(ERROR,(errmsg("igraph SET: numeric_to_bytea failed")));

            bval = SPI_getbinval(SPI_tuptable->vals[0],
                                 SPI_tuptable->tupdesc, 1, &isnull);
            bv   = DatumGetByteaPCopy(bval);
            a[0] = Int64GetDatum(sp->node_id);
            a[1] = CStringGetTextDatum(sp->prop_name);
            a[2] = Int16GetDatum(2);
            a[3] = PointerGetDatum(bv);
            ret  = SPI_execute_with_args(
                "SELECT graph_set_property($1,$2,$3::smallint,$4,NULL)",
                4, at, a, NULL, false, 0);
            break;
        }

        case IGRAPH_VAL_BOOL:
        {
            unsigned char buf[3];
            bytea *bv;
            Oid    at[] = { INT8OID, TEXTOID, INT2OID, BYTEAOID };
            Datum  a[4];

            buf[0] = (0x05 << 4);
            buf[1] = 0x00;
            buf[2] = sp->val.bval ? 0x01 : 0x00;
            bv     = (bytea *) palloc(VARHDRSZ + 3);
            SET_VARSIZE(bv, VARHDRSZ + 3);
            memcpy(VARDATA(bv), buf, 3);
            a[0]   = Int64GetDatum(sp->node_id);
            a[1]   = CStringGetTextDatum(sp->prop_name);
            a[2]   = Int16GetDatum(5);
            a[3]   = PointerGetDatum(bv);
            ret    = SPI_execute_with_args(
                "SELECT graph_set_property($1,$2,$3::smallint,$4,NULL)",
                4, at, a, NULL, false, 0);
            break;
        }

        case IGRAPH_VAL_NULL:
        {
            Oid   at[] = { INT8OID, TEXTOID };
            Datum a[]  = {
                Int64GetDatum(sp->node_id),
                CStringGetTextDatum(sp->prop_name)
            };
            ret = SPI_execute_with_args(
                "SELECT graph_delete_property($1,$2)",
                2, at, a, NULL, false, 0);
            break;
        }
    }

    (void) ret;
    SPI_finish();
    return wrap_ok_simple();
}

/* ================================================================
 * exec_get_props
 * ================================================================ */
static Jsonb *
exec_get_props(IgraphStmtGetProps *gp)
{
    Oid              argtypes[] = { INT8OID };
    Datum            args[]     = { Int64GetDatum(gp->node_id) };
    int              ret;
    JsonbParseState *s          = NULL;
    MemoryContext    caller_ctx  = CurrentMemoryContext;

    SPI_connect();
    MemoryContextSwitchTo(caller_ctx);
    ret = SPI_execute_with_args(
        "SELECT graph_get_node_properties($1)",
        1, argtypes, args, NULL, true, 1);

    if (ret != SPI_OK_SELECT)
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("igraph GET PROPERTIES: query failed")));

    jb_begin_object(&s);
    jb_key(&s, "status");  jb_str(&s, "ok");
    jb_key(&s, "node_id"); jb_int(&s, gp->node_id);
    jb_key(&s, "properties");

    if (SPI_processed == 0)
    {
        jb_begin_object(&s); jb_end_object(&s);
    }
    else
    {
        bool  isnull;
        Datum d = SPI_getbinval(SPI_tuptable->vals[0],
                                SPI_tuptable->tupdesc, 1, &isnull);
        if (isnull)
        {
            jb_begin_object(&s); jb_end_object(&s);
        }
        else
        {
            JsonbValue jv;
            Jsonb     *props = DatumGetJsonbP(d);
            jv.type            = jbvBinary;
            jv.val.binary.data = &props->root;
            jv.val.binary.len  = VARSIZE(props) - VARHDRSZ;
            pushJsonbValue(&s, WJB_VALUE, &jv);
        }
    }

    SPI_finish();
    return jb_finalise(&s);
}

/* ================================================================
 * Helper: resolve value with parameter substitution
 * ================================================================ */
static IgraphValue
resolve_value(IgraphValue val, IgraphExecContext *ctx)
{
    if (val.type == IGRAPH_VAL_PARAM && ctx && ctx->json_params)
    {
        return igraph_resolve_param(val.param_path, ctx->json_params);
    }
    return val;
}

/* ================================================================
 * Helper: get node property value for WHERE condition evaluation
 * ================================================================ */
static IgraphValue
get_node_property_value(int64 node_id, const char *prop_name, IgraphExecContext *ctx)
{
    IgraphValue result = { .type = IGRAPH_VAL_NULL };

    if (!prop_name) {
        return result;
    }

    /* Special case: "id" property returns the node ID itself */
    if (strcmp(prop_name, "id") == 0) {
        result.type = IGRAPH_VAL_INT;
        result.ival = node_id;
        return result;
    }

    /* For other properties, query the database */
    MemoryContext caller_ctx = CurrentMemoryContext;
    bool isnull = false;
    int ret;

    SPI_connect();
    MemoryContextSwitchTo(caller_ctx);

    /* Build query with table prefix */
    StringInfoData query;
    initStringInfo(&query);
    appendStringInfo(&query, "SELECT graph_get_property($1, $2");

    /* Add table_prefix parameter if needed */
    if (ctx && ctx->table_prefix && strlen(ctx->table_prefix) > 0) {
        appendStringInfo(&query, ", $3");
    }
    appendStringInfo(&query, ")");

    /* Execute query */
    if (ctx && ctx->table_prefix && strlen(ctx->table_prefix) > 0) {
        Oid argtypes[] = { INT8OID, TEXTOID, TEXTOID };
        Datum args[] = {
            Int64GetDatum(node_id),
            CStringGetDatum(prop_name),
            CStringGetDatum(ctx->table_prefix)
        };
        ret = SPI_execute_with_args(query.data, 3, argtypes, args, NULL, true, 1);
    } else {
        Oid argtypes[] = { INT8OID, TEXTOID };
        Datum args[] = {
            Int64GetDatum(node_id),
            CStringGetDatum(prop_name)
        };
        ret = SPI_execute_with_args(query.data, 2, argtypes, args, NULL, true, 1);
    }

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        Datum d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
        if (!isnull) {
            char *prop_value = TextDatumGetCString(d);

            /* Try to determine the type and convert */
            if (prop_value) {
                /* Try to parse as integer first */
                char *endptr;
                long long_val = strtoll(prop_value, &endptr, 10);
                if (*endptr == '\0') {
                    result.type = IGRAPH_VAL_INT;
                    result.ival = long_val;
                } else {
                    /* Try to parse as float */
                    double double_val = strtod(prop_value, &endptr);
                    if (*endptr == '\0') {
                        result.type = IGRAPH_VAL_FLOAT;
                        result.fval = double_val;
                    } else {
                        /* Treat as string */
                        result.type = IGRAPH_VAL_STRING;
                        result.sval = pstrdup(prop_value);
                    }
                }
            }
        }
    }

    SPI_finish();
    pfree(query.data);

    return result;
}

/* ================================================================
 * Helper: compare two IgraphValue based on operator
 * ================================================================ */
static bool
compare_values(IgraphValue left, IgraphCondOp op, IgraphValue right)
{
    /* Handle NULL cases */
    if (left.type == IGRAPH_VAL_NULL || right.type == IGRAPH_VAL_NULL) {
        switch (op) {
            case COND_EQ:  return (left.type == IGRAPH_VAL_NULL && right.type == IGRAPH_VAL_NULL);
            case COND_NEQ: return (left.type != right.type);
            default:       return false;  /* NULL comparisons with <, >, etc. are false */
        }
    }

    /* Both values are non-NULL, compare based on types */

    /* Integer comparisons */
    if (left.type == IGRAPH_VAL_INT && right.type == IGRAPH_VAL_INT) {
        switch (op) {
            case COND_EQ:  return left.ival == right.ival;
            case COND_NEQ: return left.ival != right.ival;
            case COND_LT:  return left.ival < right.ival;
            case COND_LTE: return left.ival <= right.ival;
            case COND_GT:  return left.ival > right.ival;
            case COND_GTE: return left.ival >= right.ival;
        }
    }

    /* Float comparisons (promote int to float if needed) */
    if ((left.type == IGRAPH_VAL_FLOAT || left.type == IGRAPH_VAL_INT) &&
        (right.type == IGRAPH_VAL_FLOAT || right.type == IGRAPH_VAL_INT)) {

        double left_val = (left.type == IGRAPH_VAL_FLOAT) ? left.fval : (double)left.ival;
        double right_val = (right.type == IGRAPH_VAL_FLOAT) ? right.fval : (double)right.ival;

        switch (op) {
            case COND_EQ:  return left_val == right_val;
            case COND_NEQ: return left_val != right_val;
            case COND_LT:  return left_val < right_val;
            case COND_LTE: return left_val <= right_val;
            case COND_GT:  return left_val > right_val;
            case COND_GTE: return left_val >= right_val;
        }
    }

    /* String comparisons */
    if (left.type == IGRAPH_VAL_STRING && right.type == IGRAPH_VAL_STRING) {
        int cmp = strcmp(left.sval ? left.sval : "", right.sval ? right.sval : "");
        switch (op) {
            case COND_EQ:  return cmp == 0;
            case COND_NEQ: return cmp != 0;
            case COND_LT:  return cmp < 0;
            case COND_LTE: return cmp <= 0;
            case COND_GT:  return cmp > 0;
            case COND_GTE: return cmp >= 0;
        }
    }

    /* Boolean comparisons */
    if (left.type == IGRAPH_VAL_BOOL && right.type == IGRAPH_VAL_BOOL) {
        switch (op) {
            case COND_EQ:  return left.bval == right.bval;
            case COND_NEQ: return left.bval != right.bval;
            default:       return false;  /* Boolean doesn't support <, >, etc. */
        }
    }

    /* Type mismatch - only equality checks make sense */
    switch (op) {
        case COND_EQ:  return false;  /* Different types can't be equal */
        case COND_NEQ: return true;   /* Different types are not equal */
        default:       return false;  /* Can't compare different types with <, >, etc. */
    }
}

/* ================================================================
 * Helper: evaluate WHERE condition against a node
 * ================================================================ */
static bool
evaluate_condition(IgraphCond *cond, int64 node_id, const char *node_alias, IgraphExecContext *ctx)
{
    if (!cond) {
        return true;  /* No condition = always true */
    }

    /* Check if this condition applies to the current node alias */
    if (cond->alias && node_alias && strcmp(cond->alias, node_alias) != 0) {
        return true;  /* Condition doesn't apply to this node, so it's "true" for this node */
    }

    /* Get the property value for this node */
    IgraphValue node_prop_value = get_node_property_value(node_id, cond->prop, ctx);

    /* Resolve the condition value (may contain JSON parameters) */
    IgraphValue condition_value = resolve_value(cond->val, ctx);

    /* Compare the values */
    return compare_values(node_prop_value, cond->op, condition_value);
}

/* ================================================================
 * Helper: build table name with prefix
 * ================================================================ */
static char *
build_table_name(const char *base_name, IgraphExecContext *ctx)
{
    if (ctx && ctx->table_prefix && strlen(ctx->table_prefix) > 0)
    {
        StringInfoData buf;
        initStringInfo(&buf);
        appendStringInfo(&buf, "%s_%s", ctx->table_prefix, base_name);
        return buf.data;
    }
    return pstrdup(base_name);
}

/* ================================================================
 * igraph_execute — dispatch (legacy, uses default table names)
 * ================================================================ */
Jsonb *
igraph_execute(IgraphStmt *stmt)
{
    return igraph_execute_with_context(stmt, NULL);
}

/* ================================================================
 * igraph_execute_with_context — dispatch with execution context
 * ================================================================ */
Jsonb *
igraph_execute_with_context(IgraphStmt *stmt, IgraphExecContext *ctx)
{
    switch (stmt->type)
    {
        case STMT_MATCH:       return exec_match_ctx(&stmt->match, ctx);
        case STMT_PATH:        return exec_path_ctx(&stmt->path, ctx);
        case STMT_CREATE_NODE: return exec_create_node_ctx(&stmt->create_node, ctx);
        case STMT_CREATE_EDGE: return exec_create_edge_ctx(&stmt->create_edge, ctx);
        case STMT_DELETE_NODE: return exec_delete_node_ctx(&stmt->delete_node, ctx);
        case STMT_DELETE_EDGE: return exec_delete_edge_ctx(&stmt->delete_edge, ctx);
        case STMT_SET_PROP:    return exec_set_prop_ctx(&stmt->set_prop, ctx);
        case STMT_GET_PROPS:   return exec_get_props_ctx(&stmt->get_props, ctx);
        default:
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("igraph: unknown statement type %d", stmt->type)));
    }
    return NULL;
}

/* ================================================================
 * Context-aware executor functions with table prefix support
 * ================================================================ */

static Jsonb *
exec_match_ctx(IgraphStmtMatch *m, IgraphExecContext *ctx)
{
    MemoryContext    caller_ctx = CurrentMemoryContext;
    int64            start_id;
    const char      *rel_type;
    bool             direction;
    int              max_depth;
    JsonbParseState *s;
    IgraphCond      *c;
    int              ret;
    uint64           nrows;
    int64           *node_ids;
    uint64           i;
    char            *nodes_table, *node_labels_table;

    start_id  = -1;
    rel_type  = m->rel->rel_type;
    direction = (m->rel->dir != DIR_LEFT);
    max_depth = m->rel->max_depth;
    s         = NULL;

    /* Build table names with prefix */
    nodes_table = build_table_name("nodes", ctx);
    node_labels_table = build_table_name("node_labels", ctx);

    c = m->where;
    while (c)
    {
        if (m->src->alias &&
            strcmp(c->alias, m->src->alias) == 0 &&
            strcmp(c->prop, "id") == 0 &&
            c->op == COND_EQ)
        {
            IgraphValue resolved_val = resolve_value(c->val, ctx);
            if (resolved_val.type == IGRAPH_VAL_INT)
            {
                start_id = resolved_val.ival;
                break;
            }
        }
        c = c->next;
    }

    SPI_connect();
    MemoryContextSwitchTo(caller_ctx); /* stay in caller ctx */

    if (start_id >= 0) {
        /* Use specific starting node if found */
        Oid   argtypes[] = { INT8OID, TEXTOID, BOOLOID, INT4OID };
        Datum args[]     = {
            Int64GetDatum(start_id),
            CStringGetTextDatum(rel_type),
            BoolGetDatum(direction),
            Int32GetDatum(max_depth)
        };
        ret = SPI_execute_with_args(
            "SELECT * FROM graph_traverse($1,$2,$3,$4)",
            4, argtypes, args, NULL, true, 0);
    } else {
        /* No specific start ID - get all nodes with the source label */
        StringInfoData query;
        initStringInfo(&query);
        appendStringInfo(&query,
            "SELECT DISTINCT n.id FROM %s n "
            "JOIN %s nl ON nl.id = n.label ",
            nodes_table, node_labels_table);

        if (m->src && m->src->label) {
            appendStringInfo(&query, "WHERE nl.name = '%s'", m->src->label);
        }

        ret = SPI_execute(query.data, true, 0);
        pfree(query.data);
    }

    if (ret != SPI_OK_SELECT)
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("igraph MATCH: graph_traverse failed (ret=%d)", ret)));

    nrows    = SPI_processed;
    node_ids = (int64 *) palloc(nrows * sizeof(int64));

    for (i = 0; i < nrows; i++)
    {
        bool isnull;
        node_ids[i] = DatumGetInt64(
            SPI_getbinval(SPI_tuptable->vals[i],
                          SPI_tuptable->tupdesc, 1, &isnull));
    }

    /*
     * Use StringInfo instead of JsonbParseState to avoid corruption
     * from SPI memory context switches (same approach as exec_path)
     */
    StringInfoData json_buf;
    initStringInfo(&json_buf);
    appendStringInfo(&json_buf, "[");  /* Start with array only, no status field */

    bool first_result = true;
    for (i = 0; i < nrows; i++)
    {
        int64       nid;
        char       *label;
        Jsonb      *props;
        bool        isnull;
        int         lret;
        int         pret;
        StringInfoData query;
        bool        node_passes_filter = true;
        IgraphCond *cond;

        nid   = node_ids[i];
        label = NULL;
        props = NULL;

        /* Evaluate all WHERE conditions for this node */
        cond = m->where;
        while (cond && node_passes_filter)
        {
            /* For source nodes */
            if (m->src && m->src->alias) {
                if (!evaluate_condition(cond, nid, m->src->alias, ctx)) {
                    node_passes_filter = false;
                    break;
                }
            }

            /* For target nodes - we need to get target node IDs first */
            /* This is more complex and would need traversal logic */
            /* For now, let's focus on source node filtering */

            cond = cond->next;
        }

        /* Skip this node if it doesn't pass the filter */
        if (!node_passes_filter) {
            continue;
        }

        /* Query label with prefixed table names */
        initStringInfo(&query);
        appendStringInfo(&query,
            "SELECT nl.name FROM %s n "
            "JOIN %s nl ON nl.id = n.label "
            "WHERE n.id = $1",
            nodes_table, node_labels_table);

        {
            Oid   la[]    = { INT8OID };
            Datum largs[] = { Int64GetDatum(nid) };
            lret = SPI_execute_with_args(
                query.data,
                1, la, largs, NULL, true, 1);
            if (lret == SPI_OK_SELECT && SPI_processed > 0)
                label = TextDatumGetCString(
                    SPI_getbinval(SPI_tuptable->vals[0],
                                  SPI_tuptable->tupdesc, 1, &isnull));
        }

        if (m->dst && m->dst->label && label &&
            strcmp(label, m->dst->label) != 0)
            continue;

        {
            Oid   pa[]    = { INT8OID };
            Datum pargs[] = { Int64GetDatum(nid) };
            pret = SPI_execute_with_args(
                "SELECT graph_get_node_properties($1, '')",
                1, pa, pargs, NULL, true, 1);
            if (pret == SPI_OK_SELECT && SPI_processed > 0)
            {
                Datum d = SPI_getbinval(SPI_tuptable->vals[0],
                                        SPI_tuptable->tupdesc, 1, &isnull);
                if (!isnull)
                    props = DatumGetJsonbP(d);
            }
        }

        /* Add comma separator if not first item */
        if (!first_result) {
            appendStringInfoChar(&json_buf, ',');
        }
        first_result = false;

        /* Build JSON object with StringInfo */
        appendStringInfo(&json_buf, "{\"id\":%ld", nid);
        if (label) {
            appendStringInfo(&json_buf, ",\"label\":\"%s\"", label);
        }

        /* TODO: Add full returns and properties support with StringInfo later */
        /*
        if (m->returns)
        {
            // Complex return field processing - disabled for now
        }
        else if (props)
        {
            // Complex properties processing - disabled for now
        }
        */

        /* Close JSON object */
        appendStringInfoChar(&json_buf, '}');
    }

    appendStringInfo(&json_buf, "]");  /* Close array only */

    /* Convert string to Jsonb safely */
    Datum jsonb_datum;
    Jsonb *result;

    /* If no results were added, return empty object instead of empty array */
    if (first_result) {
        jsonb_datum = DirectFunctionCall1(jsonb_in, CStringGetDatum("{}"));
    } else {
        jsonb_datum = DirectFunctionCall1(jsonb_in, CStringGetDatum(json_buf.data));
    }
    result = DatumGetJsonbP(jsonb_datum);

    /* Clean up */
    pfree(json_buf.data);
    pfree(node_ids);
    pfree(nodes_table);
    pfree(node_labels_table);
    SPI_finish();

    return result;
}

static Jsonb *
exec_path_ctx(IgraphStmtPath *p, IgraphExecContext *ctx)
{
    /* For now, PATH command doesn't use table prefixes directly */
    /* since it uses internal igraph_shortest_path_internal function */
    return exec_path(p);
}

static Jsonb *
exec_create_node_ctx(IgraphStmtCreateNode *cn, IgraphExecContext *ctx)
{
    MemoryContext caller_ctx = CurrentMemoryContext;
    int           ret;
    int64         new_id;
    bool          isnull;

    SPI_connect();
    MemoryContextSwitchTo(caller_ctx);

    {
        Oid   argtypes[] = { TEXTOID };
        Datum args[]     = { CStringGetTextDatum(cn->label) };
        ret = SPI_execute_with_args(
            "SELECT graph_add_node($1)",
            1, argtypes, args, NULL, false, 1);
    }

    if (ret != SPI_OK_SELECT || SPI_processed != 1)
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("igraph CREATE NODE: graph_add_node failed")));

    new_id = DatumGetInt64(
        SPI_getbinval(SPI_tuptable->vals[0],
                      SPI_tuptable->tupdesc, 1, &isnull));

    SPI_finish();

    /* Build result */
    {
        JsonbParseState *s = NULL;
        jb_begin_object(&s);
        jb_key(&s, "status"); jb_str(&s, "ok");
        jb_key(&s, "node_id"); jb_int(&s, new_id);
        return jb_finalise(&s);
    }
}

static Jsonb *
exec_create_edge_ctx(IgraphStmtCreateEdge *ce, IgraphExecContext *ctx)
{
    MemoryContext caller_ctx = CurrentMemoryContext;
    int           ret;

    SPI_connect();
    MemoryContextSwitchTo(caller_ctx);

    {
        Oid   argtypes[] = { INT8OID, INT8OID, TEXTOID };
        Datum args[]     = {
            Int64GetDatum(ce->from_id),
            Int64GetDatum(ce->to_id),
            CStringGetTextDatum(ce->rel_type)
        };
        ret = SPI_execute_with_args(
            "SELECT graph_add_edge($1,$2,$3)",
            3, argtypes, args, NULL, false, 0);
    }

    if (ret != SPI_OK_SELECT)
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("igraph CREATE EDGE: graph_add_edge failed")));

    SPI_finish();
    return wrap_ok_simple();
}

static Jsonb *
exec_delete_node_ctx(IgraphStmtDeleteNode *dn, IgraphExecContext *ctx)
{
    MemoryContext caller_ctx = CurrentMemoryContext;
    int           ret;

    SPI_connect();
    MemoryContextSwitchTo(caller_ctx);

    {
        Oid   argtypes[] = { INT8OID };
        Datum args[]     = { Int64GetDatum(dn->node_id) };
        ret = SPI_execute_with_args(
            "SELECT graph_delete_node($1)",
            1, argtypes, args, NULL, false, 0);
    }

    if (ret != SPI_OK_SELECT)
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("igraph DELETE NODE: graph_delete_node failed")));

    SPI_finish();
    return wrap_ok_simple();
}

static Jsonb *
exec_delete_edge_ctx(IgraphStmtDeleteEdge *de, IgraphExecContext *ctx)
{
    MemoryContext caller_ctx = CurrentMemoryContext;
    int           ret;

    SPI_connect();
    MemoryContextSwitchTo(caller_ctx);

    {
        Oid   argtypes[] = { INT8OID, INT8OID, TEXTOID };
        Datum args[]     = {
            Int64GetDatum(de->from_id),
            Int64GetDatum(de->to_id),
            CStringGetTextDatum(de->rel_type)
        };
        ret = SPI_execute_with_args(
            "DELETE FROM edges WHERE from_id = $1 AND to_id = $2 AND rel_id = (SELECT id FROM edge_labels WHERE name = $3)",
            3, argtypes, args, NULL, false, 0);
    }

    if (ret != SPI_OK_DELETE)
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("igraph DELETE EDGE: delete failed")));

    SPI_finish();
    return wrap_ok_simple();
}

static Jsonb *
exec_set_prop_ctx(IgraphStmtSetProp *sp, IgraphExecContext *ctx)
{
    MemoryContext caller_ctx = CurrentMemoryContext;
    int           ret;
    IgraphValue   resolved_val;
    const char   *type_name = "text";

    resolved_val = resolve_value(sp->val, ctx);

    /* Determine type for graph_set_property */
    switch (resolved_val.type)
    {
        case IGRAPH_VAL_INT:    type_name = "int64"; break;
        case IGRAPH_VAL_FLOAT:  type_name = "float64"; break;
        case IGRAPH_VAL_STRING: type_name = "text"; break;
        case IGRAPH_VAL_BOOL:   type_name = "bool"; break;
        default:                type_name = "text"; break;
    }

    SPI_connect();
    MemoryContextSwitchTo(caller_ctx);

    if (resolved_val.type == IGRAPH_VAL_INT)
    {
        char val_str[32];
        snprintf(val_str, sizeof(val_str), INT64_FORMAT, resolved_val.ival);

        Oid   argtypes[] = { INT8OID, TEXTOID, TEXTOID, TEXTOID };
        Datum args[]     = {
            Int64GetDatum(sp->node_id),
            CStringGetTextDatum(sp->prop_name),
            CStringGetTextDatum(type_name),
            CStringGetTextDatum(val_str)
        };
        ret = SPI_execute_with_args(
            "SELECT graph_set_property($1,$2,$3,$4)",
            4, argtypes, args, NULL, false, 0);
    }
    else if (resolved_val.type == IGRAPH_VAL_STRING)
    {
        Oid   argtypes[] = { INT8OID, TEXTOID, TEXTOID, TEXTOID };
        Datum args[]     = {
            Int64GetDatum(sp->node_id),
            CStringGetTextDatum(sp->prop_name),
            CStringGetTextDatum(type_name),
            CStringGetTextDatum(resolved_val.sval)
        };
        ret = SPI_execute_with_args(
            "SELECT graph_set_property($1,$2,$3,$4)",
            4, argtypes, args, NULL, false, 0);
    }
    else
    {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("igraph SET: unsupported value type")));
    }

    if (ret != SPI_OK_SELECT)
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("igraph SET: graph_set_property failed")));

    SPI_finish();
    return wrap_ok_simple();
}

static Jsonb *
exec_get_props_ctx(IgraphStmtGetProps *gp, IgraphExecContext *ctx)
{
    MemoryContext caller_ctx = CurrentMemoryContext;
    int           ret;
    Jsonb        *props;
    bool          isnull;

    SPI_connect();
    MemoryContextSwitchTo(caller_ctx);

    {
        Oid   argtypes[] = { INT8OID };
        Datum args[]     = { Int64GetDatum(gp->node_id) };
        ret = SPI_execute_with_args(
            "SELECT graph_get_node_properties($1)",
            1, argtypes, args, NULL, true, 1);
    }

    if (ret != SPI_OK_SELECT || SPI_processed != 1)
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("igraph GET PROPERTIES: graph_get_node_properties failed")));

    Datum d = SPI_getbinval(SPI_tuptable->vals[0],
                            SPI_tuptable->tupdesc, 1, &isnull);
    if (isnull)
        props = NULL;
    else
        props = DatumGetJsonbP(d);

    SPI_finish();

    {
        JsonbParseState *s = NULL;
        jb_begin_object(&s);
        jb_key(&s, "status"); jb_str(&s, "ok");
        jb_key(&s, "node_id"); jb_int(&s, gp->node_id);

        if (props)
        {
            JsonbValue jv;
            jb_key(&s, "properties");
            jv.type            = jbvBinary;
            jv.val.binary.data = &props->root;
            jv.val.binary.len  = VARSIZE(props) - VARHDRSZ;
            pushJsonbValue(&s, WJB_VALUE, &jv);
        }
        else
        {
            jb_key(&s, "properties");
            jb_begin_object(&s);
            jb_end_object(&s);
        }

        return jb_finalise(&s);
    }
}

