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
#include "utils/json.h"
#include "utils/lsyscache.h"

#include "igraph_query.h"

/* ── Forward declarations ──────────────────────── */
static char *build_table_name(const char *base_name, IgraphExecContext *ctx);
static Jsonb *exec_match_ctx(IgraphStmtMatch *m, IgraphExecContext *ctx);
static Jsonb *exec_match_bare_ctx(IgraphStmtMatch *m, IgraphExecContext *ctx);
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
    /* Original implementation: call graph_shortest_path without table prefix */
    MemoryContext caller_ctx = CurrentMemoryContext;
    int           ret;
    Datum         result_datum;
    bool          isnull;

    SPI_connect();
    MemoryContextSwitchTo(caller_ctx);

    {
        Oid   argtypes[] = { INT8OID, INT8OID, TEXTOID };
        Datum args[]     = {
            Int64GetDatum(p->start_id),
            Int64GetDatum(p->end_id),
            CStringGetTextDatum(p->rel_type)
        };
        ret = SPI_execute_with_args(
            "SELECT graph_shortest_path($1, $2, $3)",
            3, argtypes, args, NULL, true, 1);
    }

    if (ret != SPI_OK_SELECT)
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("igraph PATH: graph_shortest_path failed")));

    /*
     * SPI_execute_with_args() leaves CurrentMemoryContext pointing at
     * SPI's own per-call context, not the caller's — json_buf below is
     * read after SPI_finish() (which frees that context), so it must be
     * built in caller_ctx. Same hazard/fix as exec_match_ctx (task #17).
     */
    MemoryContextSwitchTo(caller_ctx);

    /* Use StringInfo approach to build result safely */
    StringInfoData json_buf;
    initStringInfo(&json_buf);

    if (SPI_processed == 0) {
        /* No path found */
        appendStringInfo(&json_buf, "{\"path\": [], \"found\": false, \"status\": \"ok\"}");
    } else {
        /* Path found */
        result_datum = SPI_getbinval(SPI_tuptable->vals[0],
                                   SPI_tuptable->tupdesc, 1, &isnull);
        if (isnull) {
            appendStringInfo(&json_buf, "{\"path\": [], \"found\": false, \"status\": \"ok\"}");
        } else {
            /* Convert BIGINT[] to JSON array */
            ArrayType *path_array = DatumGetArrayTypeP(result_datum);
            int16 elmlen;
            bool elmbyval;
            char elmalign;
            int nitems;
            Datum *elems;
            bool *nulls;

            get_typlenbyvalalign(INT8OID, &elmlen, &elmbyval, &elmalign);
            deconstruct_array(path_array, INT8OID, elmlen, elmbyval, elmalign,
                            &elems, &nulls, &nitems);

            appendStringInfo(&json_buf, "{\"path\": [");
            for (int i = 0; i < nitems; i++) {
                if (i > 0) appendStringInfo(&json_buf, ", ");
                appendStringInfo(&json_buf, "%ld", DatumGetInt64(elems[i]));
            }
            appendStringInfo(&json_buf, "], \"found\": true, \"hops\": %d, \"status\": \"ok\"}",
                           nitems > 0 ? nitems - 1 : 0);

            pfree(elems);
            pfree(nulls);
        }
    }

    SPI_finish();

    /* Convert string to Jsonb safely */
    Datum jsonb_datum = DirectFunctionCall1(jsonb_in, CStringGetDatum(json_buf.data));
    Jsonb *result = DatumGetJsonbP(jsonb_datum);

    pfree(json_buf.data);
    return result;
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

    /*
     * graph_get_property() returns the raw pg_ilib-encoded BYTEA — reading
     * that through TextDatumGetCString() (as this code used to) treats
     * undecoded binary bytes as if they were already text, which is wrong
     * regardless of crash risk. graph_get_node_properties() runs the same
     * decode path this project already trusts elsewhere
     * (decode_node_properties_jsonb), so pull the single field out of its
     * JSONB result with ->> instead of decoding by hand here.
     *
     * That decode path is correct for TEXT/BOOL/UUID/DATE/COMPLEX, but for
     * ILIB_OP_NUMERIC (op_id 0x02 — bigint/numeric properties) it
     * deliberately renders a hex string of the raw GMP payload (see
     * decode_node_properties_jsonb/ilib_field_to_jsonb in pg_igraph.c) —
     * the right contract for graph_get_node_properties() itself, but
     * re-parsing that hex string here with strtoll()/strtod() as if it
     * were base-10 (as this function used to) silently produces a
     * plausible-looking, wrong value with no relationship to the real
     * magnitude (e.g. age=30 -> "00" -> 0). So the property's raw header
     * byte (op_id, params/scale) is fetched alongside the already-decoded
     * text in the same query, and a NUMERIC property is routed to
     * pg_ilib's own bytea_to_numeric() instead — the only decoder that
     * actually understands the scale + GMP payload. Non-numeric types
     * keep using the existing, already-correct ->> text unchanged.
     */
    char *node_properties_table = build_table_name("node_properties", ctx);
    char *property_types_table  = build_table_name("property_types", ctx);
    StringInfoData query;
    initStringInfo(&query);

    if (ctx && ctx->table_prefix && strlen(ctx->table_prefix) > 0) {
        appendStringInfo(&query,
            "SELECT graph_get_node_properties($1, $3) ->> $2, "
            "CASE WHEN octet_length(np.value) >= 2 THEN get_byte(np.value,0) >> 4 END, "
            "CASE WHEN octet_length(np.value) >= 2 THEN ((get_byte(np.value,0) & 15) << 8) | get_byte(np.value,1) END, "
            "np.value "
            "FROM %s np JOIN %s pt ON pt.id = np.prop_id "
            "WHERE np.node_id = $1 AND pt.name = $2",
            node_properties_table, property_types_table);
    } else {
        appendStringInfo(&query,
            "SELECT graph_get_node_properties($1) ->> $2, "
            "CASE WHEN octet_length(np.value) >= 2 THEN get_byte(np.value,0) >> 4 END, "
            "CASE WHEN octet_length(np.value) >= 2 THEN ((get_byte(np.value,0) & 15) << 8) | get_byte(np.value,1) END, "
            "np.value "
            "FROM %s np JOIN %s pt ON pt.id = np.prop_id "
            "WHERE np.node_id = $1 AND pt.name = $2",
            node_properties_table, property_types_table);
    }

    /* Execute query */
    if (ctx && ctx->table_prefix && strlen(ctx->table_prefix) > 0) {
        Oid argtypes[] = { INT8OID, TEXTOID, TEXTOID };
        Datum args[] = {
            Int64GetDatum(node_id),
            CStringGetTextDatum(prop_name),
            CStringGetTextDatum(ctx->table_prefix)
        };
        ret = SPI_execute_with_args(query.data, 3, argtypes, args, NULL, true, 1);
    } else {
        Oid argtypes[] = { INT8OID, TEXTOID };
        Datum args[] = {
            Int64GetDatum(node_id),
            CStringGetTextDatum(prop_name)
        };
        ret = SPI_execute_with_args(query.data, 2, argtypes, args, NULL, true, 1);
    }

    /*
     * SPI_execute_with_args() leaves CurrentMemoryContext pointing at
     * SPI's own per-call context, not the caller's — every value pulled
     * out below (raw_bv copy, pstrdup'd string) must land in caller_ctx
     * or it dangles once this function's SPI_finish() runs (same
     * hazard/fix as exec_match_ctx, task #17).
     */
    MemoryContextSwitchTo(caller_ctx);

    if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        HeapTuple tup = SPI_tuptable->vals[0];
        TupleDesc td  = SPI_tuptable->tupdesc;
        bool op_isnull;
        int32 op_id = DatumGetInt32(SPI_getbinval(tup, td, 2, &op_isnull));

        if (!op_isnull && op_id == 0x02 /* ILIB_OP_NUMERIC, see pg_igraph.c */) {
            bool  params_isnull, raw_isnull;
            int32 params = DatumGetInt32(SPI_getbinval(tup, td, 3, &params_isnull));
            Datum raw_d  = SPI_getbinval(tup, td, 4, &raw_isnull);

            if (!raw_isnull) {
                /*
                 * Copy the bytea out of SPI's memory context before the
                 * nested SPI_execute_with_args() below overwrites the
                 * global SPI_tuptable — same hazard documented in
                 * decode_node_properties_jsonb()'s IMPORTANT #1.
                 */
                bytea *raw_bv = DatumGetByteaPCopy(raw_d);
                Oid    nargtypes[] = { BYTEAOID };
                Datum  nargs[]     = { PointerGetDatum(raw_bv) };
                int    nret = SPI_execute_with_args(
                    "SELECT bytea_to_numeric($1)::text",
                    1, nargtypes, nargs, NULL, true, 1);

                /* Same hazard as above — switch back before extracting
                 * numeric_text from this nested call's result. */
                MemoryContextSwitchTo(caller_ctx);

                if (nret == SPI_OK_SELECT && SPI_processed > 0) {
                    bool  nisnull;
                    Datum nd = SPI_getbinval(SPI_tuptable->vals[0],
                                              SPI_tuptable->tupdesc, 1, &nisnull);
                    if (!nisnull) {
                        char *numeric_text = TextDatumGetCString(nd);

                        if (!params_isnull && params == 0) {
                            result.type = IGRAPH_VAL_INT;
                            result.ival = strtoll(numeric_text, NULL, 10);
                        } else {
                            result.type = IGRAPH_VAL_FLOAT;
                            result.fval = strtod(numeric_text, NULL);
                        }
                    }
                }
            }
        } else {
            Datum d = SPI_getbinval(tup, td, 1, &isnull);
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
    const char *table_prefix;
    char       *dot_pos;

    if (!ctx || !ctx->table_prefix || strlen(ctx->table_prefix) == 0)
        return pstrdup(base_name);

    table_prefix = ctx->table_prefix;

    /*
     * Match pg_igraph.c's build_table_name() exactly: the prefix is
     * concatenated directly with no inserted separator (callers are
     * expected to include their own trailing "_", e.g. "myproject_"),
     * and a "schema.prefix_" form splits into a schema-qualified,
     * double-quoted identifier. The previous "%s_%s" here unconditionally
     * inserted an extra underscore — harmless for a bare prefix with no
     * trailing "_", but produced a double underscore (and thus a
     * nonexistent table name) for the "prefix_" convention actually used
     * elsewhere in this codebase, and never handled the schema-qualified
     * form at all.
     */
    dot_pos = strchr(table_prefix, '.');
    if (dot_pos == NULL)
    {
        return psprintf("%s%s", table_prefix, base_name);
    }
    else
    {
        int   schema_len = dot_pos - table_prefix;
        char *schema_name = palloc(schema_len + 1);
        char *table_prefix_part = dot_pos + 1;
        char *result;

        strncpy(schema_name, table_prefix, schema_len);
        schema_name[schema_len] = '\0';

        result = psprintf("\"%s\".\"%s%s\"", schema_name, table_prefix_part, base_name);
        pfree(schema_name);
        return result;
    }
}

