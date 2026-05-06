%{
/*
 * igraph_parser.y
 * Bison parser for the igraph query language.
 *
 * Compile: bison -d -o igraph_parser.c igraph_parser.y
 * The -d flag generates igraph_parser.h with token definitions.
 */

#include "postgres.h"
#include "igraph_query.h"
#include "igraph_parser.h"
#include "igraph_lexer.h"

/* ── Error reporting ─────────────────────────── */
static void yyerror(YYLTYPE *loc, IgraphParseState *state,
                    yyscan_t scanner, const char *msg)
{
    ereport(ERROR,
        (errcode(ERRCODE_SYNTAX_ERROR),
         errmsg("igraph syntax error: %s", msg),
         errdetail("At column %d in: %s", state->col, state->input)));
}

/* ── AST allocation helpers ─────────────────── */
#define ALLOC(type) ((type *) palloc0(sizeof(type)))

static IgraphStmt *make_stmt(IgraphStmtType t)
{
    IgraphStmt *s = ALLOC(IgraphStmt);
    s->type = t;
    return s;
}

static IgraphNodePat *make_node_pat(char *alias, char *label)
{
    IgraphNodePat *p = ALLOC(IgraphNodePat);
    p->alias = alias;
    p->label = label;
    return p;
}

static IgraphRelPat *make_rel_pat(char *rel, IgraphDir dir, int mn, int mx)
{
    IgraphRelPat *p = ALLOC(IgraphRelPat);
    p->rel_type  = rel;
    p->dir       = dir;
    p->min_depth = mn;
    p->max_depth = mx;
    return p;
}

static IgraphCond *make_cond(char *alias, char *prop,
                              IgraphCondOp op, IgraphValue val)
{
    IgraphCond *c = ALLOC(IgraphCond);
    c->alias = alias;
    c->prop  = prop;
    c->op    = op;
    c->val   = val;
    c->next  = NULL;
    return c;
}

static IgraphReturnField *make_return_field(char *alias, char *prop)
{
    IgraphReturnField *f = ALLOC(IgraphReturnField);
    f->alias = alias;
    f->prop  = prop;
    f->next  = NULL;
    return f;
}

/* Append to linked list — returns head */
static IgraphCond *cond_append(IgraphCond *head, IgraphCond *c)
{
    IgraphCond *cur;
    if (!head) return c;
    cur = head;
    while (cur->next) cur = cur->next;
    cur->next = c;
    return head;
}

static IgraphReturnField *rf_append(IgraphReturnField *head,
                                     IgraphReturnField *f)
{
    IgraphReturnField *cur;
    if (!head) return f;
    cur = head;
    while (cur->next) cur = cur->next;
    cur->next = f;
    return head;
}

%}

/* ── Parser options ──────────────────────────── */

/*
 * %code requires — copied into igraph_parser.h
 * Makes types available to anyone who includes igraph_parser.h
 */
%code requires {
#include "igraph_query.h"
/* yyscan_t is defined by flex as void* —
 * forward-declare it so igraph_parser.h compiles
 * without including igraph_lexer.h */
#ifndef YY_TYPEDEF_YY_SCANNER_T
#define YY_TYPEDEF_YY_SCANNER_T
typedef void *yyscan_t;
#endif
}
%define api.pure full          /* reentrant */
%define parse.error verbose    /* detailed error messages */
%locations                     /* track source locations */

%parse-param { IgraphParseState *state }
%parse-param { yyscan_t scanner }
%lex-param   { yyscan_t scanner }

/* ── Value types for semantic values ─────────── */
%union {
    int64              ival;
    double             fval;
    char              *sval;
    bool               bval;
    IgraphValue        val;
    IgraphCondOp       op;
    IgraphDir          dir;
    IgraphNodePat     *node_pat;
    IgraphRelPat      *rel_pat;
    IgraphCond        *cond;
    IgraphReturnField *ret_field;
    IgraphStmt        *stmt;
    struct { int min; int max; } depth;
}

/* ── Tokens ──────────────────────────────────── */
%token TK_MATCH TK_WHERE TK_RETURN
%token TK_PATH TK_FROM TK_TO TK_VIA TK_NODES
%token TK_CREATE TK_DELETE TK_NODE TK_EDGE
%token TK_SET TK_GET TK_PROPERTIES
%token TK_AND TK_OR TK_NOT TK_NULL

%token <ival>  TK_INTEGER
%token <fval>  TK_FLOAT
%token <sval>  TK_STRING TK_IDENT
%token <bval>  TK_BOOL

%token TK_LPAREN TK_RPAREN
%token TK_LBRACKET TK_RBRACKET
%token TK_LBRACE TK_RBRACE
%token TK_ARROW_R TK_ARROW_L
%token TK_DASH TK_COLON TK_DOT
%token TK_COMMA TK_STAR TK_DOTDOT
%token TK_EQ TK_NEQ TK_LT TK_LTE TK_GT TK_GTE
%token TK_EOF

