#ifndef PG_UTILS_H
#define PG_UTILS_H

/*
 * pg_utils.h — Declarações da camada de UTILS (src/pg_utils.c).
 *
 * Funções puras (sem SPI e sem tabelas): UUID, normalização de paths,
 * wildcards, escape/unescape de strings JSON e conversão de valores
 * yyjson ↔ storage. Nenhuma função aqui faz I/O de banco.
 */

#include "postgres.h"
#include "pg_types.h"
#include "yyjson.h"

/* ===========================================================================
 * UUID
 * ===========================================================================
 */
/* UUID v4 via pg_strong_random (seguro). buf deve ter >= UUID_STR_LEN. */
void generate_uuid_v4(char *buf, size_t buf_size);

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
bool extract_path_segment(const char *path, int seg_idx, char *out, size_t out_size);

/* ===========================================================================
 * STRINGS JSON
 * ===========================================================================
 */
/* Escapa uma string raw para JSON com aspas. Retorna o tamanho necessário
 * (incluindo as 2 aspas e o NUL). Se buf == NULL ou buf_size < need, apenas
 * calcula o tamanho — permite alocação exata sem truncamento. */
size_t json_escape_string(char *buf, size_t buf_size, const char *raw);
/* Aloca (palloc'd) e escapa uma string raw para JSON. Nunca trunca. */
char *json_escape_string_alloc(const char *raw);
/* Desescapa uma string JSON com aspas em buffer palloc'd. Nunca trunca. */
char *json_unescape_string_alloc(const char *quoted);

/* ===========================================================================
 * VALORES (yyjson ↔ storage)
 * ===========================================================================
 */
bool value_fits_inline(yyjson_val *val, size_t max_inline_size);
/* Serializa um primitivo para text_value. Retorna string palloc'd
 * (nunca trunca). Caller deve pfree(). */
char *serialize_primitive_value(yyjson_val *val, int *out_type);
/* Reconstrói um yyjson_mut_val a partir de (type, text_value) armazenados. */
yyjson_mut_val *make_value_from_storage(yyjson_mut_doc *doc,
                                        int type, const char *text_val);

#endif /* PG_UTILS_H */
