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

    start_id  = -1;
    rel_type  = m->rel->rel_type;
    direction = (m->rel->dir != DIR_LEFT);
    max_depth = m->rel->max_depth;
    s         = NULL;

    c = m->where;
    while (c)
    {
        if (m->src->alias &&
            strcmp(c->alias, m->src->alias) == 0 &&
            strcmp(c->prop, "id") == 0 &&
            c->op == COND_EQ &&
            c->val.type == VAL_INT)
        {
            start_id = c->val.ival;
            break;
        }
        c = c->next;
    }

    if (start_id < 0)
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("igraph MATCH: WHERE src.id = <integer> required")));

    SPI_connect();
    MemoryContextSwitchTo(caller_ctx); /* stay in caller ctx */
    {
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

    jb_begin_object(&s);
    jb_key(&s, "status"); jb_str(&s, "ok");
    jb_key(&s, "rows");
    jb_begin_array(&s);

    for (i = 0; i < nrows; i++)
    {
        int64       nid;
        char       *label;
        Jsonb      *props;
        bool        isnull;
        int         lret;
        int         pret;

        nid   = node_ids[i];
        label = NULL;
        props = NULL;

        {
            Oid   la[]    = { INT8OID };
            Datum largs[] = { Int64GetDatum(nid) };
            lret = SPI_execute_with_args(
                "SELECT nl.name FROM nodes n "
                "JOIN node_labels nl ON nl.id = n.label "
                "WHERE n.id = $1",
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
                "SELECT graph_get_node_properties($1)",
                1, pa, pargs, NULL, true, 1);
            if (pret == SPI_OK_SELECT && SPI_processed > 0)
            {
                Datum d = SPI_getbinval(SPI_tuptable->vals[0],
                                        SPI_tuptable->tupdesc, 1, &isnull);
                if (!isnull)
                    props = DatumGetJsonbP(d);
            }
        }

        jb_begin_object(&s);
        jb_key(&s, "id"); jb_int(&s, nid);
        if (label) { jb_key(&s, "label"); jb_str(&s, label); }

        if (m->returns)
        {
            IgraphReturnField *rf = m->returns;
            while (rf)
            {
                if (rf->prop && props)
                {
                    JsonbValue  jkey;
                    JsonbValue *found;
                    jkey.type           = jbvString;
                    jkey.val.string.val = rf->prop;
                    jkey.val.string.len = strlen(rf->prop);
                    found = findJsonbValueFromContainer(
                        &props->root, JB_FOBJECT, &jkey);
                    if (found)
                    {
                        jb_key(&s, rf->prop);
                        pushJsonbValue(&s, WJB_VALUE, found);
                    }
                }
                rf = rf->next;
            }
        }
        else if (props)
        {
            JsonbValue jv;
            jb_key(&s, "properties");
            jv.type            = jbvBinary;
            jv.val.binary.data = &props->root;
            jv.val.binary.len  = VARSIZE(props) - VARHDRSZ;
            pushJsonbValue(&s, WJB_VALUE, &jv);
        }

        jb_end_object(&s);
    }

    jb_end_array(&s);
    pfree(node_ids);
    SPI_finish();
    return jb_finalise(&s);
}

/* ================================================================
 * exec_path
 * ================================================================ */
