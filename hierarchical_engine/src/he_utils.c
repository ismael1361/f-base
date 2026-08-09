/*
 * he_utils.c — Camada de UTILS do motor SQLite (Clean Architecture).
 *
 * Funções puras: UUID, paths, wildcards, escape/unescape JSON,
 * decisão de inline e conversão yyjson ↔ storage.
 * Nenhuma função aqui executa SQL — apenas API sqlite3 utilitária
 * (randomness, mprintf) e yyjson.
 */

#include <sqlite3ext.h>
/* sqlite3_api é definida em he_extension.c (SQLITE_EXTENSION_INIT1/INIT2).
 * As macros do sqlite3ext.h usam esta variável — declarar extern aqui. */
extern const sqlite3_api_routines *sqlite3_api;

#include <yyjson.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "he_types.h"
#include "he_utils.h"

/* ===========================================================================
 * UUID GENERATION (v4)
 * ===========================================================================
 */

/// Gera um UUID v4 string: "f6df933d8d624407aaf96bacdc0ad1fc"
void generate_uuid_v4(char *buf, size_t buf_size)
{
  if (buf_size < UUID_STR_LEN)
  {
    if (buf_size > 0)
      buf[0] = '\0';
    return;
  }
  unsigned char bytes[16];
  sqlite3_randomness(16, bytes);
  // Set version 4 (0100 in bits 12-15)
  bytes[6] = (bytes[6] & 0x0f) | 0x40;
  // Set variant (10xx in bits 6-7 of clock_seq_hi_and_reserved)
  bytes[8] = (bytes[8] & 0x3f) | 0x80;
  snprintf(buf, buf_size,
           "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
           bytes[0], bytes[1], bytes[2], bytes[3],
           bytes[4], bytes[5],
           bytes[6], bytes[7],
           bytes[8], bytes[9],
           bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

/* ===========================================================================
 * SHA-256 (FIPS 180-4) — usado no dedup de set (escrita idempotente)
 * ===========================================================================
 */

typedef struct
{
  uint32_t state[8];
  unsigned char buffer[64];
  uint64_t bitlen;
  unsigned int buflen;
} HeSha256Ctx;

static const uint32_t HE_SHA256_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

#define HE_SHA256_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void he_sha256_transform(HeSha256Ctx *c, const unsigned char data[64])
{
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t)data[i * 4] << 24) | ((uint32_t)data[i * 4 + 1] << 16) |
           ((uint32_t)data[i * 4 + 2] << 8) | (uint32_t)data[i * 4 + 3];
  for (int i = 16; i < 64; i++)
  {
    uint32_t s0 = HE_SHA256_ROTR(w[i - 15], 7) ^ HE_SHA256_ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = HE_SHA256_ROTR(w[i - 2], 17) ^ HE_SHA256_ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
  uint32_t e = c->state[4], f = c->state[5], g = c->state[6], h = c->state[7];
  for (int i = 0; i < 64; i++)
  {
    uint32_t S1 = HE_SHA256_ROTR(e, 6) ^ HE_SHA256_ROTR(e, 11) ^ HE_SHA256_ROTR(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + S1 + ch + HE_SHA256_K[i] + w[i];
    uint32_t S0 = HE_SHA256_ROTR(a, 2) ^ HE_SHA256_ROTR(a, 13) ^ HE_SHA256_ROTR(a, 22);
    uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
    uint32_t t2 = S0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = cc;
    cc = b;
    b = a;
    a = t1 + t2;
  }
  c->state[0] += a;
  c->state[1] += b;
  c->state[2] += cc;
  c->state[3] += d;
  c->state[4] += e;
  c->state[5] += f;
  c->state[6] += g;
  c->state[7] += h;
}

static void he_sha256_init(HeSha256Ctx *c)
{
  c->state[0] = 0x6a09e667u;
  c->state[1] = 0xbb67ae85u;
  c->state[2] = 0x3c6ef372u;
  c->state[3] = 0xa54ff53au;
  c->state[4] = 0x510e527fu;
  c->state[5] = 0x9b05688cu;
  c->state[6] = 0x1f83d9abu;
  c->state[7] = 0x5be0cd19u;
  c->bitlen = 0;
  c->buflen = 0;
}

static void he_sha256_update(HeSha256Ctx *c, const unsigned char *data, size_t len)
{
  for (size_t i = 0; i < len; i++)
  {
    c->buffer[c->buflen++] = data[i];
    c->bitlen += 8;
    if (c->buflen == 64)
    {
      he_sha256_transform(c, c->buffer);
      c->buflen = 0;
    }
  }
}

static void he_sha256_final(HeSha256Ctx *c, unsigned char out[32])
{
  c->buffer[c->buflen++] = 0x80;
  if (c->buflen > 56)
  {
    while (c->buflen < 64)
      c->buffer[c->buflen++] = 0;
    he_sha256_transform(c, c->buffer);
    c->buflen = 0;
  }
  while (c->buflen < 56)
    c->buffer[c->buflen++] = 0;
  for (int i = 0; i < 8; i++)
    c->buffer[56 + i] = (unsigned char)(c->bitlen >> (56 - 8 * i));
  he_sha256_transform(c, c->buffer);
  for (int i = 0; i < 8; i++)
  {
    out[i * 4] = (unsigned char)(c->state[i] >> 24);
    out[i * 4 + 1] = (unsigned char)(c->state[i] >> 16);
    out[i * 4 + 2] = (unsigned char)(c->state[i] >> 8);
    out[i * 4 + 3] = (unsigned char)(c->state[i]);
  }
}

void sha256_hex_segments(const char **segments, const size_t *lens,
                         int n_segments, char *out_hex)
{
  HeSha256Ctx ctx;
  he_sha256_init(&ctx);
  for (int i = 0; i < n_segments; i++)
  {
    if (segments[i] && lens[i] > 0)
      he_sha256_update(&ctx, (const unsigned char *)segments[i], lens[i]);
  }
  unsigned char digest[32];
  he_sha256_final(&ctx, digest);
  static const char hexc[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++)
  {
    out_hex[i * 2] = hexc[digest[i] >> 4];
    out_hex[i * 2 + 1] = hexc[digest[i] & 0x0F];
  }
  out_hex[64] = '\0';
}

/* ===========================================================================
 * PATH NORMALIZATION (para paths concretos, sem wildcard)
 * ===========================================================================
 */

/// Normaliza um path concreto: garante '/' inicial e '/' final.
/// NÃO suporta wildcards — use path_has_wildcard() primeiro.
///
/// Exemplos:
///   "/people"     → "/people/"
///   "people"      → "/people/"
///   "/people/"    → "/people/"
void normalize_path(const char *input, char *out_path, size_t out_size)
{
  if (!input || !*input)
  {
    snprintf(out_path, out_size, "/");
    return;
  }

  size_t len = strlen(input);
  if (len >= out_size)
    len = out_size - 1;

  // Remove trailing slashes
  while (len > 1 && input[len - 1] == '/')
    len--;

  if (len == 0)
  {
    snprintf(out_path, out_size, "/");
    return;
  }

  // Monta path normalizado com '/' inicial e final
  if (input[0] == '/')
  {
    if (input[len - 1] == '/')
      snprintf(out_path, out_size, "%.*s", (int)len, input);
    else
      snprintf(out_path, out_size, "%.*s/", (int)len, input);
  }
  else
  {
    snprintf(out_path, out_size, "/%.*s/", (int)len, input);
  }
}

/// Remove o trailing '/' de um path normalizado.
/// Ex: "/people/" → "/people"
void path_without_trailing_slash(const char *normalized, char *out, size_t out_size)
{
  size_t len = strlen(normalized);
  if (len > 1 && normalized[len - 1] == '/')
    len--;
  if (len >= out_size)
    len = out_size - 1;
  memcpy(out, normalized, len);
  out[len] = '\0';
}

/* ===========================================================================
 * WILDCARD PATTERN MATCHING (para query/update multi-nível)
 * ===========================================================================
 */

/// Verifica se um path contém wildcards (* ou $)
bool path_has_wildcard(const char *path)
{
  if (!path)
    return false;
  return strchr(path, '*') != NULL || strchr(path, '$') != NULL;
}

/// Parseia um padrão de path em segmentos.
/// "/users/*/history/*" → ["users", "*", "history", "*"]
void wildcard_parse(const char *path, WildcardPattern *pattern)
{
  memset(pattern, 0, sizeof(WildcardPattern));
  if (!path || !*path)
    return;

  const char *p = path;
  if (*p == '/')
    p++;

  while (*p && pattern->count < MAX_WC_SEGMENTS)
  {
    const char *end = strchr(p, '/');
    size_t len = end ? (size_t)(end - p) : strlen(p);

    if (len > 0)
    {
      size_t cp = len < 255 ? len : 255;
      memcpy(pattern->segments[pattern->count], p, cp);
      pattern->segments[pattern->count][cp] = '\0';

      // É wildcard se for "*" ou começar com "$"
      pattern->is_wildcard[pattern->count] =
          (cp == 1 && p[0] == '*') || (cp > 0 && p[0] == '$');

      pattern->count++;
    }

    if (!end)
      break;
    p = end + 1;
  }
}

/// Verifica se um path concreto casa com um padrão wildcard.
/// Ex: wildcard_matches("/users/ewout/posts", ["users","*","posts"]) → true
bool wildcard_matches(const char *path, const WildcardPattern *pattern)
{
  if (!path || pattern->count == 0)
    return false;

  const char *p = path;
  if (*p == '/')
    p++;

  int seg_idx = 0;
  while (*p && seg_idx < pattern->count)
  {
    const char *end = strchr(p, '/');
    size_t len = end ? (size_t)(end - p) : strlen(p);

    // Pula segmentos vazios (ex: trailing slash)
    if (len == 0)
    {
      p = end ? end + 1 : p + strlen(p);
      continue;
    }

    if (pattern->is_wildcard[seg_idx])
    {
      // Wildcard casa qualquer segmento
    }
    else
    {
      // Segmento fixo: precisa casar exatamente
      if (strlen(pattern->segments[seg_idx]) != len ||
          strncmp(pattern->segments[seg_idx], p, len) != 0)
      {
        return false;
      }
    }

    seg_idx++;
    p = end ? end + 1 : p + strlen(p);
  }

  // Ignora trailing slash no path
  while (*p == '/')
    p++;

  return seg_idx == pattern->count && *p == '\0';
}

/// Extrai o prefixo fixo (parte antes do primeiro wildcard).
/// Ex: "/users/*/history/*" → "/users/"
/// Sempre retorna com '/' inicial e final.
void wildcard_fixed_prefix(const char *pattern_path, char *out, size_t out_size)
{
  if (!pattern_path || !*pattern_path)
  {
    snprintf(out, out_size, "/");
    return;
  }

  const char *p = pattern_path;
  if (*p == '/')
    p++;

  const char *first_wc = NULL;

  while (*p)
  {
    const char *end = strchr(p, '/');
    size_t len = end ? (size_t)(end - p) : strlen(p);

    if (len > 0)
    {
      if ((len == 1 && *p == '*') || *p == '$')
      {
        // first_wc aponta para o caractere '/' antes do wildcard,
        // ou para o início se não houver '/' anterior
        first_wc = (p > pattern_path) ? p - 1 : pattern_path;
        break;
      }
    }

    if (!end)
      break;
    p = end + 1;
  }

  if (first_wc)
  {
    // Constrói prefixo garantindo '/' inicial e final
    size_t n = (size_t)(first_wc - pattern_path);
    char tmp[1024];
    size_t tmp_len = 0;

    // Garante '/' inicial
    if (n == 0 || pattern_path[0] != '/')
    {
      tmp[0] = '/';
      tmp_len = 1;
    }

    // Copia o prefixo
    if (n > 0)
    {
      if (n >= sizeof(tmp) - tmp_len - 1)
        n = sizeof(tmp) - tmp_len - 2;
      memcpy(tmp + tmp_len, pattern_path, n);
      tmp_len += n;
    }

    // Garante '/' final
    if (tmp_len == 0 || tmp[tmp_len - 1] != '/')
    {
      if (tmp_len < sizeof(tmp) - 1)
        tmp[tmp_len++] = '/';
    }

    tmp[tmp_len] = '\0';
    strncpy(out, tmp, out_size - 1);
    out[out_size - 1] = '\0';
  }
  else
  {
    // Sem wildcards: normaliza
    normalize_path(pattern_path, out, out_size);
  }
}

/// Gera o upper bound para prefix range query a partir do path.
void make_upper_bound(const char *prefix, char *upper, size_t up_size)
{
  strncpy(upper, prefix, up_size - 1);
  upper[up_size - 1] = '\0';
  size_t len = strlen(upper);
  if (len > 0)
    upper[len - 1]++;
}

/* ===========================================================================
 * HELPERS DE STRING JSON
 * ===========================================================================
 */

/// Escapa uma string raw para o formato JSON com aspas: `"raw content"`.
/// Alocação EXATA com sqlite3_malloc (SEM limite de tamanho — o antigo
/// buffer fixo de 1KB TRUNCAVA strings longas silenciosamente: o valor era
/// gravado cortado no meio, sem nenhum erro). Caller libera com sqlite3_free.
char *json_escape_string_alloc(const char *raw)
{
  if (!raw)
    raw = "";

  // Passo 1: conta o tamanho necessário (aspas + escapes)
  size_t need = 2;
  for (const char *p = raw; *p; p++)
  {
    switch (*p)
    {
    case '"':
    case '\\':
    case '\n':
    case '\r':
    case '\t':
      need += 2;
      break;
    default:
      need += 1;
      break;
    }
  }
  char *buf = sqlite3_malloc64(need + 1);
  if (!buf)
    return NULL;

  // Passo 2: preenche (mesmo conjunto de escapes do original)
  char *out = buf;
  *out++ = '"';
  for (const char *p = raw; *p; p++)
  {
    switch (*p)
    {
    case '"':
      *out++ = '\\';
      *out++ = '"';
      break;
    case '\\':
      *out++ = '\\';
      *out++ = '\\';
      break;
    case '\n':
      *out++ = '\\';
      *out++ = 'n';
      break;
    case '\r':
      *out++ = '\\';
      *out++ = 'r';
      break;
    case '\t':
      *out++ = '\\';
      *out++ = 't';
      break;
    default:
      *out++ = *p;
      break;
    }
  }
  *out++ = '"';
  *out = '\0';
  return buf;
}

/// Faz o unescape de uma string JSON com aspas: `"escaped"` → raw.
/// Alocação EXATA com sqlite3_malloc (o antigo buffer fixo de 2KB também
/// truncava). Caller libera com sqlite3_free.
char *json_unescape_string_alloc(const char *quoted)
{
  if (!quoted)
    quoted = "";
  size_t len = strlen(quoted);
  if (len < 2 || quoted[0] != '"' || quoted[len - 1] != '"')
    return sqlite3_mprintf("%s", quoted);
  // Unescaped <= quoted (escapes encolhem) — len+1 é sempre suficiente
  char *buf = sqlite3_malloc64(len + 1);
  if (!buf)
    return NULL;
  size_t i, j;
  for (i = 1, j = 0; i < len - 1; i++)
  {
    if (quoted[i] == '\\' && i + 1 < len - 1)
    {
      i++;
      switch (quoted[i])
      {
      case '"':
        buf[j++] = '"';
        break;
      case '\\':
        buf[j++] = '\\';
        break;
      case 'n':
        buf[j++] = '\n';
        break;
      case 'r':
        buf[j++] = '\r';
        break;
      case 't':
        buf[j++] = '\t';
        break;
      default:
        buf[j++] = '\\';
        buf[j++] = quoted[i];
        break;
      }
    }
    else
    {
      buf[j++] = quoted[i];
    }
  }
  buf[j] = '\0';
  return buf;
}

/* ===========================================================================
 * VALUE FITS INLINE (alinhado com valueFitsInline do MDE JavaScript)
 * ===========================================================================
 */

/// Verifica se um valor JSON pode ser armazenado inline no nó pai
/// em vez de virar um nó filho dedicado.
/// Retorna true para: number, boolean, string curta, objeto/array vazio
bool value_fits_inline(void *v, size_t max_inline_size)
{
  yyjson_val *val = (yyjson_val *)v;
  if (yyjson_is_num(val) || yyjson_is_bool(val))
    return max_inline_size > 0;

  if (yyjson_is_str(val))
  {
    const char *s = yyjson_get_str(val);
    size_t len = s ? strlen(s) : 0;
    return len <= max_inline_size;
  }

  if (yyjson_is_obj(val))
  {
    // Objeto vazio → inline
    return yyjson_obj_size(val) == 0;
  }

  if (yyjson_is_arr(val))
  {
    // Array vazio → inline
    return yyjson_arr_size(val) == 0;
  }

  // null → inline (will be handled as delete)
  if (yyjson_is_null(val))
    return true;

  return false;
}

/* ===========================================================================
 * VALORES (yyjson ↔ storage)
 * ===========================================================================
 */

/// Serializa um valor primitivo yyjson como texto para text_value.
/// Alocação DINÂMICA com sqlite3_malloc (SEM limite — o antigo buffer fixo
/// de 1KB TRUNCAVA strings longas). Caller libera com sqlite3_free.
char *serialize_primitive_value_alloc(void *v, int *out_type)
{
  yyjson_val *val = (yyjson_val *)v;
  if (yyjson_is_str(val))
  {
    *out_type = TYPE_STRING;
    return json_escape_string_alloc(yyjson_get_str(val));
  }
  if (yyjson_is_int(val))
  {
    *out_type = TYPE_NUMBER;
    return sqlite3_mprintf("%lld", yyjson_get_sint(val));
  }
  if (yyjson_is_real(val))
  {
    *out_type = TYPE_NUMBER;
    return sqlite3_mprintf("%.17g", yyjson_get_real(val));
  }
  if (yyjson_is_bool(val))
  {
    *out_type = TYPE_BOOLEAN;
    return sqlite3_mprintf("%s", yyjson_get_bool(val) ? "true" : "false");
  }
  *out_type = TYPE_EMPTY;
  return sqlite3_mprintf("");
}

/// Cria um mut_val a partir do type e text_value armazenados.
/// Se type for container com inline children em text_value, faz merge.
void *make_value_from_storage(void *doc_ptr, int type, const char *text_val)
{
  yyjson_mut_doc *doc = (yyjson_mut_doc *)doc_ptr;
  switch (type)
  {
  case TYPE_OBJECT:
  case 2: /* LEGADO: antigo TYPE_ARRAY → objeto (compat de migração) */
  {
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    // Se há inline children, faz merge
    if (text_val && text_val[0] != '\0')
    {
      // Fast path: container vazio ("{}") — sem parse de yyjson_read
      // (o modo não-inline grava "{}" em TODOS os containers dedicados)
      if (text_val[0] == '{' && text_val[1] == '}' && text_val[2] == '\0')
        return obj;
      yyjson_doc *inline_doc = yyjson_read(text_val, strlen(text_val), 0);
      if (inline_doc)
      {
        yyjson_val *inline_root = yyjson_doc_get_root(inline_doc);
        if (inline_root && yyjson_is_obj(inline_root))
        {
          yyjson_obj_iter iter;
          yyjson_obj_iter_init(inline_root, &iter);
          yyjson_val *k, *v;
          while ((k = yyjson_obj_iter_next(&iter)))
          {
            v = yyjson_obj_iter_get_val(k);
            yyjson_mut_obj_add(obj,
                               yyjson_mut_strcpy(doc, yyjson_get_str(k)),
                               yyjson_val_mut_copy(doc, v));
          }
        }
        yyjson_doc_free(inline_doc);
      }
    }
    return obj;
  }
  case TYPE_NUMBER:
    if (text_val)
    {
      if (strchr(text_val, '.') || strchr(text_val, 'e') || strchr(text_val, 'E'))
        return yyjson_mut_real(doc, strtod(text_val, NULL));
      else
        return yyjson_mut_int(doc, atoll(text_val));
    }
    return yyjson_mut_int(doc, 0);
  case TYPE_BOOLEAN:
    return yyjson_mut_bool(doc, text_val && strcmp(text_val, "true") == 0);
  case TYPE_STRING:
  {
    if (text_val)
    {
      // Alocação dinâmica — o antigo buffer fixo (unesc[2048]) truncava
      char *s = json_unescape_string_alloc(text_val);
      yyjson_mut_val *v = yyjson_mut_strcpy(doc, s ? s : "");
      sqlite3_free(s);
      return v;
    }
    return yyjson_mut_strcpy(doc, "");
  }
  case TYPE_BIGINT:
  case TYPE_DATETIME:
  case TYPE_BINARY:
  case TYPE_REFERENCE:
  default:
    return yyjson_mut_strcpy(doc, text_val ? text_val : "");
  }
}

/* ===========================================================================
 * PROJEÇÃO (include/exclude de campos — caminhos pontilhados)
 * ===========================================================================
 */

/// Classifica a relação entre um path da projeção e a chave atual:
///   0 = sem relação; 1 = path == key (cópia inteira); 2 = path começa
///   com "key." (precisa descer recursivamente).
static int proj_match_kind(const char *path, const char *key, int key_len)
{
  if (strncmp(path, key, (size_t)key_len) != 0)
    return 0;
  char c = path[key_len];
  if (c == '\0')
    return 1;
  if (c == '.')
    return 2;
  return 0;
}

/// Constrói a sub-projeção do ramo "key": mantém os paths que têm "key."
/// como prefixo e os reescreve sem o prefixo.
static void proj_descend(const HeProjection *src, const char *key, int key_len,
                         HeProjection *dst)
{
  memset(dst, 0, sizeof(HeProjection));
  for (int i = 0; i < src->include_count; i++)
  {
    if (proj_match_kind(src->include[i], key, key_len) == 2)
    {
      const char *suffix = src->include[i] + key_len + 1;
      strncpy(dst->include[dst->include_count], suffix, 255);
      dst->include[dst->include_count][255] = '\0';
      dst->include_count++;
    }
  }
  for (int i = 0; i < src->exclude_count; i++)
  {
    if (proj_match_kind(src->exclude[i], key, key_len) == 2)
    {
      const char *suffix = src->exclude[i] + key_len + 1;
      strncpy(dst->exclude[dst->exclude_count], suffix, 255);
      dst->exclude[dst->exclude_count][255] = '\0';
      dst->exclude_count++;
    }
  }
}

/// Verifica se a sub-projeção (ex.: ["id"], ["a.b"]) casa com alguma
/// CHAVE do objeto — caso em que o objeto é um container NOMEADO (desce por
/// chave). Se nenhuma chave casa, o objeto é uma COLEÇÃO (objeto de objetos
/// com chaves UUID geradas no flatten de array) e a sub-projeção deve ser
/// aplicada aos VALORES (paridade com a projeção de arrays do modelo antigo).
static bool proj_obj_has_field_match(const HeProjection *proj, yyjson_val *obj)
{
  for (int i = 0; i < proj->include_count; i++)
  {
    /* primeiro segmento do path (até o '.' ou fim) */
    const char *p = proj->include[i];
    size_t seg_len = 0;
    while (p[seg_len] && p[seg_len] != '.')
      seg_len++;
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(obj, &iter);
    yyjson_val *k;
    while ((k = yyjson_obj_iter_next(&iter)))
    {
      size_t klen = yyjson_get_len(k);
      if (klen == seg_len && strncmp(yyjson_get_str(k), p, seg_len) == 0)
        return true;
    }
  }
  for (int i = 0; i < proj->exclude_count; i++)
  {
    const char *p = proj->exclude[i];
    size_t seg_len = 0;
    while (p[seg_len] && p[seg_len] != '.')
      seg_len++;
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(obj, &iter);
    yyjson_val *k;
    while ((k = yyjson_obj_iter_next(&iter)))
    {
      size_t klen = yyjson_get_len(k);
      if (klen == seg_len && strncmp(yyjson_get_str(k), p, seg_len) == 0)
        return true;
    }
  }
  return false;
}

/// Copia src aplicando a projeção (exclude vence include). Objetos que
/// perdem todas as chaves viram "{}"; arrays são projetados elemento a
/// elemento com a MESMA projeção (chaves de objetos dentro de arrays).
void *he_project_value(void *doc_ptr, void *src_ptr, const HeProjection *proj)
{
  yyjson_mut_doc *doc = (yyjson_mut_doc *)doc_ptr;
  yyjson_val *src = (yyjson_val *)src_ptr;

  // Fast path: projeção vazia → deep-copy idêntico ao comportamento atual
  if (!proj || (proj->include_count == 0 && proj->exclude_count == 0))
    return yyjson_val_mut_copy(doc, src);

  if (yyjson_is_obj(src))
  {
    yyjson_mut_val *out = yyjson_mut_obj(doc);
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(src, &iter);
    yyjson_val *k, *v;
    while ((k = yyjson_obj_iter_next(&iter)))
    {
      v = yyjson_obj_iter_get_val(k);
      const char *key = yyjson_get_str(k);
      size_t klen = yyjson_get_len(k);
      if (klen > 255)
        klen = 255;
      int key_len = (int)klen;

      // Include: sem whitelist → mantém; com whitelist → só se casar
      int inc = 0;
      for (int i = 0; i < proj->include_count && inc != 1; i++)
      {
        int kind = proj_match_kind(proj->include[i], key, key_len);
        if (kind > inc)
          inc = kind;
      }
      // Exclude: vence include (exato remove; prefixo desce e poda)
      int exc = 0;
      for (int i = 0; i < proj->exclude_count && exc != 1; i++)
      {
        int kind = proj_match_kind(proj->exclude[i], key, key_len);
        if (kind > exc)
          exc = kind;
      }

      if (exc == 1)
        continue; /* exclude exato → remove a chave inteira */
      if (proj->include_count > 0 && inc == 0)
        continue; /* whitelist sem match → remove */

      yyjson_mut_val *copy;
      if (inc == 2 || exc == 2)
      {
        HeProjection sub;
        proj_descend(proj, key, key_len, &sub);
        // Coleção (objeto de objetos com chaves UUID): a sub-projeção não
        // casa com as chaves → aplicar aos VALORES, preservando as chaves.
        // Ex.: include ["members.id"] → members: {uuid:{id}, uuid:{id}}.
        if (yyjson_is_obj(v) && !proj_obj_has_field_match(&sub, v))
        {
          yyjson_mut_val *coll = yyjson_mut_obj(doc);
          yyjson_obj_iter citer;
          yyjson_obj_iter_init(v, &citer);
          yyjson_val *ck, *cv;
          while ((ck = yyjson_obj_iter_next(&citer)))
          {
            cv = yyjson_obj_iter_get_val(ck);
            yyjson_mut_val *copy_val = he_project_value(doc, cv, &sub);
            if (copy_val)
              yyjson_mut_obj_add(coll,
                                 yyjson_mut_strcpy(doc, yyjson_get_str(ck)),
                                 copy_val);
          }
          copy = coll;
        }
        else
        {
          copy = he_project_value(doc, v, &sub);
        }
        if (!copy)
          continue;
      }
      else
      {
        copy = yyjson_val_mut_copy(doc, v);
      }
      yyjson_mut_obj_add(out, yyjson_mut_strncpy(doc, key, klen), copy);
    }
    return out;
  }

  if (yyjson_is_arr(src))
  {
    yyjson_mut_val *out = yyjson_mut_arr(doc);
    size_t n = yyjson_arr_size(src);
    for (size_t i = 0; i < n; i++)
    {
      yyjson_val *item = yyjson_arr_get(src, i);
      yyjson_mut_val *copy = (yyjson_is_obj(item) || yyjson_is_arr(item))
                                 ? he_project_value(doc, item, proj)
                                 : yyjson_val_mut_copy(doc, item);
      yyjson_mut_arr_append(out, copy);
    }
    return out;
  }

  return yyjson_val_mut_copy(doc, src);
}

/// Parseia o JSON de opções `{"include":[...],"exclude":[...]}` (arrays de
/// strings com caminhos pontilhados ou padrões wildcard de path).
void he_projection_parse(void *options_ptr, HeProjection *proj)
{
  memset(proj, 0, sizeof(HeProjection));
  yyjson_val *options = (yyjson_val *)options_ptr;
  if (!options || !yyjson_is_obj(options))
    return;

  yyjson_val *inc = yyjson_obj_get(options, "include");
  if (inc && yyjson_is_arr(inc))
  {
    size_t n = yyjson_arr_size(inc);
    for (size_t i = 0; i < n && proj->include_count < HE_MAX_PROJ_KEYS; i++)
    {
      yyjson_val *item = yyjson_arr_get(inc, i);
      if (yyjson_is_str(item))
      {
        const char *s = yyjson_get_str(item);
        size_t len = yyjson_get_len(item);
        size_t cp = len < 255 ? len : 255;
        memcpy(proj->include[proj->include_count], s, cp);
        proj->include[proj->include_count][cp] = '\0';
        proj->include_count++;
      }
    }
  }

  yyjson_val *exc = yyjson_obj_get(options, "exclude");
  if (exc && yyjson_is_arr(exc))
  {
    size_t n = yyjson_arr_size(exc);
    for (size_t i = 0; i < n && proj->exclude_count < HE_MAX_PROJ_KEYS; i++)
    {
      yyjson_val *item = yyjson_arr_get(exc, i);
      if (yyjson_is_str(item))
      {
        const char *s = yyjson_get_str(item);
        size_t len = yyjson_get_len(item);
        size_t cp = len < 255 ? len : 255;
        memcpy(proj->exclude[proj->exclude_count], s, cp);
        proj->exclude[proj->exclude_count][cp] = '\0';
        proj->exclude_count++;
      }
    }
  }
}
