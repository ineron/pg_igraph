#!/bin/bash
# pg_igraph — инициализация схемы хранения
#
# Использование:
#   ./init_graph.sh              — берёт .env из текущей директории
#   ./init_graph.sh --env /path/to/.env
#   ./init_graph.sh --drop       — удалить и пересоздать таблицы
#   ./init_graph.sh --env .env.prod --drop

set -e

# ── Аргументы ────────────────────────────────────────────────────
ENV_FILE=".env"
DROP=false

while [[ $# -gt 0 ]]; do
  case $1 in
    --env)  ENV_FILE="$2"; shift 2 ;;
    --drop) DROP=true;     shift   ;;
    *) echo "Неизвестный параметр: $1"; exit 1 ;;
  esac
done

# ── Загрузка .env ────────────────────────────────────────────────
if [[ ! -f "$ENV_FILE" ]]; then
  echo "Ошибка: файл '$ENV_FILE' не найден"
  echo "Создайте его на основе .env.example:"
  echo "  cp .env.example .env && nano .env"
  exit 1
fi

# Загружаем только KEY=VALUE строки, игнорируем комментарии
while IFS='=' read -r key value; do
  # Пропускаем пустые строки и комментарии
  [[ -z "$key" || "$key" =~ ^[[:space:]]*# ]] && continue
  # Убираем trailing пробелы (но НЕ трогаем # внутри значения)
  value="${value%"${value##*[![:space:]]}"}"
  export "$key=$value"
done < <(grep -E '^[A-Z_]+=' "$ENV_FILE")

# ── Дефолты ──────────────────────────────────────────────────────
PG_HOST="${PG_HOST:-localhost}"
PG_PORT="${PG_PORT:-5432}"
PG_USER="${PG_USER:-postgres}"
PG_DB="${PG_DB:?Ошибка: PG_DB не задан в $ENV_FILE}"
PG_SCHEMA="${PG_SCHEMA:-public}"
GRAPH_PARTITIONS="${GRAPH_PARTITIONS:-16}"

# ── Валидация ────────────────────────────────────────────────────
if [[ "$GRAPH_PARTITIONS" != "8"  && "$GRAPH_PARTITIONS" != "16" && \
      "$GRAPH_PARTITIONS" != "32" && "$GRAPH_PARTITIONS" != "64" ]]; then
  echo "Ошибка: GRAPH_PARTITIONS должен быть 8, 16, 32 или 64"
  exit 1
fi

# ── Пароль ───────────────────────────────────────────────────────
if [[ -n "$PG_PASSWORD" ]]; then
  export PGPASSWORD="$PG_PASSWORD"
fi

PSQL="psql -h $PG_HOST -p $PG_PORT -U $PG_USER -d $PG_DB"

# ── Информация ───────────────────────────────────────────────────
EDGES_PER_PART=$((500000000 / GRAPH_PARTITIONS))
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  pg_igraph — инициализация схемы"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  Конфиг:       $ENV_FILE"
echo "  База:         $PG_DB"
echo "  Схема:        $PG_SCHEMA"
echo "  Партиции:     $GRAPH_PARTITIONS (~${EDGES_PER_PART} рёбер/партиция)"
echo "  Хост:         $PG_HOST:$PG_PORT"
echo "  Пользователь: $PG_USER"
echo "  Drop:         $DROP"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# ── Проверка соединения ──────────────────────────────────────────
if ! $PSQL -c "SELECT 1" > /dev/null 2>&1; then
  echo "Ошибка: не удаётся подключиться к базе '$PG_DB'"
  echo "Проверьте параметры соединения в $ENV_FILE"
  exit 1
fi
echo "✓ Соединение установлено"
echo ""

# ── Основной SQL ─────────────────────────────────────────────────
$PSQL <<EOF

SET search_path = $PG_SCHEMA;

-- ============================================================
-- DROP (если --drop)
-- ============================================================
$(if $DROP; then cat <<DROPSQL
DO \$\$
BEGIN
  DROP TABLE IF EXISTS edge_properties    CASCADE;
  DROP TABLE IF EXISTS node_properties    CASCADE;
  DROP TABLE IF EXISTS edges              CASCADE;
  DROP TABLE IF EXISTS nodes              CASCADE;
  DROP TABLE IF EXISTS complex_type_fields CASCADE;
  DROP TABLE IF EXISTS complex_types      CASCADE;
  DROP TABLE IF EXISTS property_types     CASCADE;
  DROP TABLE IF EXISTS rel_types          CASCADE;
  DROP TABLE IF EXISTS node_labels        CASCADE;
  DROP TABLE IF EXISTS graph_meta         CASCADE;
  RAISE NOTICE 'Существующие таблицы удалены';
END;
\$\$;
DROPSQL
fi)

-- ============================================================
-- СПРАВОЧНИКИ
-- ============================================================
CREATE TABLE IF NOT EXISTS node_labels (
  id    SMALLINT PRIMARY KEY GENERATED ALWAYS AS IDENTITY,
  name  TEXT NOT NULL UNIQUE
);

CREATE TABLE IF NOT EXISTS rel_types (
  id    SMALLINT PRIMARY KEY GENERATED ALWAYS AS IDENTITY,
  name  TEXT NOT NULL UNIQUE
);

-- primitive: 1=bigint 2=text 3=uuid 4=timestamp 5=bool 6=numeric 7=jsonb
CREATE TABLE IF NOT EXISTS property_types (
  id        SMALLINT PRIMARY KEY GENERATED ALWAYS AS IDENTITY,
  name      TEXT     NOT NULL UNIQUE,
  primitive SMALLINT NOT NULL CHECK (primitive BETWEEN 1 AND 7),
  ref_label SMALLINT REFERENCES node_labels(id)
);

-- ============================================================
-- УЗЛЫ
-- ============================================================
CREATE TABLE IF NOT EXISTS nodes (
  id    BIGINT   PRIMARY KEY GENERATED ALWAYS AS IDENTITY,
  label SMALLINT NOT NULL REFERENCES node_labels(id)
);

CREATE INDEX IF NOT EXISTS idx_nodes_label ON nodes(label);

-- ============================================================
-- СВОЙСТВА УЗЛОВ
-- ============================================================
CREATE TABLE IF NOT EXISTS node_properties (
  node_id BIGINT   NOT NULL REFERENCES nodes(id) ON DELETE CASCADE,
  prop_id SMALLINT NOT NULL REFERENCES property_types(id),
  value   BYTEA    NOT NULL,
  PRIMARY KEY (node_id, prop_id)
);

CREATE INDEX IF NOT EXISTS idx_node_props_node ON node_properties(node_id);

-- ============================================================
-- РЁБРА — партиционированные по HASH(from_id)
-- ============================================================
CREATE TABLE IF NOT EXISTS edges (
  from_id   BIGINT   NOT NULL,
  to_id     BIGINT   NOT NULL,
  rel_type  SMALLINT NOT NULL REFERENCES rel_types(id),
  direction BOOL     NOT NULL DEFAULT TRUE,
  data      BYTEA
) PARTITION BY HASH (from_id);

DO \$\$
DECLARE
  i       INT;
  modulus INT := $GRAPH_PARTITIONS;
  tbl     TEXT;
BEGIN
  FOR i IN 0..modulus-1 LOOP
    tbl := format('edges_p%s', i);
    IF NOT EXISTS (
      SELECT 1 FROM pg_class c
      JOIN pg_namespace n ON n.oid = c.relnamespace
      WHERE c.relname = tbl AND n.nspname = '$PG_SCHEMA'
    ) THEN
      EXECUTE format(
        'CREATE TABLE %I PARTITION OF edges
         FOR VALUES WITH (MODULUS %s, REMAINDER %s)',
        tbl, modulus, i
      );
      EXECUTE format(
        'CREATE INDEX ON %I (from_id, rel_type, to_id) WHERE direction = TRUE',
        tbl
      );
      EXECUTE format(
        'CREATE INDEX ON %I (from_id, rel_type, to_id) WHERE direction = FALSE',
        tbl
      );
      /*
       * Covering index for build_adj_list bulk load.
       * The parent table is HASH-partitioned on from_id, so a query
       * WHERE rel_type=$1 AND direction=$2 cannot use partition pruning
       * and would otherwise full-scan all partitions.
       * This index lets the query do an index-only scan on each partition
       * touching only the rows for the requested rel_type — O(|E_reltype|)
       * instead of O(|E_total|).
       */
      EXECUTE format(
        'CREATE INDEX ON %I (rel_type, direction) INCLUDE (from_id, to_id)',
        tbl
      );
      RAISE NOTICE 'Создана партиция edges: %', tbl;
    ELSE
      RAISE NOTICE 'Партиция уже существует: %', tbl;
    END IF;
  END LOOP;
END;
\$\$;

-- ============================================================
-- СВОЙСТВА РЁБЕР — тоже партиционированные
-- ============================================================
CREATE TABLE IF NOT EXISTS edge_properties (
  from_id   BIGINT   NOT NULL,
  to_id     BIGINT   NOT NULL,
  rel_type  SMALLINT NOT NULL,
  direction BOOL     NOT NULL,
  prop_id   SMALLINT NOT NULL REFERENCES property_types(id),
  value     BYTEA    NOT NULL
) PARTITION BY HASH (from_id);

DO \$\$
DECLARE
  i       INT;
  modulus INT := $GRAPH_PARTITIONS;
  tbl     TEXT;
BEGIN
  FOR i IN 0..modulus-1 LOOP
    tbl := format('edge_properties_p%s', i);
    IF NOT EXISTS (
      SELECT 1 FROM pg_class c
      JOIN pg_namespace n ON n.oid = c.relnamespace
      WHERE c.relname = tbl AND n.nspname = '$PG_SCHEMA'
    ) THEN
      EXECUTE format(
        'CREATE TABLE %I PARTITION OF edge_properties
         FOR VALUES WITH (MODULUS %s, REMAINDER %s)',
        tbl, modulus, i
      );
      EXECUTE format(
        'CREATE INDEX ON %I (from_id, to_id, prop_id)', tbl
      );
      RAISE NOTICE 'Создана партиция edge_properties: %', tbl;
    END IF;
  END LOOP;
END;
\$\$;

-- ============================================================
-- КОМПЛЕКСНЫЕ ТИПЫ
-- ============================================================

-- Реестр комплексных типов (Money, Address, Contact...)
-- op_id=0x0E в BYTEA заголовке означает комплексный тип,
-- params (12 бит) = id этой таблицы (до 4096 типов)
CREATE TABLE IF NOT EXISTS complex_types (
  id    SMALLINT PRIMARY KEY GENERATED ALWAYS AS IDENTITY,
  name  TEXT NOT NULL UNIQUE
);

-- Поля комплексного типа — только имена и порядок.
-- op_id каждого поля хранится в самом BYTEA (самоописывающийся формат),
-- здесь нужны только имена для сборки JSON при чтении.
CREATE TABLE IF NOT EXISTS complex_type_fields (
  type_id  SMALLINT NOT NULL REFERENCES complex_types(id) ON DELETE CASCADE,
  pos SMALLINT NOT NULL CHECK (pos >= 0),  -- порядок в бинарном потоке
  field_name TEXT NOT NULL,                        -- имя поля в итоговом JSON
  PRIMARY KEY (type_id, pos)
);

CREATE INDEX IF NOT EXISTS idx_complex_fields_type
  ON complex_type_fields(type_id, pos);

-- ============================================================
-- МЕТА: параметры развёртывания
-- ============================================================
CREATE TABLE IF NOT EXISTS graph_meta (
  key   TEXT PRIMARY KEY,
  value TEXT NOT NULL
);

INSERT INTO graph_meta(key, value) VALUES
  ('version',    '1.0'),
  ('partitions', '$GRAPH_PARTITIONS'),
  ('schema',     '$PG_SCHEMA'),
  ('created_at', NOW()::TEXT)
ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value;

-- ============================================================
-- РАСШИРЕНИЕ
-- ============================================================
CREATE EXTENSION IF NOT EXISTS pg_igraph;

SELECT '✓ pg_igraph schema initialized successfully' AS status;

EOF

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  Параметры развёртывания:"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
$PSQL -c "SELECT key, value FROM ${PG_SCHEMA}.graph_meta ORDER BY key;"
echo ""
echo "✓ Готово"

unset PGPASSWORD
