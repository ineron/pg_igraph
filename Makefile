MODULE_big   = pg_igraph
OBJS         = pg_igraph.o igraph_lexer.o igraph_parser.o \
               igraph_exec.o igraph_query_func.o
EXTENSION    = pg_igraph
DATA         = pg_igraph--1.0.sql pg_igraph--1.0--1.1.sql pg_igraph--1.1.sql
PG_CONFIG   ?= pg_config

override with_llvm = no

PG_CPPFLAGS  = -I$(shell $(PG_CONFIG) --includedir-server) -I.

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# ── Generated sources ────────────────────────────
igraph_lexer.c igraph_lexer.h: igraph_lexer.l igraph_parser.h
	flex --header-file=igraph_lexer.h -o igraph_lexer.c igraph_lexer.l

igraph_parser.c igraph_parser.h: igraph_parser.y
	bison -d -o igraph_parser.c igraph_parser.y

# Ensure generated files exist before compiling objects
igraph_lexer.o:  igraph_lexer.c  igraph_query.h igraph_parser.h
igraph_parser.o: igraph_parser.c igraph_query.h
igraph_exec.o:   igraph_exec.c   igraph_query.h igraph_parser.h
igraph_query_func.o: igraph_query_func.c igraph_query.h igraph_parser.h igraph_lexer.h

# ── Convenience targets ──────────────────────────
init:
	./init_graph.sh

init-drop:
	./init_graph.sh --drop

deploy: all install init

clean-generated:
	rm -f igraph_lexer.c igraph_lexer.h igraph_parser.c igraph_parser.h