static Jsonb *
exec_path(IgraphStmtPath *p)
{
    /*
     * Build result as a plain C string then parse with jsonb_in.
     * This completely avoids JsonbParseState which gets corrupted
     * when CurrentMemoryContext switches happen during BFS execution.
     */
    MemoryContext  caller_ctx = CurrentMemoryContext;
    int64         *path       = NULL;
    int            path_len   = 0;
    int16          rel_id;
    int            ret;
    StringInfoData buf;
    int            k;
    Datum          jsonb_datum;
    Jsonb         *result;

    /* ── Phase 1: resolve rel_type → id via SPI ── */
    SPI_connect();
    MemoryContextSwitchTo(caller_ctx);

    {
        Oid   argtypes[] = { TEXTOID };
        Datum args[]     = { CStringGetTextDatum(p->rel_type) };
        ret = SPI_execute_with_args(
            "SELECT id FROM rel_types WHERE name = $1",
            1, argtypes, args, NULL, true, 1);
    }

    if (ret != SPI_OK_SELECT || SPI_processed == 0)
    {
        SPI_finish();
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("igraph PATH: unknown rel_type '%s'", p->rel_type)));
    }

    {
        bool isnull;
        rel_id = DatumGetInt16(
            SPI_getbinval(SPI_tuptable->vals[0],
                          SPI_tuptable->tupdesc, 1, &isnull));
    }

    SPI_finish();
    MemoryContextSwitchTo(caller_ctx);

    /* ── Phase 2: BFS — internal function owns its SPI ── */
    path = igraph_shortest_path_internal(p->start_id, p->end_id,
                                         rel_id, &path_len);

    MemoryContextSwitchTo(caller_ctx);

    /* ── Phase 3: build JSON as plain string, then parse ── */
    initStringInfo(&buf);

    if (path == NULL || path_len == 0)
    {
        appendStringInfoString(&buf, "{\"status\":\"ok\",\"found\":false}");
    }
    else
    {
        appendStringInfoString(&buf, "{\"status\":\"ok\",\"found\":true,\"path\":[");
        for (k = 0; k < path_len; k++)
        {
            if (k > 0)
                appendStringInfoChar(&buf, ',');
            appendStringInfo(&buf, INT64_FORMAT, path[k]);
        }
        appendStringInfo(&buf, "],\"hops\":%d}", path_len - 1);
    }

    /* Parse string → Jsonb safely, no JsonbParseState involved */
    jsonb_datum = DirectFunctionCall1(jsonb_in,
                                      CStringGetDatum(buf.data));
    result = DatumGetJsonbP(jsonb_datum);

    pfree(buf.data);

    return result;
}

/* ================================================================
 * exec_create_node
 * ================================================================ */
static Jsonb *
exec_create_node(IgraphStmtCreateNode *cn)
{
    MemoryContext caller_ctx = CurrentMemoryContext;
    Oid   argtypes[] = { TEXTOID };
    Datum args[]     = { CStringGetTextDatum(cn->label) };
    int   ret;
    bool  isnull;
    int64 node_id;

    SPI_connect();
    MemoryContextSwitchTo(caller_ctx);
    ret = SPI_execute_with_args(
        "SELECT graph_add_node($1)",
        1, argtypes, args, NULL, false, 1);

    if (ret != SPI_OK_SELECT)
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("igraph CREATE NODE: graph_add_node failed")));

    node_id = DatumGetInt64(
        SPI_getbinval(SPI_tuptable->vals[0],
                      SPI_tuptable->tupdesc, 1, &isnull));
    SPI_finish();
    return wrap_node_id(node_id);
}

/* ================================================================
 * exec_create_edge
 * ================================================================ */
static Jsonb *
exec_create_edge(IgraphStmtCreateEdge *ce)
{
    Oid   argtypes[] = { INT8OID, INT8OID, TEXTOID };
    Datum args[]     = {
        Int64GetDatum(ce->from_id),
        Int64GetDatum(ce->to_id),
        CStringGetTextDatum(ce->rel_type)
    };
    int ret;
    MemoryContext caller_ctx = CurrentMemoryContext;

    SPI_connect();
    MemoryContextSwitchTo(caller_ctx);
    ret = SPI_execute_with_args(
        "SELECT graph_add_edge($1,$2,$3)",
        3, argtypes, args, NULL, false, 0);
    SPI_finish();

    if (ret != SPI_OK_SELECT && ret != SPI_OK_UTILITY)
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("igraph CREATE EDGE: graph_add_edge failed")));

    return wrap_ok_simple();
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
        case VAL_STRING:
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

        case VAL_INT:
        case VAL_FLOAT:
        {
            double dval  = (sp->val.type == VAL_INT)
                           ? (double) sp->val.ival : sp->val.fval;
            int    scale = (sp->val.type == VAL_INT) ? 0 : 2;
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

        case VAL_BOOL:
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

        case VAL_NULL:
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
 * igraph_execute — dispatch
 * ================================================================ */
Jsonb *
igraph_execute(IgraphStmt *stmt)
{
    switch (stmt->type)
    {
        case STMT_MATCH:       return exec_match(&stmt->match);
        case STMT_PATH:        return exec_path(&stmt->path);
        case STMT_CREATE_NODE: return exec_create_node(&stmt->create_node);
        case STMT_CREATE_EDGE: return exec_create_edge(&stmt->create_edge);
        case STMT_DELETE_NODE: return exec_delete_node(&stmt->delete_node);
        case STMT_DELETE_EDGE: return exec_delete_edge(&stmt->delete_edge);
        case STMT_SET_PROP:    return exec_set_prop(&stmt->set_prop);
        case STMT_GET_PROPS:   return exec_get_props(&stmt->get_props);
        default:
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("igraph: unknown statement type %d", stmt->type)));
    }
    return NULL;
}
