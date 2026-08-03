#ifndef PG_TYPES_H
#define PG_TYPES_H

/*
 * pg_types.h — Constantes e estruturas compartilhadas da extensão PostgreSQL.
 *
 * Camada de TIPOS, usada por utils (src/pg_utils.c) e services
 * (src/pg_services.c). Não contém lógica — apenas definições.
 */

#include "postgres.h"
#include "yyjson.h"

/* ===========================================================================
 * TYPE SYSTEM (alinhado com o MDE JavaScript em ivipbase)
 * ===========================================================================
 * TYPE_EMPTY=0, TYPE_OBJECT=1, TYPE_ARRAY=2, TYPE_NUMBER=3,
 * TYPE_BOOLEAN=4, TYPE_STRING=5, TYPE_DATETIME=6, TYPE_BIGINT=7,
 * TYPE_BINARY=8, TYPE_REFERENCE=9
 *
 * Convenção de paths:
 *   - Containers (OBJECT, ARRAY) terminam com '/'  → ex: "/users/100/"
 *   - Primitivos NÃO terminam com '/'               → ex: "/users/100/name"
 *
 * text_value:
 *   - Para containers: JSON com os filhos inline (ex: '{"name":"John","age":30}')
 *   - Para STRING: valor JSON-escapeado (ex: '"John"')
 *   - Para NUMBER: representação textual (ex: "30" ou "3.14")
 *   - Para BOOLEAN: "true" ou "false"
 */
#define TYPE_EMPTY 0
#define TYPE_OBJECT 1
#define TYPE_ARRAY 2
#define TYPE_NUMBER 3
#define TYPE_BOOLEAN 4
#define TYPE_STRING 5
#define TYPE_DATETIME 6
#define TYPE_BIGINT 7
#define TYPE_BINARY 8
#define TYPE_REFERENCE 9

#define DEFAULT_MAX_INLINE_SIZE 0
#define UUID_STR_LEN 37

/* ===========================================================================
 * WILDCARD PATTERN MATCHING (para query/update multi-nível)
 * ===========================================================================
 */
#define MAX_WC_SEGMENTS 64

typedef struct
{
  char segments[MAX_WC_SEGMENTS][256];
  bool is_wildcard[MAX_WC_SEGMENTS];
  int count;
} WildcardPattern;

/* ===========================================================================
 * QUERY: filtros, ordenação e entradas de ordenação
 * ===========================================================================
 */
typedef struct
{
  char key[256];
  char op[16];
  char compare[512];
  yyjson_val *compare_val;
  bool valid;
} QueryFilter;

typedef struct
{
  char key[256];
  bool ascending;
} QueryOrder;

typedef struct
{
  yyjson_val *val;
  size_t index;
} SortEntry;

/* ===========================================================================
 * CSV
 * ===========================================================================
 */
typedef struct
{
  char *path;
  int type;
  char *text_value;
} CsvRow;

#endif /* PG_TYPES_H */
