MODULE_big   = pg_igraph
OBJS         = pg_igraph.o igraph_lexer.o igraph_parser.o \
               igraph_exec.o igraph_query_func.o
EXTENSION    = pg_igraph
DATA         = pg_igraph--1.0.sql pg_igraph--1.0--1.1.sql pg_igraph--1.1.sql
PG_CONFIG   ?= pg_config

override with_llvm = no

PG_CPPFLAGS  = -I$(shell $(PG_CONFIG) --includedir-server) -I.

# ── Regression tests (pg_regress via PGXS) ──────
# REGRESS must be set before `include $(PGXS)` — the ifdef REGRESS block
# inside pgxs.mk is evaluated at parse time, not at rule-execution time.
REGRESS = 00_setup 01_node_edge_crud 02_traversal_and_path 03_create_delete_set_ddl \
          04_match_where_return 05_match_prefixed 06_query_params \
          07_known_bugs_and_regressions

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Own dedicated regression DB — never touches the shared graph_test instance,
# and never touches the generic contrib_regression DB other extensions may use.
CONTRIB_TESTDB = pg_igraph_regress
REGRESS_OPTS  += --inputdir=test

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