/* ── Non-terminal types ──────────────────────── */
%type <stmt>      stmt
%type <stmt>      stmt_match stmt_path
%type <stmt>      stmt_create_node stmt_create_edge
%type <stmt>      stmt_delete_node stmt_delete_edge
%type <stmt>      stmt_set_prop stmt_get_props
%type <node_pat>  node_pattern
%type <rel_pat>   rel_pattern
%type <dir>       rel_dir_open rel_dir_close
%type <depth>     depth_spec
%type <cond>      where_clause cond_list cond_item
%type <ret_field> return_clause return_list return_item
%type <val>       literal
%type <op>        cmp_op
%type <sval>      opt_label opt_alias prop_ref_alias prop_ref_prop
%type <ival>      node_id

%%

/* ================================================================
 * Top-level
 * ================================================================ */
program:
      stmt TK_EOF
        {
            state->result = $1;
            YYACCEPT;
        }
    | stmt
        {
            state->result = $1;
            YYACCEPT;
        }
    ;

stmt:
      stmt_match        { $$ = $1; }
    | stmt_path         { $$ = $1; }
    | stmt_create_node  { $$ = $1; }
    | stmt_create_edge  { $$ = $1; }
    | stmt_delete_node  { $$ = $1; }
    | stmt_delete_edge  { $$ = $1; }
    | stmt_set_prop     { $$ = $1; }
    | stmt_get_props    { $$ = $1; }
    ;

/* ================================================================
 * MATCH (src)-[:REL*min..max]->(dst)
 * WHERE alias.prop OP val
 * RETURN alias.prop, ...
 *
 * Examples:
 *   MATCH (n:Category)-[:PARENT_OF*1..5]->(m:Product)
 *   WHERE n.id = 1
 *   RETURN m.id, m.name
 * ================================================================ */
stmt_match:
    TK_MATCH node_pattern rel_pattern node_pattern
    where_clause
    return_clause
        {
            IgraphStmt *s = make_stmt(STMT_MATCH);
            s->match.src     = $2;
            s->match.rel     = $3;
            s->match.dst     = $4;
            s->match.where   = $5;
            s->match.returns = $6;
            $$ = s;
        }
    ;

/* ── Node pattern: (alias:Label) or (alias) or (:Label) ── */
node_pattern:
    TK_LPAREN opt_alias opt_label TK_RPAREN
        { $$ = make_node_pat($2, $3); }
    ;

opt_alias:
      TK_IDENT          { $$ = $1; }
    | /* empty */       { $$ = NULL; }
    ;

opt_label:
      TK_COLON TK_IDENT { $$ = $2; }
    | /* empty */       { $$ = NULL; }
    ;

/* ── Relationship pattern ─────────────────────────────────
 * Covers:
 *   -[:REL]->          depth 1..1 forward
 *   -[:REL*3..7]->     depth 3..7 forward
 *   -[:REL*5]->        depth 5..5 forward
 *   <-[:REL]-          backward
 * ────────────────────────────────────────────────────────── */
rel_pattern:
    rel_dir_open
    TK_LBRACKET TK_COLON TK_IDENT depth_spec TK_RBRACKET
    rel_dir_close
        {
            /* Determine direction from open/close tokens */
            IgraphDir dir;
            if ($1 == DIR_LEFT)
                dir = DIR_LEFT;
            else
                dir = DIR_RIGHT;
            $$ = make_rel_pat($4, dir, $5.min, $5.max);
        }
    ;

rel_dir_open:
      TK_DASH           { $$ = DIR_RIGHT; }  /* -[  */
    | TK_ARROW_L        { $$ = DIR_LEFT;  }  /* <-[ */
    ;

rel_dir_close:
      TK_DASH           { $$ = DIR_RIGHT; }  /* ]- */
    | TK_ARROW_R        { $$ = DIR_RIGHT; }  /* ]-> */
    ;

/* depth: *1..10  or  *5  or empty (default 1..1) */
depth_spec:
      TK_STAR TK_INTEGER TK_DOTDOT TK_INTEGER
        { $$.min = (int)$2; $$.max = (int)$4; }
    | TK_STAR TK_INTEGER
        { $$.min = (int)$2; $$.max = (int)$2; }
    | TK_STAR
        { $$.min = 1; $$.max = 1000000; }  /* unbounded */
    | /* empty */
        { $$.min = 1; $$.max = 1; }
    ;

/* ── WHERE clause ────────────────────────────── */
where_clause:
      TK_WHERE cond_list  { $$ = $2; }
    | /* empty */         { $$ = NULL; }
    ;

cond_list:
      cond_item                       { $$ = $1; }
    | cond_list TK_AND cond_item      { $$ = cond_append($1, $3); }
    ;

cond_item:
    prop_ref_alias TK_DOT prop_ref_prop cmp_op literal
        { $$ = make_cond($1, $3, $4, $5); }
    ;

prop_ref_alias: TK_IDENT { $$ = $1; } ;
prop_ref_prop:  TK_IDENT { $$ = $1; } ;

cmp_op:
      TK_EQ    { $$ = COND_EQ;  }
    | TK_NEQ   { $$ = COND_NEQ; }
    | TK_LT    { $$ = COND_LT;  }
    | TK_LTE   { $$ = COND_LTE; }
    | TK_GT    { $$ = COND_GT;  }
    | TK_GTE   { $$ = COND_GTE; }
    ;

