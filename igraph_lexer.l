%{
/*
 * igraph_lexer.l
 * Flex tokenizer for the igraph query language.
 *
 * Tokens are passed to the Bison parser via igraph_parser.y
 * Compile: flex -o igraph_lexer.c igraph_lexer.l
 */

#include "postgres.h"
#include "igraph_query.h"    /* shared types — must come before parser.h */
#include "igraph_parser.h"   /* Bison-generated token definitions */

/* Current parse state — passed through for error reporting */
#define YY_EXTRA_TYPE IgraphParseState *

/* Track line/column for error messages */
#define YY_USER_ACTION \
    yyextra->col += yyleng;

/* Copy matched text into palloc'd string */
static char *lexer_strdup(const char *s) {
    return pstrdup(s);
}

%}

/* ── Flex options ─────────────────────────────── */
/* Options: reentrant, bison-bridge, bison-locations, noyywrap */
/* case-insensitive keywords, nodefault for unmatched warning   */
%option reentrant
%option bison-bridge
%option bison-locations
%option extra-type="IgraphParseState *"
%option noyywrap
%option case-insensitive
%option nodefault

/* ── Character classes ────────────────────────── */
DIGIT       [0-9]
ALPHA       [a-zA-Z_]
ALNUM       [a-zA-Z0-9_]
WS          [ \t\r\n]+
INTEGER     {DIGIT}+
FLOAT       {DIGIT}+\.{DIGIT}+
IDENT       {ALPHA}{ALNUM}*
STRING      '([^'\\]|\\.)*'

%%

 /* ── Whitespace & comments ──────────────────── */
{WS}            { /* skip */ }
"--"[^\n]*      { /* skip line comment */ }

 /* ── Keywords ───────────────────────────────── */
"MATCH"         { return TK_MATCH;      }
"WHERE"         { return TK_WHERE;      }
"RETURN"        { return TK_RETURN;     }
"PATH"          { return TK_PATH;       }
"FROM"          { return TK_FROM;       }
"TO"            { return TK_TO;         }
"VIA"           { return TK_VIA;        }
"NODES"         { return TK_NODES;      }
"CREATE"        { return TK_CREATE;     }
"DELETE"        { return TK_DELETE;     }
"NODE"          { return TK_NODE;       }
"EDGE"          { return TK_EDGE;       }
"SET"           { return TK_SET;        }
"GET"           { return TK_GET;        }
"PROPERTIES"    { return TK_PROPERTIES; }
"AND"           { return TK_AND;        }
"OR"            { return TK_OR;         }
"NOT"           { return TK_NOT;        }
"NULL"          { return TK_NULL;       }
"TRUE"          { yylval->bval = true;  return TK_BOOL; }
"FALSE"         { yylval->bval = false; return TK_BOOL; }

 /* ── Punctuation ─────────────────────────────── */
"("             { return TK_LPAREN;    }
")"             { return TK_RPAREN;    }
"["             { return TK_LBRACKET;  }
"]"             { return TK_RBRACKET;  }
"{"             { return TK_LBRACE;    }
"}"             { return TK_RBRACE;    }
"->"            { return TK_ARROW_R;   }
"<-"            { return TK_ARROW_L;   }
"-"             { return TK_DASH;      }
":"             { return TK_COLON;     }
"."             { return TK_DOT;       }
","             { return TK_COMMA;     }
"*"             { return TK_STAR;      }
".."            { return TK_DOTDOT;    }
"="             { return TK_EQ;        }
"!="            { return TK_NEQ;       }
"<"             { return TK_LT;        }
"<="            { return TK_LTE;       }
">"             { return TK_GT;        }
">="            { return TK_GTE;       }

 /* ── Literals ────────────────────────────────── */
{INTEGER}       {
                    yylval->ival = atoll(yytext);
                    return TK_INTEGER;
                }

{FLOAT}         {
                    yylval->fval = atof(yytext);
                    return TK_FLOAT;
                }

{STRING}        {
                    /* Strip surrounding quotes */
                    char *s = lexer_strdup(yytext + 1);
                    s[strlen(s) - 1] = '\0';
                    yylval->sval = s;
                    return TK_STRING;
                }

 /* ── Identifiers ─────────────────────────────── */
{IDENT}         {
                    yylval->sval = lexer_strdup(yytext);
                    return TK_IDENT;
                }

 /* ── End of input ────────────────────────────── */
<<EOF>>         { return TK_EOF; }

 /* ── Unrecognized character ──────────────────── */
.               {
                    ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR),
                         errmsg("igraph: unexpected character '%s' at column %d",
                                yytext, yyextra->col)));
                }

%%
