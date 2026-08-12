# Changelog

All notable changes to pg_igraph will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.2.0] - 2026-08-05

### Added
- **SQL-Callable Schema Provisioning**: `graph_provision_schema(table_prefix DEFAULT '', partitions DEFAULT 16)` creates a graph instance's backing tables (nodes, edges, properties, complex types, partitions, indexes) entirely from SQL — the same schema `init_graph.sh` builds, but usable from a session with no filesystem/shell/`.env` access. Accepts the same `table_prefix` contract as `graph_add_node`/`igraph_query`/etc. (`''`, `'myprefix_'`, or `'myschema.myprefix_'`, auto-creating the schema if needed), and is idempotent — safe to call again on an existing prefix.

## [1.1.0] - 2026-05-31

### Added
- **JSON Parameters Support**: Full support for `&data.field` syntax in WHERE clauses
- **Enhanced WHERE Clauses**: Support for all comparison operators (`=`, `>`, `<`, `>=`, `<=`, `!=`)
- **Flexible Filtering**: No longer limited to `WHERE src.id = X` format
- **Multi-Graph Support**: Table prefix support for multiple graph instances
- **Schema Separation**: Support for `"schema.prefix_"` format
- **Property System Enhancement**: Full table prefix support for property operations

### Changed
- **Clean Response Format**: `MATCH`/`RETURN` queries return pure arrays `[{...}]` for data or empty objects `{}`, with no `"status"` field. `CREATE`, `DELETE`, `SET`, and `PATH` statements still include `"status": "ok"` in their response — that field was not removed project-wide, only from row-returning queries.
- **Default Version**: Set v1.1 as default extension version

### Fixed
- **Parameter Resolution**: Fixed type checking for JSON parameters in WHERE clauses
- **Memory Safety**: Resolved JsonbParseState segmentation faults using StringInfo approach
- **Function Ambiguity**: Fixed PostgreSQL function resolution conflicts
- **Installation Process**: Proper library updates with `./install.sh` script

### Performance
- **Zero Server Crashes**: Eliminated all memory corruption issues
- **Production Stable**: Core functionality fully tested and validated
- **Enhanced Debugging**: Established comprehensive debugging methodology

### Technical Details
- Replaced dangerous JsonbParseState with safe StringInfo pattern
- Implemented comprehensive NULL protection patterns
- Added strategic debug message placement for troubleshooting
- Enhanced SPI result processing with proper validation

## [1.0.0] - 2026-05-27

### Added
- Initial release of pg_igraph
- Native PostgreSQL graph traversal engine
- Cypher-like query language with flex/bison parser
- BFS and shortest path algorithms
- REF system for external object integration
- Adaptive execution strategy (SPI → C hash maps)
- Hash-partitioned storage with covering indexes

### Performance
- Up to ~21.5x faster than recursive CTEs on shortest-path queries (gap widens with path length — CTE's array-based visited check is O(n²) in path length, pg_igraph scales close to linearly); ~3.3-3.6x faster on full BFS traversal. Multi-hop traversal on shallow, non-hierarchical graphs is currently slower than a plain CTE — see task tracker.
- BFS traversal: 335,923-node tree in ~230ms (medium scale) / 6.7M-node tree in 5.88s (large scale)
- Shortest path: 10K-node chain in ~590ms (medium scale) / 100K-node chain in 6.5s (large scale)

  *(The figures originally published here — 227ms/49ms and a blanket "200x+" claim — were never measured against real benchmark runs. Corrected 2026-08-11 after a cross-project fact-check; see `benchmark.sh` for the reproducible methodology.)*

---

**Note**: Detailed development history and technical documentation can be found in the [docs/](docs/) directory.