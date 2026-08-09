#ifndef HE_TYPES_H
#define HE_TYPES_H

/*
 * he_types.h — Constantes e estruturas compartilhadas do motor SQLite.
 *
 * Camada de TIPOS (Clean Architecture): usada por utils, repo, mapper,
 * query, csv, services e controllers. Não contém lógica — apenas
 * definições de domínio, sem dependência de sqlite3 ou yyjson headers
 * (apenas tipos primitivos; yyjson_val* aparece como ponteiro opaco).
 */

#include <stdbool.h>
#include <stddef.h>

/* ===========================================================================
 * TYPE SYSTEM (alinhado com o MDE JavaScript em ivipbase)
 * ===========================================================================
 * TYPE_EMPTY=0, TYPE_OBJECT=1, TYPE_NUMBER=3,
 * TYPE_BOOLEAN=4, TYPE_STRING=5, TYPE_DATETIME=6, TYPE_BIGINT=7,
 * TYPE_BINARY=8, TYPE_REFERENCE=9
 *
 * A arquitetura é OBJETO-ONLY: não existe TYPE_ARRAY (o valor 2 é
 * reservado para dados LEGADOS gravados por versões antigas — leitores
 * tratam type=2 como objeto para não corromper migração). Arrays do JSON
 * de entrada viram objetos com chaves únicas (UUID/push ID, estilo
 * Firebase): {"<uuid>": value, ...} — nunca arrays.
 *
 * Convenção de paths:
 *   - Containers (OBJECT) terminam com '/'  → ex: "/users/100/"
 *   - Primitivos NÃO terminam com '/'               → ex: "/users/100/name"
 *
 * text_value:
 *   - Para containers: JSON com os filhos inline (ex: '{"name":"John","age":30}')
 *   - Para STRING: valor JSON-escapeado (ex: '"John"')
 *   - Para NUMBER: representação textual (ex: "30" ou "3.14")
 *   - Para BOOLEAN: "true" ou "false"
 *   - Para DATETIME/BIGINT/BINARY/REFERENCE: representação textual
 */
#define TYPE_EMPTY 0
#define TYPE_OBJECT 1
/* 2 = LEGADO (antigo TYPE_ARRAY) — NÃO usar em escritas novas */
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
 * Suporta:
 *   "*"          → qualquer segmento (ex: "/users/wc/posts")
 *   "$nome"      → variável nomeada (ex: "/users/$uid/posts")
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
  char key[256];        /* Campo para filtrar (suporta notação aninhada: "address.city") */
  char segments[8][64]; /* Segmentos da key pré-parseados (sem strtok no hot path) */
  int seg_count;
  char op[16];       /* Operador: <, <=, ==, !=, >=, >, like, exists, in, between */
  char compare[512]; /* Valor para comparar (como string) */
  void *compare_val; /* Referência ao valor JSON parseado (yyjson_val*) */
  bool valid;
} QueryFilter;

typedef struct
{
  char key[256];
  char segments[8][64]; /* Segmentos da key pré-parseados (sem strtok no hot path) */
  int seg_count;
  bool ascending;
} QueryOrder;

typedef struct
{
  void *val; /* yyjson_val* */
  size_t index;
} SortEntry;

/* ===========================================================================
 * PROJEÇÃO (include/exclude de campos — caminhos pontilhados)
 * ===========================================================================
 * Caminhos no formato "chave" ou "a.b.c" (paridade com os filtros, que
 * navegam por segmentos com '.'). Exclude SEMPRE vence include.
 *   include vazio → sem whitelist (retorna tudo que não for excluído).
 *   exclude vazio → sem blacklist.
 * Para CSV/import por path, os mesmos slots guardam padrões wildcard
 * de path (segmentos fixos e "*") — o interpretador decide como usar.
 */
#define HE_MAX_PROJ_KEYS 32

typedef struct
{
  char include[HE_MAX_PROJ_KEYS][256];
  int include_count;
  char exclude[HE_MAX_PROJ_KEYS][256];
  int exclude_count;
} HeProjection;

/* ===========================================================================
 * CSV (RFC 4180)
 * ===========================================================================
 */
#define CSV_HEADER "path,type,text_value\n"

typedef struct
{
  char *path;
  int type;
  char *text_value; /* Pode ser NULL */
} CsvRow;

#endif /* HE_TYPES_H */
