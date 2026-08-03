#!/bin/sh
# =============================================================================
# Build Script da Extensão PostgreSQL
# Compila a extensão C (controllers + services + utils) e copia para o
# diretório de extensões do embedded-postgres
# (@embedded-postgres/windows-x64/native/lib).
#
# Estrutura:
#   hierarchical_engine_pg.c  — controllers (entry points SQL)
#   src/pg_utils.c            — utils (uuid, paths, wildcards, strings, valores)
#   src/pg_services.c         — services (SPI, flatten/insert, merge, extract, query, CSV)
#   yyjson.c                  — parser JSON
#
# Pré-requisitos:
#   - w64devkit (gcc 15+) no PATH  → https://github.com/skeeto/w64devkit
#   - Binários de desenvolvimento do PG 18.4 extraídos em pg_extension/pgsql
#     (postgresql-18.4-1-windows-x64-binaries.zip da EnterpriseDB)
# =============================================================================
set -e

cd "$(dirname "$0")"

OUTPUT="hierarchical_engine.dll"
PGSQL_DIR="pgsql/pgsql"
DEST="../../node_modules/@embedded-postgres/windows-x64/native/lib"

# ---------------------------------------------------------------
# Detecta gcc (w64devkit)
# ---------------------------------------------------------------
if ! command -v gcc > /dev/null 2>&1; then
    echo "❌ gcc não encontrado no PATH. Instale o w64devkit e adicione ao PATH."
    exit 1
fi

# ---------------------------------------------------------------
# Verifica headers do PG
# ---------------------------------------------------------------
if [ ! -f "$PGSQL_DIR/include/server/postgres.h" ]; then
    echo "❌ Headers do PostgreSQL não encontrados em $PGSQL_DIR/include/server"
    echo "   Extraia postgresql-18.4-1-windows-x64-binaries.zip em pg_extension/pgsql"
    exit 1
fi

echo "🔨 Compilando extensão PostgreSQL..."

gcc -shared -O3 -static-libgcc \
    -I"$PGSQL_DIR/include" \
    -I"$PGSQL_DIR/include/server" \
    -I"$PGSQL_DIR/include/internal" \
    -I. \
    hierarchical_engine_pg.c src/pg_utils.c src/pg_services.c yyjson.c \
    "$PGSQL_DIR/lib/postgres.lib" \
    -o "$OUTPUT"

echo "✅ Extensão compilada: $OUTPUT"

# ---------------------------------------------------------------
# Copia para o diretório de extensões do embedded-postgres
# ---------------------------------------------------------------
if [ -d "$DEST" ]; then
    cp "$OUTPUT" "$DEST/"
    echo "✅ Copiada para $DEST/"
else
    echo "⚠️  Diretório de extensões não encontrado: $DEST"
    echo "   Copie manualmente $OUTPUT para lib/postgresql do seu Postgres."
fi

# ---------------------------------------------------------------
# Resumo
# ---------------------------------------------------------------
echo ""
echo "=================================================="
echo "Para registrar as funções no banco (via Node/psql):"
echo "  CREATE FUNCTION set_json(text, text) RETURNS text"
echo "    AS 'hierarchical_engine', 'set_json' LANGUAGE C STRICT;"
echo "  CREATE FUNCTION extract_json(text) RETURNS text"
echo "    AS 'hierarchical_engine', 'extract_json' LANGUAGE C STRICT;"
echo "  CREATE FUNCTION query_json(text, text) RETURNS text"
echo "    AS 'hierarchical_engine', 'query_json' LANGUAGE C STRICT;"
echo "  CREATE FUNCTION update_json(text, text) RETURNS text"
echo "    AS 'hierarchical_engine', 'update_json' LANGUAGE C STRICT;"
echo "  CREATE FUNCTION export_csv(text) RETURNS text"
echo "    AS 'hierarchical_engine', 'export_csv' LANGUAGE C STRICT;"
echo "  CREATE FUNCTION import_csv(text, text) RETURNS text"
echo "    AS 'hierarchical_engine', 'import_csv' LANGUAGE C STRICT;"
echo "  CREATE FUNCTION ingest_json(text, text) RETURNS text"
echo "    AS 'hierarchical_engine', 'ingest_json' LANGUAGE C STRICT;"
echo "=================================================="
