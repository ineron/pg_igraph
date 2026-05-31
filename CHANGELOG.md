# Changelog

All notable changes to pg_igraph will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.1.0] - 2026-05-31

### Added
- **JSON Parameters Support**: Full support for `&data.field` syntax in WHERE clauses
- **Enhanced WHERE Clauses**: Support for all comparison operators (`=`, `>`, `<`, `>=`, `<=`, `!=`)
- **Flexible Filtering**: No longer limited to `WHERE src.id = X` format
- **Multi-Graph Support**: Table prefix support for multiple graph instances
- **Schema Separation**: Support for `"schema.prefix_"` format
- **Property System Enhancement**: Full table prefix support for property operations

### Changed
- **Clean Response Format**: Removed unnecessary `"status": "ok"` field from responses
- **Response Structure**: Returns pure arrays `[{...}]` for data or empty objects `{}`
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
- 200x+ performance improvement over recursive CTEs
- BFS traversal: 335K nodes in ~227ms
- Shortest path: 10K-node chain in ~49ms

---

**Note**: Detailed development history and technical documentation can be found in the [docs/](docs/) directory.