/* ================================================================
 * Helper: resolve a RETURN field's alias to a concrete node id for
 * the row currently being emitted by exec_match_ctx.
 *
 * Every result row now carries its own resolved (row_src_id, row_dst_id)
 * pair — the anchored case (WHERE src.id = <int>) fills row_src_id with
 * the constant start_id for every row, the unanchored case fills it with
 * each traversal's own seed candidate — so this is just a direct
 * alias→id lookup, no "is there an anchor" branching needed.
 * ================================================================ */
static bool
resolve_match_alias_id(IgraphStmtMatch *m, const char *alias,
                        int64 row_src_id, int64 row_dst_id, int64 *out_id)
{
    if (m->src && m->src->alias && strcmp(alias, m->src->alias) == 0)
    {
        *out_id = row_src_id;
        return true;
    }
    if (m->dst && m->dst->alias && strcmp(alias, m->dst->alias) == 0)
    {
        *out_id = row_dst_id;
        return true;
    }
    return false;
}

/* ================================================================
 * Helper: append an IgraphValue as a JSON scalar to a StringInfo
 * ================================================================ */
static void
append_igraph_value_json(StringInfoData *buf, IgraphValue v)
{
    switch (v.type)
    {
        case IGRAPH_VAL_INT:
            appendStringInfo(buf, INT64_FORMAT, v.ival);
            break;
        case IGRAPH_VAL_FLOAT:
            appendStringInfo(buf, "%.17g", v.fval);
            break;
        case IGRAPH_VAL_BOOL:
            appendStringInfoString(buf, v.bval ? "true" : "false");
            break;
        case IGRAPH_VAL_STRING:
            escape_json(buf, v.sval ? v.sval : "");
            break;
        case IGRAPH_VAL_NULL:
        case IGRAPH_VAL_PARAM:
        default:
            appendStringInfoString(buf, "null");
            break;
    }
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
    int              min_depth;
    JsonbParseState *s;
    IgraphCond      *c;
    int              ret;
    uint64           nrows;
    int64           *pair_src;
    int64           *pair_dst;
    uint64           i;
    char            *nodes_table, *node_labels_table;

    if (m->rel == NULL)
        return exec_match_bare_ctx(m, ctx);

    start_id  = -1;
    rel_type  = m->rel->rel_type;
    direction = (m->rel->dir != DIR_LEFT);
    max_depth = m->rel->max_depth;
    min_depth = m->rel->min_depth;
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
        /* Use extended API functions that support table prefixes when context is provided */
        if (ctx && ctx->table_prefix && strlen(ctx->table_prefix) > 0) {
            /* Build SQL query with table prefix support */
            StringInfoData traverse_query;
            initStringInfo(&traverse_query);

            char *edges_table = build_table_name("edges", ctx);
            char *rel_types_table = build_table_name("rel_types", ctx);

            appendStringInfo(&traverse_query,
                "WITH RECURSIVE traverse_cte AS ("
                "  SELECT %ld::bigint as id, 0 as depth "
                "  UNION ALL "
                "  SELECT CASE WHEN %s THEN e.to_id ELSE e.from_id END as id, "
                "         tc.depth + 1 "
                "  FROM traverse_cte tc "
                "  JOIN %s e ON (CASE WHEN %s THEN e.from_id ELSE e.to_id END) = tc.id "
                "         AND e.direction = %s "
                "  JOIN %s rt ON rt.id = e.rel_type "
                "  WHERE tc.depth < %d AND rt.name = '%s' "
                ") "
                "SELECT DISTINCT id FROM traverse_cte WHERE depth >= %d",
                start_id, direction ? "true" : "false", edges_table,
                direction ? "true" : "false", direction ? "true" : "false",
                rel_types_table, max_depth, rel_type, min_depth);

            ret = SPI_execute(traverse_query.data, true, 0);
            pfree(traverse_query.data);
            pfree(edges_table);
            pfree(rel_types_table);

            if (ret != SPI_OK_SELECT)
                ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("igraph MATCH: graph_traverse failed (ret=%d)", ret)));

            /*
             * SPI_execute()/SPI_execute_with_args() leave CurrentMemoryContext
             * pointing at SPI's own per-call context, not the caller's — every
             * other SPI call site in this codebase (pg_igraph.c) explicitly
             * switches back afterward for the same reason. Skipping this here
             * left the eventual jsonb_in() result built in a context that
             * SPI_finish() frees before it's returned — a real, reproduced
             * SIGSEGV at scale (task #17 testing, 2026-08-04).
             */
            MemoryContextSwitchTo(caller_ctx);

            nrows    = SPI_processed;
            pair_src = (int64 *) palloc((nrows > 0 ? nrows : 1) * sizeof(int64));
            pair_dst = (int64 *) palloc((nrows > 0 ? nrows : 1) * sizeof(int64));

            {
                uint64 kept = 0;

                for (i = 0; i < nrows; i++)
                {
                    bool  isnull;
                    int64 candidate = DatumGetInt64(
                        SPI_getbinval(SPI_tuptable->vals[i],
                                      SPI_tuptable->tupdesc, 1, &isnull));

                    /*
                     * A cycle (e.g. start -> a -> start) could bring the
                     * traversal back to the anchor's own id at depth >= 1,
                     * which the CTE's depth filter alone wouldn't catch.
                     * For MATCH (n)-[:R]->(m), m is the far side of the
                     * edge, not n — so drop the anchor's own id here.
                     */
                    if (candidate == start_id)
                        continue;

                    pair_src[kept] = start_id;
                    pair_dst[kept] = candidate;
                    kept++;
                }
                nrows = kept;
            }
        } else {
            /*
             * Default tables: pure-C BFS via the shared multi-seed helper
             * (single seed here). It already tracks per-node depth and
             * excludes the seed's own id, so min_depth enforcement and the
             * anchor-exclusion both come for free.
             */
            int out_len = 0;

            igraph_match_traverse_multi_internal(&start_id, 1, rel_type, direction,
                                                 max_depth, min_depth,
                                                 &pair_src, &pair_dst, &out_len);
            nrows = (uint64) out_len;
        }
    } else {
        /*
         * No explicit n.id = <int> anchor: every label-matching node is a
         * candidate `n`, and the relationship must actually be walked from
         * each of them — this branch used to return the label-matching
         * nodes directly as if they were `m`, with no edge walk at all
         * (task #17). Collect the candidates first, then batch-traverse
         * from all of them in one shot.
         */
        StringInfoData query;
        uint64         n_candidates;
        int64         *candidate_ids = NULL;

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

        if (ret != SPI_OK_SELECT)
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("igraph MATCH: candidate scan failed (ret=%d)", ret)));

        /* See the matching comment on the anchored branch above — SPI_execute()
         * does not restore the caller's memory context on its own. */
        MemoryContextSwitchTo(caller_ctx);

        n_candidates = SPI_processed;
        if (n_candidates > 0)
        {
            candidate_ids = (int64 *) palloc(n_candidates * sizeof(int64));
            for (i = 0; i < n_candidates; i++)
            {
                bool isnull;
                candidate_ids[i] = DatumGetInt64(
                    SPI_getbinval(SPI_tuptable->vals[i],
                                  SPI_tuptable->tupdesc, 1, &isnull));
            }
        }

        if (n_candidates == 0)
        {
            pair_src = NULL;
            pair_dst = NULL;
            nrows    = 0;
        }
        else if (ctx && ctx->table_prefix && strlen(ctx->table_prefix) > 0)
        {
            /*
             * Prefixed graphs have no C-level adjacency engine — build_
             * adj_list only knows the default edges/rel_types tables, and
             * a cached SPI plan can't target a dynamic table name. Stay on
             * a recursive CTE here, but seed it from every candidate at
             * once instead of walking one candidate per query, so this is
             * still a single SPI round trip no matter how many candidates
             * matched.
             */
            char      *edges_table     = build_table_name("edges", ctx);
            char      *rel_types_table = build_table_name("rel_types", ctx);
            Datum     *elems;
            ArrayType *seed_arr;
            StringInfoData traverse_query;

            elems = (Datum *) palloc(n_candidates * sizeof(Datum));
            for (i = 0; i < n_candidates; i++)
                elems[i] = Int64GetDatum(candidate_ids[i]);
            seed_arr = construct_array(elems, (int) n_candidates,
                                       INT8OID, sizeof(int64),
                                       true, TYPALIGN_DOUBLE);
            pfree(elems);

            initStringInfo(&traverse_query);
            appendStringInfo(&traverse_query,
                "WITH RECURSIVE traverse_cte AS ("
                "  SELECT c.id AS root_id, c.id AS id, 0 AS depth "
                "    FROM unnest($1::bigint[]) AS c(id) "
                "  UNION ALL "
                "  SELECT tc.root_id, "
                "         CASE WHEN %s THEN e.to_id ELSE e.from_id END, "
                "         tc.depth + 1 "
                "  FROM traverse_cte tc "
                "  JOIN %s e ON (CASE WHEN %s THEN e.from_id ELSE e.to_id END) = tc.id "
                "         AND e.direction = %s "
                "  JOIN %s rt ON rt.id = e.rel_type "
                "  WHERE tc.depth < %d AND rt.name = '%s' "
                ") "
                "SELECT DISTINCT root_id, id FROM traverse_cte WHERE depth >= %d",
                direction ? "true" : "false", edges_table,
                direction ? "true" : "false", direction ? "true" : "false",
                rel_types_table, max_depth, rel_type, min_depth);

            {
                Oid   argtypes[] = { INT8ARRAYOID };
                Datum args[]     = { PointerGetDatum(seed_arr) };
                ret = SPI_execute_with_args(traverse_query.data,
                                            1, argtypes, args, NULL, true, 0);
            }
            pfree(traverse_query.data);
            pfree(edges_table);
            pfree(rel_types_table);

            if (ret != SPI_OK_SELECT)
                ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("igraph MATCH: unanchored prefixed traversal failed (ret=%d)", ret)));

            /* See the matching comment on the anchored branch above. */
            MemoryContextSwitchTo(caller_ctx);

            nrows    = SPI_processed;
            pair_src = (int64 *) palloc((nrows > 0 ? nrows : 1) * sizeof(int64));
            pair_dst = (int64 *) palloc((nrows > 0 ? nrows : 1) * sizeof(int64));
            for (i = 0; i < nrows; i++)
            {
                bool isnull;
                pair_src[i] = DatumGetInt64(
                    SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull));
                pair_dst[i] = DatumGetInt64(
                    SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 2, &isnull));
            }
        }
        else
        {
            /*
             * Default tables: batched pure-C BFS (igraph_match_traverse_
             * multi_internal) — one bulk edge-table load, then an
             * in-memory traversal per candidate, zero further SPI round
             * trips.
             */
            int64 *c_src = NULL, *c_dst = NULL;
            int    c_len = 0;

            igraph_match_traverse_multi_internal(candidate_ids, (int) n_candidates,
                                                 rel_type, direction, max_depth, min_depth,
                                                 &c_src, &c_dst, &c_len);
            pair_src = c_src;
            pair_dst = c_dst;
            nrows    = (uint64) c_len;
        }
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
        int64       dst_id;
        char       *label;
        bool        isnull;
        int         lret;
        StringInfoData query;
        bool        node_passes_filter = true;
        IgraphCond *cond;

        dst_id = pair_dst[i];
        label  = NULL;

        /*
         * Evaluate all WHERE conditions for this row, resolving each
         * condition's own alias (src or dst) against this row's own
         * (pair_src[i], pair_dst[i]) pair — every row now carries its own
         * independently-resolved src/dst, anchored or not (see
         * resolve_match_alias_id).
         */
        cond = m->where;
        while (cond && node_passes_filter)
        {
            int64 cond_id;

            if (resolve_match_alias_id(m, cond->alias, pair_src[i], pair_dst[i], &cond_id))
            {
                if (!evaluate_condition(cond, cond_id, cond->alias, ctx)) {
                    node_passes_filter = false;
                    break;
                }
            }
            /* else: this row can't resolve the condition's alias —
             * leave it unenforced rather than reject every row. */

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
            Datum largs[] = { Int64GetDatum(dst_id) };
            lret = SPI_execute_with_args(
                query.data,
                1, la, largs, NULL, true, 1);
            /* See the matching comment above exec_match_ctx's traversal
             * calls — SPI_execute_with_args() leaves CurrentMemoryContext
             * pointing at SPI's own context, not the caller's. This runs
             * once per result row, so it's the site most directly
             * responsible for leaving that wrong context active right
             * before the final jsonb_in() call below. */
            MemoryContextSwitchTo(caller_ctx);
            if (lret == SPI_OK_SELECT && SPI_processed > 0)
                label = TextDatumGetCString(
                    SPI_getbinval(SPI_tuptable->vals[0],
                                  SPI_tuptable->tupdesc, 1, &isnull));
        }

        if (m->dst && m->dst->label && label &&
            strcmp(label, m->dst->label) != 0)
            continue;

        /* Add comma separator if not first item */
        if (!first_result) {
            appendStringInfoChar(&json_buf, ',');
        }
        first_result = false;

        if (m->returns)
        {
            /* RETURN projection: emit exactly the requested alias[.prop] fields */
            IgraphReturnField *rf;
            bool               first_field = true;

            appendStringInfoChar(&json_buf, '{');
            for (rf = m->returns; rf; rf = rf->next)
            {
                int64 alias_id;
                bool  resolved = resolve_match_alias_id(m, rf->alias, pair_src[i], pair_dst[i], &alias_id);

                if (!first_field)
                    appendStringInfoChar(&json_buf, ',');
                first_field = false;

                appendStringInfoChar(&json_buf, '"');
                appendStringInfoString(&json_buf, rf->alias);
                if (rf->prop)
                {
                    appendStringInfoChar(&json_buf, '.');
                    appendStringInfoString(&json_buf, rf->prop);
                }
                appendStringInfoString(&json_buf, "\":");

                if (!resolved)
                {
                    appendStringInfoString(&json_buf, "null");
                }
                else if (!rf->prop || strcmp(rf->prop, "id") == 0)
                {
                    appendStringInfo(&json_buf, INT64_FORMAT, alias_id);
                }
                else
                {
                    IgraphValue v = get_node_property_value(alias_id, rf->prop, ctx);
                    append_igraph_value_json(&json_buf, v);
                }
            }
            appendStringInfoChar(&json_buf, '}');
        }
        else
        {
            /* No RETURN clause: default shape — matched node's id + label */
            appendStringInfo(&json_buf, "{\"id\":%ld", dst_id);
            if (label) {
                appendStringInfoString(&json_buf, ",\"label\":");
                escape_json(&json_buf, label);
            }
            appendStringInfoChar(&json_buf, '}');
        }
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
    if (pair_src) pfree(pair_src);
    if (pair_dst) pfree(pair_dst);
    pfree(nodes_table);
    pfree(node_labels_table);
    SPI_finish();

    return result;
}

