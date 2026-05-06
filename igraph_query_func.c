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

/* ================================================================
 * igraph_parse — run flex+bison on the query string
 * Returns a palloc'd IgraphStmt AST or throws on syntax error.
 * ================================================================ */
IgraphStmt *
igraph_parse(const char *query)
{
    IgraphParseState state;
    yyscan_t         scanner;
    YY_BUFFER_STATE  buf;

    memset(&state, 0, sizeof(state));
    state.input  = query;
    state.col    = 0;
    state.result = NULL;

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
 * PG function: igraph_query(TEXT) → JSONB
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
