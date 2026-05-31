/*
 * igraph_query.h
 * Shared types between the igraph lexer, parser, and executor.
 */

#ifndef IGRAPH_QUERY_H
#define IGRAPH_QUERY_H

#include "postgres.h"
#include "utils/jsonb.h"

/* ================================================================
 * Parse state — threaded through lex/yacc for error reporting
 * ================================================================ */
typedef struct IgraphParseState {
    const char *input;      /* original query string            */
    int         col;        /* current column (for errors)      */
    void       *result;     /* IgraphStmt* set by parser        */
    /* Extended context for table prefixes and JSON parameters */
    const char *table_prefix; /* table prefix for multi-graph support */
    Jsonb      *json_params;  /* parsed JSON parameters */
} IgraphParseState;

/* ================================================================
 * Execution context — passed to executor functions
 * ================================================================ */
typedef struct IgraphExecContext {
    const char *table_prefix; /* table prefix for dynamic table names */
    Jsonb      *json_params;  /* JSON parameters for &data.field substitution */
} IgraphExecContext;

/* ================================================================
 * AST node types
 * ================================================================ */
typedef enum IgraphStmtType {
    STMT_MATCH,             /* MATCH ... WHERE ... RETURN ...   */
    STMT_PATH,              /* PATH FROM x TO y VIA rel         */
    STMT_CREATE_NODE,       /* CREATE (n:Label)                 */
    STMT_CREATE_EDGE,       /* CREATE (x)-[:R]->(y)             */
    STMT_DELETE_NODE,       /* DELETE NODE x                    */
    STMT_DELETE_EDGE,       /* DELETE EDGE FROM x TO y VIA r    */
    STMT_SET_PROP,          /* SET NODE x prop = val            */
    STMT_GET_PROPS,         /* GET NODE x PROPERTIES            */
} IgraphStmtType;

/* Direction of a relationship pattern */
typedef enum IgraphDir {
    DIR_RIGHT,              /* (a)-[:R]->(b)  forward           */
    DIR_LEFT,               /* (a)<-[:R]-(b)  backward          */
    DIR_BOTH,               /* (a)-[:R]-(b)   undirected        */
} IgraphDir;

/* Value types for AST nodes */
typedef enum IgraphValType {
    IGRAPH_VAL_INT, IGRAPH_VAL_FLOAT, IGRAPH_VAL_STRING, IGRAPH_VAL_BOOL, IGRAPH_VAL_NULL, IGRAPH_VAL_PARAM
} IgraphValType;

typedef struct IgraphValue {
    IgraphValType type;
    union {
        int64       ival;
        double      fval;
        char       *sval;
        bool        bval;
        char       *param_path; /* JSON parameter path like "data.threshold" */
    };
} IgraphValue;

/* A single node pattern: (alias:Label) or (alias:Label REF Type = value) */
typedef struct IgraphNodePat {
    char *alias;            /* variable name, e.g. "n"          */
    char *label;            /* node label, e.g. "Product"       */
    char *ref_type;         /* REF target type, e.g. "User"     */
    IgraphValue ref_value;  /* REF value (UUID or param)        */
    bool  has_ref;          /* true if REF specified            */
} IgraphNodePat;

/* A relationship pattern: -[:REL*min..max]-> */
typedef struct IgraphRelPat {
    char       *rel_type;   /* relationship type name           */
    IgraphDir   dir;        /* direction                        */
    int         min_depth;  /* minimum hops (default 1)         */
    int         max_depth;  /* maximum hops (default 1)         */
} IgraphRelPat;

/* A WHERE condition: alias.prop OP value */
typedef enum IgraphCondOp {
    COND_EQ,  COND_NEQ,
    COND_LT,  COND_LTE,
    COND_GT,  COND_GTE,
} IgraphCondOp;

/* Moved above - removed duplicate */

typedef struct IgraphCond {
    char         *alias;    /* variable: "n"                    */
    char         *prop;     /* property: "id" or "name"         */
    IgraphCondOp  op;
    IgraphValue   val;
    struct IgraphCond *next; /* linked list for AND chains      */
} IgraphCond;

