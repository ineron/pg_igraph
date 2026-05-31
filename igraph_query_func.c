/*
 * igraph_query_func.c
 * PostgreSQL entry point: igraph_query(TEXT) → JSONB
 *
 * Ties together lexer → parser → executor.
 */

#include "postgres.h"
#include "fmgr.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/jsonb.h"

#include "igraph_query.h"
#include "igraph_parser.h"
#include "igraph_lexer.h"
#include "lib/stringinfo.h"
#include "utils/jsonfuncs.h"
#include <math.h>

/* ================================================================
 * igraph_resolve_param — resolve JSON parameter value from path
 * ================================================================ */
IgraphValue
igraph_resolve_param(const char *param_path, Jsonb *json_params)
{
    IgraphValue result;
    JsonbValue *found = NULL;
    JsonbValue  key_val;
    char       *path_copy, *token, *saveptr;
    JsonbContainer *container;

    memset(&result, 0, sizeof(result));
    result.type = IGRAPH_VAL_NULL;

    if (!json_params || !param_path)
        return result;

    /* Navigate the JSON path (e.g., "data.threshold") */
    path_copy = pstrdup(param_path);
    container = &json_params->root;

    token = strtok_r(path_copy, ".", &saveptr);
    while (token && container)
    {
        key_val.type = jbvString;
        key_val.val.string.val = token;
        key_val.val.string.len = strlen(token);

        found = findJsonbValueFromContainer(container, JB_FOBJECT, &key_val);
        if (!found)
            break;

        if (found->type == jbvBinary)
            container = found->val.binary.data;
        else
            container = NULL; /* Final value reached */

        token = strtok_r(NULL, ".", &saveptr);
    }

    pfree(path_copy);

    if (!found)
        return result;

    /* Convert JsonbValue to IgraphValue */
    switch (found->type)
    {
        case jbvNumeric:
            /* Convert to float first, then check if it's integral */
            result.fval = DatumGetFloat8(DirectFunctionCall1(numeric_float8,
                                                           NumericGetDatum(found->val.numeric)));
            if (result.fval == floor(result.fval))
            {
                result.type = IGRAPH_VAL_INT;
                result.ival = (int64) result.fval;
            }
            else
            {
                result.type = IGRAPH_VAL_FLOAT;
            }
            break;

        case jbvString:
            result.type = IGRAPH_VAL_STRING;
            result.sval = pnstrdup(found->val.string.val, found->val.string.len);
            break;

        case jbvBool:
            result.type = IGRAPH_VAL_BOOL;
            result.bval = found->val.boolean;
            break;

        case jbvNull:
            result.type = IGRAPH_VAL_NULL;
            break;

        default:
            result.type = IGRAPH_VAL_NULL;
            break;
    }

    return result;
}

/* ================================================================
 * igraph_parse_extended — run flex+bison with extended context
 * Returns a palloc'd IgraphStmt AST or throws on syntax error.
 * ================================================================ */
IgraphStmt *
igraph_parse_extended(const char *query, const char *table_prefix, Jsonb *json_params)
{
    IgraphParseState state;
    yyscan_t         scanner;
    YY_BUFFER_STATE  buf;

    memset(&state, 0, sizeof(state));
    state.input        = query;
    state.col          = 0;
    state.result       = NULL;
    state.table_prefix = table_prefix;
    state.json_params  = json_params;

    /* Initialise reentrant scanner */
    if (yylex_init_extra(&state, &scanner) != 0)
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("igraph: failed to initialise lexer")));

    buf = yy_scan_string(query, scanner);
    yy_switch_to_buffer(buf, scanner);

    /* Run parser — throws ereport(ERROR) on syntax error */
    int rc = yyparse(&state, scanner);

    yy_delete_buffer(buf, scanner);
    yylex_destroy(scanner);

    if (rc != 0 || state.result == NULL)
        ereport(ERROR,
            (errcode(ERRCODE_SYNTAX_ERROR),
             errmsg("igraph: parse failed for query: %s", query)));

    return (IgraphStmt *) state.result;
}