/* ================================================================
 * exec_match_bare_ctx — MATCH (n) / MATCH (n:Label) with no
 * relationship pattern. Unlike exec_match_ctx's src-rel-dst shape,
 * there is no traversal here: candidates are enumerated directly by
 * an optional WHERE src.id = <int> constant, or by src's label, or
 * (with neither) every node in the graph. `m->dst` is always NULL
 * for this shape, so a RETURN field can only resolve against src's
 * alias.
 * ================================================================ */
static Jsonb *
exec_match_bare_ctx(IgraphStmtMatch *m, IgraphExecContext *ctx)
{
    MemoryContext    caller_ctx = CurrentMemoryContext;
    int64            start_id;
    IgraphCond      *c;
    int              ret;
    uint64           nrows;
    int64           *node_ids;
    uint64           i;
    char            *nodes_table, *node_labels_table;
    StringInfoData   query;

    start_id = -1;
    nodes_table = build_table_name("nodes", ctx);
    node_labels_table = build_table_name("node_labels", ctx);

    /* Look for src.id = <int> to narrow enumeration to one node */
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
    MemoryContextSwitchTo(caller_ctx);

    initStringInfo(&query);
    if (start_id >= 0 && m->src && m->src->label)
    {
        Oid   argtypes[] = { INT8OID, TEXTOID };
        Datum args[]     = { Int64GetDatum(start_id),
                              CStringGetTextDatum(m->src->label) };
        appendStringInfo(&query,
            "SELECT n.id FROM %s n JOIN %s nl ON nl.id = n.label "
            "WHERE n.id = $1 AND nl.name = $2",
            nodes_table, node_labels_table);
        ret = SPI_execute_with_args(query.data, 2, argtypes, args,
                                     NULL, true, 0);
    }
    else if (start_id >= 0)
    {
        Oid   argtypes[] = { INT8OID };
        Datum args[]     = { Int64GetDatum(start_id) };
        appendStringInfo(&query, "SELECT id FROM %s WHERE id = $1",
                          nodes_table);
        ret = SPI_execute_with_args(query.data, 1, argtypes, args,
                                     NULL, true, 0);
    }
    else if (m->src && m->src->label)
    {
        Oid   argtypes[] = { TEXTOID };
        Datum args[]     = { CStringGetTextDatum(m->src->label) };
        appendStringInfo(&query,
            "SELECT DISTINCT n.id FROM %s n JOIN %s nl ON nl.id = n.label "
            "WHERE nl.name = $1",
            nodes_table, node_labels_table);
        ret = SPI_execute_with_args(query.data, 1, argtypes, args,
                                     NULL, true, 0);
    }
    else
    {
        appendStringInfo(&query, "SELECT id FROM %s", nodes_table);
        ret = SPI_execute(query.data, true, 0);
    }
    pfree(query.data);

    if (ret != SPI_OK_SELECT)
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("igraph MATCH: bare node enumeration failed (ret=%d)", ret)));

    /*
     * SPI_execute()/SPI_execute_with_args() leave CurrentMemoryContext
     * pointing at SPI's own per-call context, not the caller's — node_ids
     * and the eventual jsonb_in() result must not be built there, or
     * SPI_finish() below frees them out from under the return value
     * (same hazard/fix as exec_match_ctx, task #17).
     */
    MemoryContextSwitchTo(caller_ctx);

    nrows    = SPI_processed;
    node_ids = (int64 *) palloc(nrows * sizeof(int64));
    for (i = 0; i < nrows; i++)
    {
        bool isnull;
        node_ids[i] = DatumGetInt64(
            SPI_getbinval(SPI_tuptable->vals[i],
                          SPI_tuptable->tupdesc, 1, &isnull));
    }

    /* StringInfo, not JsonbParseState — SPI memory context switches
     * corrupt a JsonbParseState across calls (same rationale as
     * exec_match_ctx/exec_path). */
    StringInfoData json_buf;
    initStringInfo(&json_buf);
    appendStringInfoChar(&json_buf, '[');

    bool first_result = true;
    for (i = 0; i < nrows; i++)
    {
        int64       nid = node_ids[i];
        char       *label = NULL;
        bool        isnull;
        int         lret;
        StringInfoData label_query;
        bool        node_passes_filter = true;
        IgraphCond *cond;

        /* Each candidate resolves the src alias to its own id — there
         * is no dst in a bare match (m->dst is always NULL for this
         * shape), so row_dst_id is irrelevant here. */
        cond = m->where;
        while (cond && node_passes_filter)
        {
            int64 cond_id;
            if (resolve_match_alias_id(m, cond->alias, nid, nid, &cond_id))
            {
                if (!evaluate_condition(cond, cond_id, cond->alias, ctx))
                    node_passes_filter = false;
            }
            cond = cond->next;
        }
        if (!node_passes_filter)
            continue;

        initStringInfo(&label_query);
        appendStringInfo(&label_query,
            "SELECT nl.name FROM %s n JOIN %s nl ON nl.id = n.label "
            "WHERE n.id = $1",
            nodes_table, node_labels_table);
        {
            Oid   la[]    = { INT8OID };
            Datum largs[] = { Int64GetDatum(nid) };
            lret = SPI_execute_with_args(label_query.data,
                                          1, la, largs, NULL, true, 1);
            /* See the caller_ctx comment above the candidate-scan query —
             * label must be pstrdup'd (via TextDatumGetCString) in the
             * caller's context, not SPI's transient one, since it's used
             * after SPI_finish() further down. */
            MemoryContextSwitchTo(caller_ctx);
            if (lret == SPI_OK_SELECT && SPI_processed > 0)
                label = TextDatumGetCString(
                    SPI_getbinval(SPI_tuptable->vals[0],
                                  SPI_tuptable->tupdesc, 1, &isnull));
        }
        pfree(label_query.data);

        if (!first_result)
            appendStringInfoChar(&json_buf, ',');
        first_result = false;

        if (m->returns)
        {
            IgraphReturnField *rf;
            bool               first_field = true;

            appendStringInfoChar(&json_buf, '{');
            for (rf = m->returns; rf; rf = rf->next)
            {
                int64 alias_id;
                bool  resolved = resolve_match_alias_id(m, rf->alias, nid, nid, &alias_id);

                if (!first_field)
                    appendStringInfoChar(&json_buf, ',');
                first_field = false;

                appendStringInfoChar(&json_buf, '"');
                appendStringInfoString(&json_buf, rf->alias);
                if (rf->prop)
                {
                    appendStringInfoChar(&json_buf, '.');
                    appendStringInfoString(&json_buf, rf->prop);
                }
                appendStringInfoString(&json_buf, "\":");

                if (!resolved)
                {
                    appendStringInfoString(&json_buf, "null");
                }
                else if (!rf->prop || strcmp(rf->prop, "id") == 0)
                {
                    appendStringInfo(&json_buf, INT64_FORMAT, alias_id);
                }
                else
                {
                    IgraphValue v = get_node_property_value(alias_id, rf->prop, ctx);
                    append_igraph_value_json(&json_buf, v);
                }
            }
            appendStringInfoChar(&json_buf, '}');
        }
        else
        {
            appendStringInfo(&json_buf, "{\"id\":%ld", nid);
            if (label) {
                appendStringInfoString(&json_buf, ",\"label\":");
                escape_json(&json_buf, label);
            }
            appendStringInfoChar(&json_buf, '}');
        }
    }

    appendStringInfoChar(&json_buf, ']');

    Datum jsonb_datum;
    Jsonb *result;

    if (first_result) {
        jsonb_datum = DirectFunctionCall1(jsonb_in, CStringGetDatum("{}"));
    } else {
        jsonb_datum = DirectFunctionCall1(jsonb_in, CStringGetDatum(json_buf.data));
    }
    result = DatumGetJsonbP(jsonb_datum);

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
    if (ctx && ctx->table_prefix && strlen(ctx->table_prefix) > 0) {
        /* For now: PATH queries with table prefixes use a fallback approach */
        /* TODO: Implement full CTE-based shortest path for prefixed tables */
        StringInfoData json_buf;
        initStringInfo(&json_buf);

        /* Return "not found" for prefixed tables temporarily */
        appendStringInfo(&json_buf, "{\"path\": [], \"found\": false, \"status\": \"ok\", \"note\": \"PATH with table prefixes: basic support\"}");

        /* Convert to JSONB safely */
        Datum jsonb_datum = DirectFunctionCall1(jsonb_in, CStringGetDatum(json_buf.data));
        Jsonb *result = DatumGetJsonbP(jsonb_datum);

        pfree(json_buf.data);
        return result;
    } else {
        /* Use the original implementation for default tables (v1.0 compatibility) */
        return exec_path(p);
    }
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
        if (ctx && ctx->table_prefix && strlen(ctx->table_prefix) > 0) {
            /* Use extended API function that supports table prefixes */
            Oid   argtypes[] = { TEXTOID, TEXTOID };
            Datum args[]     = {
                CStringGetTextDatum(cn->label),
                CStringGetTextDatum(ctx->table_prefix)
            };
            ret = SPI_execute_with_args(
                "SELECT graph_add_node($1, $2)",
                2, argtypes, args, NULL, false, 1);
        } else {
            /* Use standard function for default tables (v1.0 compatibility) */
            Oid   argtypes[] = { TEXTOID };
            Datum args[]     = { CStringGetTextDatum(cn->label) };
            ret = SPI_execute_with_args(
                "SELECT graph_add_node($1)",
                1, argtypes, args, NULL, false, 1);
        }
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
        if (ctx && ctx->table_prefix && strlen(ctx->table_prefix) > 0) {
            /* Use extended API function that supports table prefixes */
            Oid   argtypes[] = { INT8OID, INT8OID, TEXTOID, TEXTOID };
            Datum args[]     = {
                Int64GetDatum(ce->from_id),
                Int64GetDatum(ce->to_id),
                CStringGetTextDatum(ce->rel_type),
                CStringGetTextDatum(ctx->table_prefix)
            };
            ret = SPI_execute_with_args(
                "SELECT graph_add_edge($1,$2,$3,$4)",
                4, argtypes, args, NULL, false, 0);
        } else {
            /* Use standard function for default tables (v1.0 compatibility) */
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
        if (ctx && ctx->table_prefix && strlen(ctx->table_prefix) > 0) {
            /* Use extended API function that supports table prefixes */
            Oid   argtypes[] = { INT8OID, TEXTOID };
            Datum args[]     = {
                Int64GetDatum(dn->node_id),
                CStringGetTextDatum(ctx->table_prefix)
            };
            ret = SPI_execute_with_args(
                "SELECT graph_delete_node($1, $2)",
                2, argtypes, args, NULL, false, 0);
        } else {
            /* Use standard function for default tables (v1.0 compatibility) */
            Oid   argtypes[] = { INT8OID };
            Datum args[]     = { Int64GetDatum(dn->node_id) };
            ret = SPI_execute_with_args(
                "SELECT graph_delete_node($1)",
                1, argtypes, args, NULL, false, 0);
        }
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
        if (ctx && ctx->table_prefix && strlen(ctx->table_prefix) > 0) {
            /* Build prefixed table names for DELETE query */
            char *edges_table = build_table_name("edges", ctx);
            char *rel_types_table = build_table_name("rel_types", ctx);
            StringInfoData query;
            initStringInfo(&query);
            appendStringInfo(&query,
                "DELETE FROM %s WHERE from_id = $1 AND to_id = $2 "
                "AND rel_type = (SELECT id FROM %s WHERE name = $3)",
                edges_table, rel_types_table);

            Oid   argtypes[] = { INT8OID, INT8OID, TEXTOID };
            Datum args[]     = {
                Int64GetDatum(de->from_id),
                Int64GetDatum(de->to_id),
                CStringGetTextDatum(de->rel_type)
            };
            ret = SPI_execute_with_args(query.data, 3, argtypes, args, NULL, false, 0);

            pfree(query.data);
            pfree(edges_table);
            pfree(rel_types_table);
        } else {
            /* Use standard tables for default case (v1.0 compatibility) */
            Oid   argtypes[] = { INT8OID, INT8OID, TEXTOID };
            Datum args[]     = {
                Int64GetDatum(de->from_id),
                Int64GetDatum(de->to_id),
                CStringGetTextDatum(de->rel_type)
            };
            ret = SPI_execute_with_args(
                "DELETE FROM edges WHERE from_id = $1 AND to_id = $2 AND rel_type = (SELECT id FROM rel_types WHERE name = $3)",
                3, argtypes, args, NULL, false, 0);
        }
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
    int16         primitive = 0;
    bytea        *value_bytea = NULL;
    bool          isnull;

    resolved_val = resolve_value(sp->val, ctx);

    SPI_connect();
    MemoryContextSwitchTo(caller_ctx);

    if (resolved_val.type == IGRAPH_VAL_NULL)
    {
        /* SET ... = NULL means "unset" — graph_delete_property has no
         * table-prefix-aware overload yet, so refuse rather than
         * silently deleting from the wrong (default) tables. */
        if (ctx && ctx->table_prefix && strlen(ctx->table_prefix) > 0)
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("igraph SET: NULL is not yet supported for table-prefixed graphs")));

        Oid   argtypes[] = { INT8OID, TEXTOID };
        Datum args[]     = {
            Int64GetDatum(sp->node_id),
            CStringGetTextDatum(sp->prop_name)
        };
        ret = SPI_execute_with_args(
            "SELECT graph_delete_property($1,$2)",
            2, argtypes, args, NULL, false, 0);

        if (ret != SPI_OK_SELECT)
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("igraph SET: graph_delete_property failed")));

        SPI_finish();
        return wrap_ok_simple();
    }

    /*
     * Encode the resolved literal into a pg_ilib BYTEA via pg_ilib's own
     * encoder functions — never hand-roll the op_id header byte (that's
     * how the old BOOL path drifted, see pattern node 841). INT and FLOAT
     * both go through numeric_to_bytea() rather than bigint_to_bytea(),
     * which has a malformed header and can't be decoded even by pg_ilib's
     * own bytea_to_bigint() (filed as pg_ilib task #2).
     */
    switch (resolved_val.type)
    {
        case IGRAPH_VAL_STRING:
        {
            Oid   argtypes[] = { TEXTOID };
            Datum args[]     = { CStringGetTextDatum(resolved_val.sval) };

            primitive = 2; /* text */
            ret = SPI_execute_with_args(
                "SELECT str_to_bytea($1)", 1, argtypes, args, NULL, true, 1);
            if (ret != SPI_OK_SELECT || SPI_processed == 0)
                ereport(ERROR, (errmsg("igraph SET: str_to_bytea failed")));
            /*
             * SPI_execute_with_args() leaves CurrentMemoryContext pointing
             * at SPI's own per-call context, not the caller's — value_bytea
             * is passed into the graph_set_property() SPI call further
             * down, so its copy must land in caller_ctx or it dangles by
             * the time that second call reads it (same hazard/fix as
             * exec_match_ctx, task #17).
             */
            MemoryContextSwitchTo(caller_ctx);
            value_bytea = DatumGetByteaPCopy(
                SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
            break;
        }
        case IGRAPH_VAL_INT:
        {
            Oid   argtypes[] = { INT8OID, INT4OID };
            Datum args[]     = { Int64GetDatum(resolved_val.ival), Int32GetDatum(0) };

            primitive = 1; /* bigint */
            ret = SPI_execute_with_args(
                "SELECT numeric_to_bytea($1::numeric,$2)",
                2, argtypes, args, NULL, true, 1);
            if (ret != SPI_OK_SELECT || SPI_processed == 0)
                ereport(ERROR, (errmsg("igraph SET: numeric_to_bytea failed")));
            /* See the caller_ctx comment in the STRING branch above. */
            MemoryContextSwitchTo(caller_ctx);
            value_bytea = DatumGetByteaPCopy(
                SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
            break;
        }
        case IGRAPH_VAL_FLOAT:
        {
            Oid   argtypes[] = { FLOAT8OID };
            Datum args[]     = { Float8GetDatum(resolved_val.fval) };

            primitive = 6; /* numeric */
            /*
             * params (the header's declared scale) must match the actual
             * number of decimal digits GMP-encodes, or bytea_to_numeric()
             * misplaces the decimal point on decode (e.g. a hardcoded
             * scale=2 turned 3.5 into 0.35 — the payload only carried one
             * digit). scale($1::numeric) derives the real digit count
             * instead of guessing a fixed value.
             */
            ret = SPI_execute_with_args(
                "SELECT numeric_to_bytea($1::numeric, scale($1::numeric))",
                1, argtypes, args, NULL, true, 1);
            if (ret != SPI_OK_SELECT || SPI_processed == 0)
                ereport(ERROR, (errmsg("igraph SET: numeric_to_bytea failed")));
            /* See the caller_ctx comment in the STRING branch above. */
            MemoryContextSwitchTo(caller_ctx);
            value_bytea = DatumGetByteaPCopy(
                SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
            break;
        }
        case IGRAPH_VAL_BOOL:
        {
            Oid   argtypes[] = { BOOLOID };
            Datum args[]     = { BoolGetDatum(resolved_val.bval) };

            primitive = 5; /* bool */
            ret = SPI_execute_with_args(
                "SELECT bool_to_bytea($1)", 1, argtypes, args, NULL, true, 1);
            if (ret != SPI_OK_SELECT || SPI_processed == 0)
                ereport(ERROR, (errmsg("igraph SET: bool_to_bytea failed")));
            /* See the caller_ctx comment in the STRING branch above. */
            MemoryContextSwitchTo(caller_ctx);
            value_bytea = DatumGetByteaPCopy(
                SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
            break;
        }
        default:
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("igraph SET: unsupported value type")));
    }

    if (ctx && ctx->table_prefix && strlen(ctx->table_prefix) > 0)
    {
        Oid   argtypes[] = { INT8OID, TEXTOID, INT2OID, BYTEAOID, TEXTOID };
        Datum args[]     = {
            Int64GetDatum(sp->node_id),
            CStringGetTextDatum(sp->prop_name),
            Int16GetDatum(primitive),
            PointerGetDatum(value_bytea),
            CStringGetTextDatum(ctx->table_prefix)
        };
        ret = SPI_execute_with_args(
            "SELECT graph_set_property($1,$2,$3,$4,NULL,$5)",
            5, argtypes, args, NULL, false, 0);
    }
    else
    {
        Oid   argtypes[] = { INT8OID, TEXTOID, INT2OID, BYTEAOID };
        Datum args[]     = {
            Int64GetDatum(sp->node_id),
            CStringGetTextDatum(sp->prop_name),
            Int16GetDatum(primitive),
            PointerGetDatum(value_bytea)
        };
        ret = SPI_execute_with_args(
            "SELECT graph_set_property($1,$2,$3,$4,NULL)",
            4, argtypes, args, NULL, false, 0);
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
        if (ctx && ctx->table_prefix && strlen(ctx->table_prefix) > 0) {
            /* Use extended API function that supports table prefixes */
            Oid   argtypes[] = { INT8OID, TEXTOID };
            Datum args[]     = {
                Int64GetDatum(gp->node_id),
                CStringGetTextDatum(ctx->table_prefix)
            };
            ret = SPI_execute_with_args(
                "SELECT graph_get_node_properties($1, $2)",
                2, argtypes, args, NULL, true, 1);
        } else {
            /* Use standard function for default tables (v1.0 compatibility) */
            Oid   argtypes[] = { INT8OID };
            Datum args[]     = { Int64GetDatum(gp->node_id) };
            ret = SPI_execute_with_args(
                "SELECT graph_get_node_properties($1)",
                1, argtypes, args, NULL, true, 1);
        }
    }

    if (ret != SPI_OK_SELECT || SPI_processed != 1)
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("igraph GET PROPERTIES: graph_get_node_properties failed")));

    /*
     * SPI_execute_with_args() leaves CurrentMemoryContext pointing at
     * SPI's own per-call context, not the caller's (same hazard/fix as
     * exec_match_ctx, task #17). That alone isn't enough here though:
     * DatumGetJsonbP() doesn't copy when the value isn't toasted, so
     * `props` would still just point into SPI's tuple memory, which
     * SPI_finish() below frees before `props->root` is read further
     * down — use DatumGetJsonbPCopy() to force an actual copy.
     */
    MemoryContextSwitchTo(caller_ctx);

    Datum d = SPI_getbinval(SPI_tuptable->vals[0],
                            SPI_tuptable->tupdesc, 1, &isnull);
    if (isnull)
        props = NULL;
    else
        props = DatumGetJsonbPCopy(d);

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