/* A single RETURN field: alias.prop or alias.id */
typedef struct IgraphReturnField {
    char *alias;
    char *prop;             /* NULL means return node id        */
    struct IgraphReturnField *next;
} IgraphReturnField;

/* ================================================================
 * Top-level statement AST nodes
 * ================================================================ */

typedef struct IgraphStmtMatch {
    IgraphNodePat    *src;         /* left node pattern          */
    IgraphRelPat     *rel;         /* relationship pattern       */
    IgraphNodePat    *dst;         /* right node pattern         */
    IgraphCond       *where;       /* WHERE conditions (AND list)*/
    IgraphReturnField *returns;    /* RETURN fields              */
} IgraphStmtMatch;

typedef struct IgraphStmtPath {
    int64  start_id;
    int64  end_id;
    char  *rel_type;
} IgraphStmtPath;

typedef struct IgraphStmtCreateNode {
    char        *label;
    /* properties to set immediately — linked list */
    char       **prop_names;
    IgraphValue *prop_vals;
    int          prop_count;
    /* REF support */
    char        *ref_type;      /* REF target label, e.g. "User" */
    IgraphValue  ref_value;     /* REF UUID value */
    bool         has_ref;       /* true if REF specified */
} IgraphStmtCreateNode;

typedef struct IgraphStmtCreateEdge {
    int64  from_id;
    int64  to_id;
    char  *rel_type;
} IgraphStmtCreateEdge;

typedef struct IgraphStmtDeleteNode {
    int64 node_id;
} IgraphStmtDeleteNode;

typedef struct IgraphStmtDeleteEdge {
    int64  from_id;
    int64  to_id;
    char  *rel_type;
} IgraphStmtDeleteEdge;

typedef struct IgraphStmtSetProp {
    int64        node_id;
    char        *prop_name;
    IgraphValue  val;
} IgraphStmtSetProp;

typedef struct IgraphStmtGetProps {
    int64 node_id;
} IgraphStmtGetProps;

/* ================================================================
 * Unified statement wrapper
 * ================================================================ */
typedef struct IgraphStmt {
    IgraphStmtType type;
    union {
        IgraphStmtMatch      match;
        IgraphStmtPath       path;
        IgraphStmtCreateNode create_node;
        IgraphStmtCreateEdge create_edge;
        IgraphStmtDeleteNode delete_node;
        IgraphStmtDeleteEdge delete_edge;
        IgraphStmtSetProp    set_prop;
        IgraphStmtGetProps   get_props;
    };
} IgraphStmt;

/* ================================================================
 * Public API
 * ================================================================ */

/* Parse a query string → IgraphStmt AST */
IgraphStmt *igraph_parse(const char *query);

/* Parse a query string with extended context → IgraphStmt AST */
IgraphStmt *igraph_parse_extended(const char *query, const char *table_prefix, Jsonb *json_params);

/* Execute an AST → JSONB result (legacy, uses default table names) */
Jsonb *igraph_execute(IgraphStmt *stmt);

/* Execute an AST with context → JSONB result */
Jsonb *igraph_execute_with_context(IgraphStmt *stmt, IgraphExecContext *ctx);

/* Free an AST (uses pfree) */
void igraph_stmt_free(IgraphStmt *stmt);

/* Resolve JSON parameter value from path like "data.threshold" */
IgraphValue igraph_resolve_param(const char *param_path, Jsonb *json_params);

#endif /* IGRAPH_QUERY_H */

/* ================================================================
 * Internal BFS API — callable from igraph_exec.c without SPI nesting
 * ================================================================ */

/*
 * igraph_shortest_path_internal — bidirectional BFS without SQL wrapper.
 * Caller must open SPI before calling. Returns palloc'd array of node ids
 * in caller's memory context, or NULL if no path exists.
 * *out_len is set to the number of nodes in the path.
 */
int64 *igraph_shortest_path_internal(int64  start_id,
                                     int64  end_id,
                                     int16  rel_id,
                                     int   *out_len);