/* ── RETURN clause ───────────────────────────── */
return_clause:
      TK_RETURN return_list  { $$ = $2; }
    | /* empty */            { $$ = NULL; }
    ;

return_list:
      return_item                     { $$ = $1; }
    | return_list TK_COMMA return_item { $$ = rf_append($1, $3); }
    ;

return_item:
      TK_IDENT TK_DOT TK_IDENT
        { $$ = make_return_field($1, $3); }
    | TK_IDENT
        { $$ = make_return_field($1, NULL); }
    ;

/* ================================================================
 * PATH FROM start TO end VIA rel_type
 * RETURN NODES
 *
 * Example:
 *   PATH FROM 1 TO 99 VIA PARENT_OF
 *   RETURN NODES
 * ================================================================ */
stmt_path:
    TK_PATH TK_FROM node_id TK_TO node_id TK_VIA TK_IDENT
    return_clause
        {
            IgraphStmt *s = make_stmt(STMT_PATH);
            s->path.start_id = $3;
            s->path.end_id   = $5;
            s->path.rel_type = $7;
            $$ = s;
        }
    ;

/* ================================================================
 * CREATE (n:Label)
 *
 * Example:
 *   CREATE (n:Product)
 * ================================================================ */
stmt_create_node:
    TK_CREATE TK_LPAREN opt_alias TK_COLON TK_IDENT TK_RPAREN
        {
            IgraphStmt *s = make_stmt(STMT_CREATE_NODE);
            s->create_node.label      = $5;
            s->create_node.prop_count = 0;
            $$ = s;
        }
    ;

/* ================================================================
 * CREATE (from_id)-[:REL_TYPE]->(to_id)
 *
 * Example:
 *   CREATE (1)-[:PARENT_OF]->(2)
 * ================================================================ */
stmt_create_edge:
    TK_CREATE
    TK_LPAREN node_id TK_RPAREN
    TK_DASH TK_LBRACKET TK_COLON TK_IDENT TK_RBRACKET TK_ARROW_R
    TK_LPAREN node_id TK_RPAREN
        {
            IgraphStmt *s = make_stmt(STMT_CREATE_EDGE);
            s->create_edge.from_id  = $3;
            s->create_edge.to_id    = $12;
            s->create_edge.rel_type = $8;
            $$ = s;
        }
    ;

/* ================================================================
 * DELETE NODE id
 *
 * Example:
 *   DELETE NODE 42
 * ================================================================ */
stmt_delete_node:
    TK_DELETE TK_NODE node_id
        {
            IgraphStmt *s = make_stmt(STMT_DELETE_NODE);
            s->delete_node.node_id = $3;
            $$ = s;
        }
    ;

/* ================================================================
 * DELETE EDGE FROM id TO id VIA rel_type
 *
 * Example:
 *   DELETE EDGE FROM 1 TO 2 VIA PARENT_OF
 * ================================================================ */
stmt_delete_edge:
    TK_DELETE TK_EDGE TK_FROM node_id TK_TO node_id TK_VIA TK_IDENT
        {
            IgraphStmt *s = make_stmt(STMT_DELETE_EDGE);
            s->delete_edge.from_id  = $4;
            s->delete_edge.to_id    = $6;
            s->delete_edge.rel_type = $8;
            $$ = s;
        }
    ;

/* ================================================================
 * SET NODE id prop = value
 *
 * Examples:
 *   SET NODE 42 name = 'Galaxy S24'
 *   SET NODE 42 price = 799.99
 * ================================================================ */
stmt_set_prop:
    TK_SET TK_NODE node_id TK_IDENT TK_EQ literal
        {
            IgraphStmt *s = make_stmt(STMT_SET_PROP);
            s->set_prop.node_id   = $3;
            s->set_prop.prop_name = $4;
            s->set_prop.val       = $6;
            $$ = s;
        }
    ;

/* ================================================================
 * GET NODE id PROPERTIES
 *
 * Example:
 *   GET NODE 42 PROPERTIES
 * ================================================================ */
stmt_get_props:
    TK_GET TK_NODE node_id TK_PROPERTIES
        {
            IgraphStmt *s = make_stmt(STMT_GET_PROPS);
            s->get_props.node_id = $3;
            $$ = s;
        }
    ;

/* ================================================================
 * Shared primitives
 * ================================================================ */
node_id:
      TK_INTEGER  { $$ = $1; }
    ;

literal:
      TK_INTEGER
        {
            $$.type = VAL_INT;
            $$.ival = $1;
        }
    | TK_FLOAT
        {
            $$.type = VAL_FLOAT;
            $$.fval = $1;
        }
    | TK_STRING
        {
            $$.type = VAL_STRING;
            $$.sval = $1;
        }
    | TK_BOOL
        {
            $$.type = VAL_BOOL;
            $$.bval = $1;
        }
    | TK_NULL
        {
            $$.type = VAL_NULL;
        }
    ;

%%
