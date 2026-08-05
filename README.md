# pg_igraph 🚀

**High-Performance Graph Traversal Engine for PostgreSQL**

[![MIT License](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![PostgreSQL 14+](https://img.shields.io/badge/PostgreSQL-14%2B-blue.svg)](https://www.postgresql.org/)
[![Performance](https://img.shields.io/badge/Performance-200x%20faster-green.svg)](#performance)
[![Version](https://img.shields.io/badge/Version-1.1-blue.svg)](#version-11-features)

> Transform your PostgreSQL into a high-performance graph database. No external systems needed.

## 🔥 Performance That Speaks

| Operation | Dataset | pg_igraph | Recursive CTE | **Improvement** |
|-----------|---------|-----------|---------------|-----------------|
| BFS Traversal | 335K nodes | 227ms | 47,000ms | **🚀 207x** |
| Shortest Path | 10K nodes | 49ms | 8,500ms | **🚀 173x** |
| Multi-hop Query | 50K nodes | 156ms | 12,000ms | **🚀 77x** |

## ⚡ Quick Start

```bash
# Install
git clone https://github.com/ineron/pg_igraph
cd pg_igraph && make && sudo make install

# Enable
psql -c "CREATE EXTENSION pg_igraph;"
```

```sql
-- Find friends within 3 degrees
SELECT igraph_query('
  MATCH (u:User)-[:FOLLOWS*1..3]->(friend)
  WHERE u.id = 42
  RETURN friend
');

-- Shortest path between users  
SELECT igraph_query('PATH FROM 100 TO 500 VIA FOLLOWS');

-- 🆕 NEW in v1.1: Multiple graphs with table prefixes
-- Prefixed tables must exist first (one-time setup per graph instance) —
-- either the shell script:
--   ./init_graph.sh --prefix social_network
-- or, 🆕 NEW in v1.2, entirely from SQL (no filesystem/shell access needed):
--   SELECT graph_provision_schema('social_network_');
-- The prefix argument must include the trailing underscore that both
-- appends to every table name they create:
SELECT graph_add_node('User', 'social_network_');

-- 🆕 NEW in v1.1: JSON parameters with enhanced WHERE clauses
-- (default, unprefixed graph — see the table-prefix example above for '<prefix>_' graphs)
SELECT igraph_query('', 
  'MATCH (u:User)-[:FOLLOWS]->(f) 
   WHERE f.influence > &data.threshold AND u.id != &data.exclude_id 
   RETURN f.name',
  '{"data":{"threshold":100, "exclude_id":42}}'
);

-- 🆕 NEW in v1.1: All comparison operators supported
SELECT igraph_query('',
  'MATCH (source:User)-[:follows]->(target:User) 
   WHERE source.id >= &data.min_id AND source.id <= &data.max_id
   RETURN source, target',
  '{"data":{"min_id":1, "max_id":100}}'
);
```

## 🎯 Why pg_igraph?

### ✅ **Native PostgreSQL Integration**
- No external graph databases
- Leverage existing PostgreSQL infrastructure  
- ACID compliance and backup strategies work

### ✅ **Adaptive Performance**
- Automatically switches between SPI and C hash maps
- Smart thresholds based on graph topology
- Zero SQL overhead for large traversals

### ✅ **Rich Query Language**
- Cypher-like syntax integrated into SQL
- Full AST with flex/bison parser
- Support for complex pattern matching

### ✅ **External System Integration** 
- REF system for external object references
- Custom resolver functions for business logic
- Seamless integration with existing applications

## 🆕 Version 1.1 Features

### Enhanced WHERE Clauses
- **JSON Parameters**: `&data.field` syntax for dynamic queries
- **All Comparison Operators**: `=`, `>`, `<`, `>=`, `<=`, `!=`
- **Flexible Filtering**: No longer limited to `WHERE src.id = X`

### Clean API Response
- **Pure Data**: Returns arrays `[{...}]` or empty objects `{}`
- **No Status Fields**: Removed unnecessary `"status": "ok"`
- **Library-Ready**: Perfect for integration into larger systems

### Multi-Graph Support
- **Table Prefixes**: Support multiple graph instances, each with its own set of `<prefix>_nodes`, `<prefix>_edges`, etc. tables created via `./init_graph.sh --prefix <name>`
- **Prefix Format**: Pass the prefix **with its trailing underscore** (e.g. `'social_network_'`) to `graph_add_node`/`igraph_query`/etc. — it's concatenated directly onto table names, not auto-separated
- **Independent Graphs**: Isolated data and operations

## 🆕 Version 1.2 Features

### SQL-Callable Schema Provisioning
- **`graph_provision_schema(table_prefix DEFAULT '', partitions DEFAULT 16)`**: Creates a graph instance's backing tables (nodes, edges, properties, complex types, partitions and indexes) directly from SQL — the same schema `init_graph.sh` builds, but usable from a session with no filesystem/shell/`.env` access (e.g. an application's own portal-role connection).
- **Same prefix contract**: `''` for the bare tables, `'myprefix_'` for prefixed tables in the current schema, or `'myschema.myprefix_'` to provision into (and auto-create) a dedicated schema — identical to what `graph_add_node`/`igraph_query`/etc. already accept.
- **Idempotent**: safe to call again for an existing prefix; existing tables/indexes/partitions are left untouched.

## 🏗️ Architecture Highlights

- **Dual-Direction Storage**: Forward and reverse edges for optimal access
- **Hash Partitioning**: 8-64 partitions for parallel processing
- **Covering Indexes**: Three indexes per partition for maximum performance
- **Adaptive Algorithms**: Phase 1 (SPI) → Phase 2 (C hash maps)

## 🎯 Perfect For

- 🏢 **Social Networks**: Friend recommendations, influence analysis
- 🛒 **E-commerce**: Product recommendations, customer journeys  
- 🏨 **Enterprise**: Org charts, workflow dependencies
- 🌍 **Geographic**: Route optimization, network topology
- 🔐 **Security**: Access control, fraud detection

## 📊 Benchmarks

```bash
# Run comprehensive benchmarks
./benchmark.sh --scale large
```

See [detailed benchmark results](docs/benchmarks.md) for more performance data.

## 🔧 API Reference

### Core Functions
```sql
-- Schema provisioning (creates a graph instance's backing tables from SQL —
-- same prefix contract as the functions below, no shell/filesystem access needed)
graph_provision_schema(table_prefix DEFAULT '', partitions DEFAULT 16) → VOID

-- Graph management
graph_add_node(label, table_prefix DEFAULT '') → BIGINT
graph_add_edge(from_id, to_id, relationship, table_prefix DEFAULT '') → VOID

-- Traversal  
graph_traverse(start, rel, direction, max_depth) → SETOF BIGINT
graph_shortest_path(start, end, rel) → BIGINT[]

-- Properties (primitive: 1=bigint, 2=text, 3=uuid, 4=timestamp, 5=bool,
-- 6=numeric, 7=jsonb — value must be pre-encoded with the matching
-- pg_ilib *_to_bytea() function; ref_label labels the property as a
-- typed reference to another node's label, or pass NULL for a plain value)
graph_set_property(node_id, prop_name, primitive, value, ref_label DEFAULT NULL, table_prefix DEFAULT '') → VOID
graph_get_property(node_id, prop_name, table_prefix DEFAULT '') → BYTEA  -- raw pg_ilib payload; decode via graph_get_node_properties() for readable JSON

-- External integration
graph_resolve_ref(ref_uuid uuid, ref_type text) → JSONB
```

### Query Language
```sql
-- Pattern matching with JSON parameters
MATCH (n:Label)-[:REL*1..5]->(m) 
WHERE n.prop > &data.threshold AND m.id != &data.exclude
RETURN m

-- Path finding (FROM/TO take literal integer node ids, VIA a bare
-- relationship-type identifier — none of the three accept &data.* params)
PATH FROM 100 TO 500 VIA FOLLOWS

-- Node creation with references
CREATE (n:Type REF External = &data.external_uuid)
```

## 🚀 Installation

### Prerequisites
```bash
# Install PostgreSQL development headers
sudo yum install postgresql14-devel

# Install flex and bison for query parser
sudo yum install flex bison

# Install pg_ilib dependency
git clone https://github.com/ineron/pg_ilib
cd pg_ilib && make && sudo make install
```

### Build and Install
```bash
git clone https://github.com/ineron/pg_igraph
cd pg_igraph
make
sudo make install
```

### Database Setup
```sql
-- Enable extensions (order matters!)
CREATE EXTENSION pg_ilib;  -- Required dependency
CREATE EXTENSION pg_igraph;

-- Initialize graph schema
./init_graph.sh
```

## ⚙️ Configuration

### Partitioning Strategy
```bash
# .env configuration
GRAPH_PARTITIONS=16  # Recommended for 10M-100M edges

# Scaling guidelines:
# 8 partitions:  up to 10M edges
# 16 partitions: 10M-100M edges  
# 32 partitions: 100M-500M edges
# 64 partitions: 500M+ edges
```

### Performance Tuning
```conf
# Recommended PostgreSQL settings
shared_buffers = 4GB
effective_cache_size = 12GB
random_page_cost = 1.1  # For SSD storage
work_mem = 256MB
```

## 🌐 External Integration

### REF System
```sql
-- Create nodes with external references
CREATE (order:Order REF User = &data.user_uuid);

-- Resolve references through external systems (2 args — no resolver-function
-- argument; the resolver used is fixed per ref_type, not passed at call time)
SELECT graph_resolve_ref(uuid, 'User');
```

### Custom Resolvers
```sql
-- Custom resolver function for CRM integration
CREATE FUNCTION crm_user_resolver(
    ref_data BYTEA,
    fields TEXT[] DEFAULT NULL
) RETURNS JSONB
AS $$
-- Your business logic here
-- Unpack ref_data, query external systems, return enriched data
$$;
```

## 🤝 Contributing

We welcome contributions! See [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md) for guidelines.

### Development Setup
```bash
git clone https://github.com/ineron/pg_igraph
cd pg_igraph
make clean && make
./install.sh
```

## 📚 Documentation

- [Installation Guide](docs/installation.md)
- [Performance Tuning](docs/performance.md)  
- [Query Language Reference](docs/query-language.md)
- [External Integration Guide](docs/LEDGYX_INTEGRATION.md)
- [API Documentation](docs/api.md)
- [Version History](docs/CHANGELOG_v1.1.md)

## 🏗️ How It Works

### Adaptive Execution Strategy
pg_igraph automatically switches between execution modes:
- **Phase 1**: SPI-based execution for small frontiers
- **Phase 2**: In-memory C hash maps for large-scale traversals
- **Smart Thresholds**: Automatically optimizes based on frontier size (default: 200 nodes)

### Storage Architecture
- **Dual-direction edges**: Both forward and reverse stored explicitly
- **Hash-partitioned tables**: 8/16/32/64 partitions for parallel access
- **Covering Indexes**: Three indexes per partition for optimal patterns

### Why Not Recursive CTEs?
PostgreSQL's recursive executor materializes intermediate results and cannot maintain a visited set across iterations. On a 335K-node tree, recursive CTEs take **47 seconds** vs pg_igraph's **227ms**.

## 📄 License

MIT License - see [LICENSE](LICENSE) file for details.

## 🌟 Support the Project

If pg_igraph helps your project, please:
- ⭐ Star this repository
- 🐛 Report issues and bugs
- 📝 Contribute to documentation
- 💡 Share use cases and success stories

---

**Created with ❤️ by Eugene** | [Email](mailto:ineron.spb@gmail.com) | [Issues](https://github.com/ineron/pg_igraph/issues)