/* ================================================================
 * igraph_parse — run flex+bison on the query string (legacy)
 * Returns a palloc'd IgraphStmt AST or throws on syntax error.
 * ================================================================ */
IgraphStmt *
igraph_parse(const char *query)
{
    return igraph_parse_extended(query, NULL, NULL);
}

/* ================================================================
 * igraph_stmt_free — release AST memory
 * ================================================================ */
void
igraph_stmt_free(IgraphStmt *stmt)
{
    if (!stmt) return;

    switch (stmt->type)
    {
        case STMT_MATCH:
            if (stmt->match.src) pfree(stmt->match.src);
            if (stmt->match.rel) pfree(stmt->match.rel);
            if (stmt->match.dst) pfree(stmt->match.dst);
            /* cond list */
            {
                IgraphCond *c = stmt->match.where;
                while (c) {
                    IgraphCond *next = c->next;
                    pfree(c);
                    c = next;
                }
            }
            /* return field list */
            {
                IgraphReturnField *f = stmt->match.returns;
                while (f) {
                    IgraphReturnField *next = f->next;
                    pfree(f);
                    f = next;
                }
            }
            break;
        default:
            break;
    }
    pfree(stmt);
}

/* ================================================================
 * PG function: igraph_query(TEXT) → JSONB (legacy)
 * ================================================================ */
PG_FUNCTION_INFO_V1(igraph_query);
Datum igraph_query(PG_FUNCTION_ARGS)
{
    text       *query_text = PG_GETARG_TEXT_PP(0);
    char       *query_str  = text_to_cstring(query_text);
    IgraphStmt *stmt;
    Jsonb      *result;

    /* Parse */
    stmt = igraph_parse(query_str);

    /*
     * Each exec_* function manages its own SPI connection.
     * We do NOT wrap in an outer SPI here — that would cause nested
     * SPI contexts when graph_shortest_path is called from exec_path,
     * leading to memory context confusion and "could not find block"
     * crashes. Let each executor function own its SPI lifetime.
     */
    result = igraph_execute(stmt);

    igraph_stmt_free(stmt);

    PG_RETURN_POINTER(result);
}

/* ================================================================
 * PG function: igraph_query(table_prefix TEXT, query TEXT, json_params TEXT) → JSONB
 * ================================================================ */
PG_FUNCTION_INFO_V1(igraph_query_extended);
Datum igraph_query_extended(PG_FUNCTION_ARGS)
{
    /* Check for NULL arguments first */
    if (PG_ARGISNULL(0))
    {
        ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                       errmsg("table_prefix cannot be NULL")));
    }
    if (PG_ARGISNULL(1))
    {
        ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                       errmsg("query cannot be NULL")));
    }

    text                 *table_prefix_text = PG_GETARG_TEXT_PP(0);
    text                 *query_text        = PG_GETARG_TEXT_PP(1);
    text                 *json_params_text  = PG_ARGISNULL(2) ? NULL : PG_GETARG_TEXT_PP(2);
    char                 *table_prefix      = text_to_cstring(table_prefix_text);
    char                 *query_str         = text_to_cstring(query_text);
    char                 *json_params_str   = json_params_text ? text_to_cstring(json_params_text) : NULL;

    Jsonb                *json_params       = NULL;
    IgraphStmt           *stmt;
    IgraphExecContext     ctx;
    Jsonb                *result;

    /* Parse JSON parameters if provided */
    if (json_params_str && strlen(json_params_str) > 0)
    {
        Datum jsonb_datum = DirectFunctionCall1(jsonb_in, CStringGetDatum(json_params_str));
        json_params = DatumGetJsonbP(jsonb_datum);
    }

    /* Parse query with extended context */
    stmt = igraph_parse_extended(query_str, table_prefix, json_params);

    /* Setup execution context */
    memset(&ctx, 0, sizeof(ctx));
    ctx.table_prefix = table_prefix;
    ctx.json_params  = json_params;

    /* Execute */
    result = igraph_execute_with_context(stmt, &ctx);

    igraph_stmt_free(stmt);

    PG_RETURN_POINTER(result);
}
