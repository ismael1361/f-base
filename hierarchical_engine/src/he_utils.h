#ifndef HE_UTILS_H
#define HE_UTILS_H

/*
 * he_utils.h — Declarações da camada de UTILS (src/he_utils.c).
 *
 * Funções puras (sem I/O de banco): UUID, normalização de paths,
 * wildcards, escape/unescape de strings JSON, decisão de inline e
 * conversão yyjson ↔ storage. Nenhuma função aqui executa SQL.
 */

#include <stddef.h>
#include <stdbool.h>
#include "he_types.h"

/* Protótipos opacos (definidos em sqlite3ext.h apenas nos .c) */
typedef struct sqlite3 sqlite3;

/* ===========================================================================
 * UUID
 * ===========================================================================
 */
/* UUID v4 via sqlite3_randomness. buf deve ter >= UUID_STR_LEN. */
void generate_uuid_v4(char *buf, size_t buf_size);

/* ===========================================================================
 * HASH (SHA-256) — dedup de set (escrita idempotente)
 * ===========================================================================
 */
/* Calcula SHA-256 dos n_segments segmentos concatenados (streaming, sem
 * cópia intermediária) e escreve o digest hex (64 chars + NUL) em out_hex,
 * que deve ter >= 65 bytes. n_segments >= 1. */
void sha256_hex_segments(const char **segments, const size_t *lens,
                         int n_segments, char *out_hex);

/* ===========================================================================
 * PATHS
 * ===========================================================================
 */
void normalize_path(const char *input, char *out_path, size_t out_size);
void path_without_trailing_slash(const char *normalized, char *out, size_t out_size);
void make_upper_bound(const char *prefix, char *upper, size_t up_size);

/* ===========================================================================
 * WILDCARDS (* e $var)
 * ===========================================================================
 */
bool path_has_wildcard(const char *path);
void wildcard_parse(const char *path, WildcardPattern *pattern);
bool wildcard_matches(const char *path, const WildcardPattern *pattern);
void wildcard_fixed_prefix(const char *pattern_path, char *out, size_t out_size);

/* ===========================================================================
 * STRINGS JSON
 * ===========================================================================
 */
/* Escapa uma string raw para JSON com aspas — alocação EXATA via
 * sqlite3_malloc (sem limite: o antigo buffer fixo de 1KB truncava
 * strings longas silenciosamente). Caller libera com sqlite3_free. */
char *json_escape_string_alloc(const char *raw);
/* Unescape de string JSON com aspas — alocação EXATA via sqlite3_malloc
 * (o antigo buffer de 2KB também truncava). Caller libera com sqlite3_free. */
char *json_unescape_string_alloc(const char *quoted);

/* ===========================================================================
 * VALORES (yyjson ↔ storage)
 * ===========================================================================
 */
/* Verifica se um valor JSON pode ser armazenado inline no nó pai */
bool value_fits_inline(void *val, size_t max_inline_size);
/* Serializa um primitivo para text_value (preenche *out_type) com
 * alocação DINÂMICA via sqlite3_malloc (sem limite de tamanho — o
 * buffer fixo truncava). Caller libera com sqlite3_free. */
char *serialize_primitive_value_alloc(void *val, int *out_type);
/* Reconstrói um yyjson_mut_val* a partir de (type, text_value) armazenados
 * (type=2 — antigo TYPE_ARRAY — é tratado como objeto para compat de
 * migração de dados legados). */
void *make_value_from_storage(void *doc, int type, const char *text_val);

/* ===========================================================================
 * PROJEÇÃO (include/exclude)
 * ===========================================================================
 */
/* Parseia o JSON de opções `{"include":[...],"exclude":[...]}` (paths
 * pontilhados) para um HeProjection. options_root NULL ou sem os campos →
 * projeção vazia (nenhum filtro). Slots excedentes são ignorados. */
void he_projection_parse(void *options_root, HeProjection *proj);

/* Copia src (yyjson_val*) para um yyjson_mut_val* no doc aplicando a
 * projeção include/exclude (exclude vence include; caminhos pontilhados
 * preservam os ancestrais como containers de passagem). Com projeção vazia
 * é um deep-copy idêntico a yyjson_val_mut_copy (fast path, zero overhead).
 * Objetos que ficam sem nenhuma chave são mantidos como "{}". */
void *he_project_value(void *doc, void *src, const HeProjection *proj);

#endif /* HE_UTILS_H */
