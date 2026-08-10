/*
 * he_query.c — Camada QUERY ENGINE do motor SQLite (Clean Architecture).
 *
 * Filtros, ordenação e paginação sobre JSON reconstruído. Suporta
 * prefixo direto e padrão wildcard multi-nível (* e $var). A ordenação
 * usa um quicksort local com contexto injetado (sem estado global —
 * thread-safe e reentrante).
 */

#include <sqlite3ext.h>
/* sqlite3_api é definida em he_extension.c (SQLITE_EXTENSION_INIT1/INIT2).
 * As macros do sqlite3ext.h usam esta variável — declarar extern aqui. */
extern const sqlite3_api_routines *sqlite3_api;

#include <yyjson.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "he_types.h"
#include "he_utils.h"
#include "he_repo.h"
#include "he_query.h"
#include "he_stmt_cache.h"

/* ===========================================================================
 * SPEC DE QUERY (filtros + ordenação + paginação)
 * ===========================================================================
 */

typedef struct
{
  QueryFilter filters[64];
  int filter_count;
  QueryOrder orders[16];
  int order_count;
  size_t skip;
  size_t take;
  HeProjection proj; /* include/exclude de campos (vazio = sem projeção) */
} HeQuerySpec;

/// Pré-parseia uma chave pontilhada ("address.city") em segmentos — evita
/// copiar + strtok a cada avaliação de filtro/ordenação (hot path).
static void query_split_key(const char *key, char segments[][64], int *count)
{
  int sc = 0;
  const char *p = key;
  while (*p && sc < 8)
  {
    const char *dot = strchr(p, '.');
    size_t len = dot ? (size_t)(dot - p) : strlen(p);
    if (len >= 64)
      len = 63;
    memcpy(segments[sc], p, len);
    segments[sc][len] = '\0';
    sc++;
    if (!dot)
      break;
    p = dot + 1;
  }
  *count = sc;
}

/// Parseia filtros, ordenação e paginação do JSON da query (idêntico ao
/// comportamento original; extraído para eliminar duplicação entre o
/// caminho wildcard e o caminho direto).
static void query_spec_parse(yyjson_val *query_root, HeQuerySpec *spec)
{
  memset(spec, 0, sizeof(HeQuerySpec));
  if (!query_root)
  {
    spec->take = 0;
    return;
  }

  // Filtros
  yyjson_val *filters_val = yyjson_obj_get(query_root, "filters");
  if (filters_val && yyjson_is_arr(filters_val))
  {
    size_t n = yyjson_arr_size(filters_val);
    for (size_t i = 0; i < n && spec->filter_count < 64; i++)
    {
      yyjson_val *f = yyjson_arr_get(filters_val, i);
      if (!yyjson_is_obj(f))
        continue;

      yyjson_val *key_v = yyjson_obj_get(f, "key");
      yyjson_val *op_v = yyjson_obj_get(f, "op");
      yyjson_val *cmp_v = yyjson_obj_get(f, "compare");

      if (key_v && yyjson_is_str(key_v) && op_v && yyjson_is_str(op_v))
      {
        QueryFilter *flt = &spec->filters[spec->filter_count];
        const char *kstr = yyjson_get_str(key_v);
        strncpy(flt->key, kstr, sizeof(flt->key) - 1);
        query_split_key(kstr, flt->segments, &flt->seg_count);
        strncpy(flt->op, yyjson_get_str(op_v), sizeof(flt->op) - 1);
        if (cmp_v)
        {
          flt->compare_val = (void *)cmp_v;
          if (yyjson_is_str(cmp_v))
            strncpy(flt->compare, yyjson_get_str(cmp_v), sizeof(flt->compare) - 1);
          else if (yyjson_is_num(cmp_v))
            snprintf(flt->compare, sizeof(flt->compare), "%.17g", yyjson_get_num(cmp_v));
          else if (yyjson_is_bool(cmp_v))
            strcpy(flt->compare, yyjson_get_bool(cmp_v) ? "true" : "false");
        }
        flt->valid = true;
        spec->filter_count++;
      }
    }
  }

  // Ordenação
  yyjson_val *orders_val = yyjson_obj_get(query_root, "order");
  if (orders_val && yyjson_is_arr(orders_val))
  {
    size_t n = yyjson_arr_size(orders_val);
    for (size_t i = 0; i < n && spec->order_count < 16; i++)
    {
      yyjson_val *o = yyjson_arr_get(orders_val, i);
      if (!yyjson_is_obj(o))
        continue;
      yyjson_val *k = yyjson_obj_get(o, "key");
      yyjson_val *a = yyjson_obj_get(o, "ascending");
      if (k && yyjson_is_str(k))
      {
        QueryOrder *ord = &spec->orders[spec->order_count];
        const char *kstr = yyjson_get_str(k);
        strncpy(ord->key, kstr, sizeof(ord->key) - 1);
        query_split_key(kstr, ord->segments, &ord->seg_count);
        ord->ascending = !a ||
                         (yyjson_is_bool(a) && yyjson_get_bool(a)) ||
                         (yyjson_is_int(a) && yyjson_get_int(a) == 1);
        spec->order_count++;
      }
    }
  }

  // Paginação
  yyjson_val *skip_val = yyjson_obj_get(query_root, "skip");
  yyjson_val *take_val = yyjson_obj_get(query_root, "take");
  spec->skip = skip_val && yyjson_is_int(skip_val) ? (size_t)yyjson_get_int(skip_val) : 0;
  spec->take = take_val && yyjson_is_int(take_val) ? (size_t)yyjson_get_int(take_val) : 0;
  if (spec->take == 0)
    spec->take = (size_t)-1;

  // Projeção de campos (include/exclude — paths pontilhados)
  he_projection_parse(query_root, &spec->proj);
}

/* ===========================================================================
 * OPERADOR LIKE (fnmatch-style com %) — COMPARTILHADO
 * ===========================================================================
 * Semântica do operador "like" do query engine: %x% = substring, x% =
 * prefixo, %x = sufixo, sem % = igualdade exata. Usado por evaluate_filter
 * (imutável — pipeline direto/fallback) E por evaluate_filter_mut
 * (mutável — streaming). NUNCA implementar like como strstr literal dos %
 * (o streaming retornaria resultados divergentes do fallback — bug real
 * confirmado: "%Genius%" dava 0 itens no streaming vs 2 no fallback).
 */
static bool like_match(const char *str_val, const char *pattern)
{
  size_t clen = strlen(pattern);
  size_t slen = strlen(str_val);
  if (clen == 0)
    return (slen == 0);
  /* %x% → substring */
  if (pattern[0] == '%' && pattern[clen - 1] == '%')
  {
    size_t sub_len = clen - 2;
    char sub[256];
    if (sub_len < sizeof(sub))
    {
      memcpy(sub, pattern + 1, sub_len);
      sub[sub_len] = '\0';
      return strstr(str_val, sub) != NULL;
    }
    return false;
  }
  /* %x → sufixo */
  if (pattern[0] == '%')
  {
    const char *suffix = pattern + 1;
    size_t sul = strlen(suffix);
    return slen >= sul && strcmp(str_val + slen - sul, suffix) == 0;
  }
  /* x% → prefixo */
  if (pattern[clen - 1] == '%')
  {
    size_t prefix_len = clen - 1;
    if (prefix_len < 256)
    {
      char prefix[256];
      memcpy(prefix, pattern, prefix_len);
      prefix[prefix_len] = '\0';
      return strncmp(str_val, prefix, prefix_len) == 0;
    }
    return false;
  }
  /* sem % → igualdade exata */
  return strcmp(str_val, pattern) == 0;
}

/* ===========================================================================
 * AVALIAÇÃO DE FILTROS
 * ===========================================================================
 */

/// Avalia um filtro em um valor JSON
static bool evaluate_filter(yyjson_val *obj, QueryFilter *filter)
{
  // Navega pela chain de chaves (ex: "address.city") usando os segmentos
  // pré-parseados no query_spec_parse — sem copiar + strtok por avaliação.
  yyjson_val *val = obj;
  for (int s = 0; s < filter->seg_count && val; s++)
  {
    if (yyjson_is_obj(val))
      val = yyjson_obj_get(val, filter->segments[s]);
    else
      val = NULL;
  }

  if (!val)
  {
    // Valor não encontrado
    if (strcmp(filter->op, "!exists") == 0)
      return true;
    if (strcmp(filter->op, "exists") == 0)
      return false;
    // Para outros operadores, null não corresponde
    return false;
  }

  // Extrai o valor a comparar
  double num_val = 0;
  bool is_num = false;
  const char *str_val = NULL;

  if (yyjson_is_num(val))
  {
    num_val = yyjson_get_num(val);
    is_num = true;
  }
  else if (yyjson_is_bool(val))
  {
    num_val = yyjson_get_bool(val) ? 1 : 0;
    is_num = true;
  }
  else if (yyjson_is_str(val))
  {
    str_val = yyjson_get_str(val);
  }
  else if (yyjson_is_null(val))
  {
    if (strcmp(filter->op, "==") == 0 && strcmp(filter->compare, "null") == 0)
      return true;
    if (strcmp(filter->op, "!=") == 0 && strcmp(filter->compare, "null") != 0)
      return true;
    return false;
  }
  else
  {
    return false;
  }

  const char *op = filter->op;
  yyjson_val *compare_val = (yyjson_val *)filter->compare_val;

  // Operadores de comparação numérica
  if (is_num && compare_val && yyjson_is_num(compare_val))
  {
    double cmp = yyjson_get_num(compare_val);

    if (strcmp(op, "<") == 0)
      return num_val < cmp;
    if (strcmp(op, "<=") == 0)
      return num_val <= cmp;
    if (strcmp(op, "==") == 0)
      return num_val == cmp;
    if (strcmp(op, "!=") == 0)
      return num_val != cmp;
    if (strcmp(op, ">=") == 0)
      return num_val >= cmp;
    if (strcmp(op, ">") == 0)
      return num_val > cmp;
    if (strcmp(op, "between") == 0 && yyjson_is_arr(compare_val))
    {
      yyjson_val *lo = yyjson_arr_get(compare_val, 0);
      yyjson_val *hi = yyjson_arr_get(compare_val, 1);
      if (lo && hi && yyjson_is_num(lo) && yyjson_is_num(hi))
        return num_val >= yyjson_get_num(lo) && num_val <= yyjson_get_num(hi);
    }
  }

  // Operadores de string
  if (str_val != NULL)
  {
    if (strcmp(op, "==") == 0)
      return strcmp(str_val, filter->compare) == 0;
    if (strcmp(op, "!=") == 0)
      return strcmp(str_val, filter->compare) != 0;
    if (strcmp(op, "like") == 0)
      return like_match(str_val, filter->compare);
    if (strcmp(op, "matches") == 0)
    {
      // regex match (simplificado: substring case-insensitive)
      return strstr(str_val, filter->compare) != NULL;
    }
    if (strcmp(op, "in") == 0 && compare_val && yyjson_is_arr(compare_val))
    {
      size_t n = yyjson_arr_size(compare_val);
      for (size_t i = 0; i < n; i++)
      {
        yyjson_val *item = yyjson_arr_get(compare_val, i);
        if (yyjson_is_str(item) && strcmp(str_val, yyjson_get_str(item)) == 0)
          return true;
        if (yyjson_is_num(item) && is_num && yyjson_get_num(item) == num_val)
          return true;
      }
      return false;
    }
  }

  // Operadores universais
  if (strcmp(op, "exists") == 0)
    return true;
  if (strcmp(op, "!exists") == 0)
    return false;

  return false;
}

/* ===========================================================================
 * ORDENAÇÃO (sem estado global — contexto injetado)
 * ===========================================================================
 */

/// Igual a evaluate_filter, mas para valores MUTÁVEIS (yyjson_mut_val*):
/// o streaming reconstrói os filhos diretos como mut_val (lista circular),
/// então yyjson_obj_get (que assume memória contígua de imutável) leria
/// memória errada. A navegação por chaves usa yyjson_mut_obj_get.
static bool evaluate_filter_mut(yyjson_mut_val *obj, QueryFilter *filter)
{
  yyjson_mut_val *val = obj;
  for (int s = 0; s < filter->seg_count && val; s++)
  {
    if (yyjson_mut_is_obj(val))
      val = yyjson_mut_obj_get(val, filter->segments[s]);
    else
      val = NULL;
  }

  if (!val)
  {
    if (strcmp(filter->op, "!exists") == 0)
      return true;
    if (strcmp(filter->op, "exists") == 0)
      return false;
    return false;
  }

  double num_val = 0;
  bool is_num = false;
  const char *str_val = NULL;

  if (yyjson_mut_is_num(val))
  {
    num_val = yyjson_mut_get_num(val);
    is_num = true;
  }
  else if (yyjson_mut_is_bool(val))
  {
    num_val = yyjson_mut_get_bool(val) ? 1 : 0;
    is_num = true;
  }
  else if (yyjson_mut_is_str(val))
  {
    str_val = yyjson_mut_get_str(val);
  }
  else if (yyjson_mut_is_null(val))
  {
    if (strcmp(filter->op, "==") == 0 && strcmp(filter->compare, "null") == 0)
      return true;
    if (strcmp(filter->op, "!=") == 0 && strcmp(filter->compare, "null") != 0)
      return true;
    return false;
  }
  else
  {
    return false;
  }

  const char *op = filter->op;
  yyjson_val *compare_val = (yyjson_val *)filter->compare_val;

  if (is_num && compare_val && yyjson_is_num(compare_val))
  {
    double cmp = yyjson_get_num(compare_val);
    if (strcmp(op, "<") == 0)
      return num_val < cmp;
    if (strcmp(op, "<=") == 0)
      return num_val <= cmp;
    if (strcmp(op, "==") == 0)
      return num_val == cmp;
    if (strcmp(op, "!=") == 0)
      return num_val != cmp;
    if (strcmp(op, ">=") == 0)
      return num_val >= cmp;
    if (strcmp(op, ">") == 0)
      return num_val > cmp;
    if (strcmp(op, "between") == 0 && yyjson_is_arr(compare_val))
    {
      yyjson_val *lo = yyjson_arr_get(compare_val, 0);
      yyjson_val *hi = yyjson_arr_get(compare_val, 1);
      if (lo && hi && yyjson_is_num(lo) && yyjson_is_num(hi))
        return num_val >= yyjson_get_num(lo) && num_val <= yyjson_get_num(hi);
    }
  }

  if (str_val != NULL)
  {
    if (strcmp(op, "==") == 0)
      return strcmp(str_val, filter->compare) == 0;
    if (strcmp(op, "!=") == 0)
      return strcmp(str_val, filter->compare) != 0;
    if (strcmp(op, "like") == 0)
      return like_match(str_val, filter->compare);
    if (strcmp(op, "matches") == 0)
      return strstr(str_val, filter->compare) != NULL;
    if (strcmp(op, "in") == 0 && compare_val && yyjson_is_arr(compare_val))
    {
      size_t n = yyjson_arr_size(compare_val);
      for (size_t i = 0; i < n; i++)
      {
        yyjson_val *item = yyjson_arr_get(compare_val, i);
        if (yyjson_is_str(item) && strcmp(str_val, yyjson_get_str(item)) == 0)
          return true;
        if (yyjson_is_num(item) && is_num && yyjson_get_num(item) == num_val)
          return true;
      }
      return false;
    }
  }

  if (strcmp(op, "exists") == 0)
    return true;
  if (strcmp(op, "!exists") == 0)
    return false;
  return false;
} /// Compara duas SortEntry de acordo com as QueryOrder.
/// Recebe o array de orders via arg (DIP — elimina o antigo g_sort_orders).
static int compare_entries(const void *a, const void *b, void *arg)
{
  const SortEntry *ea = (const SortEntry *)a;
  const SortEntry *eb = (const SortEntry *)b;
  QueryOrder *orders = (QueryOrder *)arg;

  if (!orders)
    return 0;

  for (int i = 0; orders[i].key[0] != '\0'; i++)
  {
    // Navega pela chain de chaves usando os segmentos pré-parseados
    // (sem copiar + strtok por comparação — hot path do qsort).
    yyjson_val *va = (yyjson_val *)ea->val;
    yyjson_val *vb = (yyjson_val *)eb->val;
    for (int s = 0; s < orders[i].seg_count && va && vb; s++)
    {
      if (yyjson_is_obj(va))
        va = yyjson_obj_get(va, orders[i].segments[s]);
      else
        va = NULL;
      if (yyjson_is_obj(vb))
        vb = yyjson_obj_get(vb, orders[i].segments[s]);
      else
        vb = NULL;
    }

    double na = 0, nb = 0;
    bool a_num = false, b_num = false;
    const char *sa = NULL, *sb = NULL;

    if (va && yyjson_is_num(va))
    {
      na = yyjson_get_num(va);
      a_num = true;
    }
    else if (va && yyjson_is_str(va))
      sa = yyjson_get_str(va);
    if (vb && yyjson_is_num(vb))
    {
      nb = yyjson_get_num(vb);
      b_num = true;
    }
    else if (vb && yyjson_is_str(vb))
      sb = yyjson_get_str(vb);

    int cmp = 0;
    if (a_num && b_num)
      cmp = (na < nb) ? -1 : (na > nb) ? 1
                                       : 0;
    else if (sa && sb)
      cmp = strcmp(sa, sb);
    else if (a_num && !b_num)
      cmp = -1;
    else if (!a_num && b_num)
      cmp = 1;
    else
      cmp = 0;

    if (cmp != 0)
      return orders[i].ascending ? cmp : -cmp;
  }

  return (ea->index < eb->index) ? -1 : (ea->index > eb->index) ? 1
                                                                : 0;
}

/// Quicksort local com comparador que recebe contexto (portátil entre
/// MSVC/MinGW/glibc/macOS, ao contrário de qsort_r/qsort_s). A ordem é
/// total graças ao tiebreak por index, então o resultado independe do
/// algoritmo — igual ao comportamento anterior com qsort.
static void he_qsort_impl(char *base, size_t lo, size_t hi, size_t size,
                          int (*cmp)(const void *, const void *, void *),
                          void *arg)
{
  while (lo < hi)
  {
    // Insertion sort para partições pequenas (evita recursão profunda)
    if (hi - lo < 8)
    {
      for (size_t i = lo + 1; i <= hi; i++)
      {
        char tmp[512];
        memcpy(tmp, base + i * size, size);
        size_t j = i;
        while (j > lo && cmp(base + (j - 1) * size, tmp, arg) > 0)
        {
          memcpy(base + j * size, base + (j - 1) * size, size);
          j--;
        }
        memcpy(base + j * size, tmp, size);
      }
      return;
    }

    // Partição (Lomuto) com pivô no meio
    size_t mid = lo + (hi - lo) / 2;
    char tmp[512];
    memcpy(tmp, base + mid * size, size);
    memcpy(base + mid * size, base + hi * size, size);
    memcpy(base + hi * size, tmp, size);

    size_t i = lo;
    for (size_t j = lo; j < hi; j++)
    {
      if (cmp(base + j * size, base + hi * size, arg) <= 0)
      {
        memcpy(tmp, base + i * size, size);
        memcpy(base + i * size, base + j * size, size);
        memcpy(base + j * size, tmp, size);
        i++;
      }
    }
    memcpy(tmp, base + i * size, size);
    memcpy(base + i * size, base + hi * size, size);
    memcpy(base + hi * size, tmp, size);

    // Recursão na partição menor, iteração na maior (O(log n) de stack)
    if (i - lo < hi - i)
    {
      if (i > lo)
        he_qsort_impl(base, lo, i - 1, size, cmp, arg);
      lo = i + 1;
    }
    else
    {
      if (i < hi)
        he_qsort_impl(base, i + 1, hi, size, cmp, arg);
      hi = i - 1;
    }
  }
}

static void he_qsort(void *base, size_t nmemb, size_t size,
                     int (*cmp)(const void *, const void *, void *),
                     void *arg)
{
  if (!base || nmemb < 2)
    return;
  he_qsort_impl((char *)base, 0, nmemb - 1, size, cmp, arg);
}

/* ===========================================================================
 * HELPERS PARA WILDCARD COM CAPTURA DE VARIÁVEIS
 * ===========================================================================
 */

/// Extrai um segmento específico de um path.
/// Ex: extract_path_segment("/users/alice/posts/p123", 3) → "p123"
static bool extract_path_segment(const char *path, int seg_idx,
                                 char *out, size_t out_size)
{
  if (!path || !*path || out_size == 0)
    return false;
  out[0] = '\0';
  const char *p = path;
  if (*p == '/')
    p++;
  int current = 0;
  while (*p && current < seg_idx)
  {
    const char *end = strchr(p, '/');
    if (!end)
      return false;
    p = end + 1;
    current++;
  }
  if (!*p || current != seg_idx)
    return false;
  const char *end = strchr(p, '/');
  size_t len = end ? (size_t)(end - p) : strlen(p);
  if (len >= out_size)
    len = out_size - 1;
  memcpy(out, p, len);
  out[len] = '\0';
  return true;
}

/// Avalia um filtro de variável (ex: $postId == "abc" ou {key} == "abc")
/// comparando o valor capturado com o valor do filtro.
static bool evaluate_var_filter(const char *captured_val,
                                const QueryFilter *filter)
{
  if (!captured_val)
    return false;

  if (strcmp(filter->op, "==") == 0)
    return strcmp(captured_val, filter->compare) == 0;
  if (strcmp(filter->op, "!=") == 0)
    return strcmp(captured_val, filter->compare) != 0;
  if (strcmp(filter->op, "like") == 0)
    return strstr(captured_val, filter->compare) != NULL;
  return false;
}

/* ===========================================================================
 * DO_QUERY_WILDCARD: executa query em paths com wildcard multi-nível
 * ===========================================================================
 */

/// Processa uma query onde o path contém wildcards (* ou $var).
/// Retorna string alocada (JSON array) ou NULL com *err (mensagem) em
/// erro; NULL sem *err significa "sem dados" (equivalente a "[]").
static char *do_query_wildcard(HeStmtCache *cache, const char *pattern,
                               const char *query_str, char **err)
{
  if (err)
    *err = NULL;

  // ── 1. Parseia o JSON da query ──
  yyjson_doc *query_doc = yyjson_read(query_str, strlen(query_str), 0);
  if (!query_doc)
  {
    if (err)
      *err = sqlite3_mprintf("Invalid query JSON in wildcard query");
    return NULL;
  }
  yyjson_val *query_root = yyjson_doc_get_root(query_doc);
  if (!query_root)
  {
    yyjson_doc_free(query_doc);
    if (err)
      *err = sqlite3_mprintf("Empty query JSON");
    return NULL;
  }

  // ── 2. Extrai filtros, ordenação e paginação ──
  HeQuerySpec spec;
  query_spec_parse(query_root, &spec);

  // ── 3. Separa filtros de variável de filtros de campo ──
  QueryFilter field_filters[64];
  int field_filter_count = 0;
  QueryFilter var_filters[16];
  int var_filter_count = 0;

  for (int f = 0; f < spec.filter_count; f++)
  {
    const char *k = spec.filters[f].key;
    if (k[0] == '$' || strcmp(k, "{key}") == 0)
    {
      if (var_filter_count < 16)
        var_filters[var_filter_count++] = spec.filters[f];
    }
    else
    {
      if (field_filter_count < 64)
        field_filters[field_filter_count++] = spec.filters[f];
    }
  }

  // ── 4. Obtém prefixo fixo e parseia o padrão wildcard ──
  char fixed[1024];
  wildcard_fixed_prefix(pattern, fixed, sizeof(fixed));

  char upper[1024];
  make_upper_bound(fixed, upper, sizeof(upper));

  WildcardPattern wc_pat;
  wildcard_parse(pattern, &wc_pat);

  // ── 4b. Identifica capturas $variable no padrão ──
  int var_capture_seg[64];
  char var_capture_name[64][256];
  int var_capture_count = 0;
  int last_wildcard_seg = -1;

  for (int i = 0; i < wc_pat.count; i++)
  {
    if (wc_pat.is_wildcard[i])
    {
      if (wc_pat.segments[i][0] == '$' && wc_pat.segments[i][1] != '\0')
      {
        if (var_capture_count < 64)
        {
          strncpy(var_capture_name[var_capture_count],
                  wc_pat.segments[i] + 1, 255);
          var_capture_name[var_capture_count][255] = '\0';
          var_capture_seg[var_capture_count] = i;
          var_capture_count++;
        }
      }
      last_wildcard_seg = i;
    }
  }

  // ── 5. Escaneia a tabela nodes por paths que casam o padrão ──
  sqlite3_stmt *scan_stmt = cache->scan_path;
  sqlite3_reset(scan_stmt);
  sqlite3_bind_text(scan_stmt, 1, fixed, -1, SQLITE_STATIC);
  sqlite3_bind_text(scan_stmt, 2, upper, -1, SQLITE_STATIC);

  // Lista de paths que casam (já filtrados por var_filters se houver)
  char **paths = NULL;
  int path_count = 0;
  int path_cap = 128;
  paths = (char **)calloc(path_cap, sizeof(char *));

  while (sqlite3_step(scan_stmt) == SQLITE_ROW)
  {
    const char *p = (const char *)sqlite3_column_text(scan_stmt, 0);
    if (!p)
      continue;
    if (!wildcard_matches(p, &wc_pat))
      continue;

    // Se há filtros de variável, verifica antes de armazenar
    if (var_filter_count > 0)
    {
      bool passes = true;
      for (int vf = 0; vf < var_filter_count && passes; vf++)
      {
        const char *vkey = var_filters[vf].key;
        const char *captured = NULL;
        char cap_buf[256];

        if (strcmp(vkey, "{key}") == 0)
        {
          // {key} = valor do último segmento wildcard
          if (last_wildcard_seg >= 0)
          {
            if (extract_path_segment(p, last_wildcard_seg,
                                     cap_buf, sizeof(cap_buf)))
              captured = cap_buf;
          }
        }
        else if (vkey[0] == '$')
        {
          // $varname — busca nas capturas definidas
          const char *vname = vkey + 1;
          for (int vd = 0; vd < var_capture_count; vd++)
          {
            if (strcmp(var_capture_name[vd], vname) == 0)
            {
              if (extract_path_segment(p, var_capture_seg[vd],
                                       cap_buf, sizeof(cap_buf)))
                captured = cap_buf;
              break;
            }
          }
        }

        if (!captured)
        {
          passes = false;
          break;
        }
        passes = evaluate_var_filter(captured, &var_filters[vf]);
      }
      if (!passes)
        continue;
    }

    if (path_count >= path_cap)
    {
      path_cap *= 2;
      paths = (char **)realloc(paths, path_cap * sizeof(char *));
    }
    paths[path_count] = sqlite3_mprintf("%s", p);
    path_count++;
  }
  sqlite3_reset(scan_stmt);

  // ── 6. Para cada path match, obtém o valor JSON ──
  sqlite3_stmt *extract_stmt = cache->extract;
  sqlite3_reset(extract_stmt);

  yyjson_mut_doc *result_doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *result_arr = yyjson_mut_arr(result_doc);
  yyjson_mut_doc_set_root(result_doc, result_arr);

  SortEntry *entries = (SortEntry *)calloc(path_count > 0 ? path_count : 1, sizeof(SortEntry));
  yyjson_doc **item_docs = (yyjson_doc **)calloc(path_count > 0 ? path_count : 1, sizeof(yyjson_doc *));
  size_t valid = 0;

  for (int i = 0; i < path_count; i++)
  {
    sqlite3_bind_text(extract_stmt, 1, paths[i], -1, SQLITE_STATIC);
    if (sqlite3_step(extract_stmt) == SQLITE_ROW)
    {
      const char *json_str = (const char *)sqlite3_column_text(extract_stmt, 0);
      if (json_str && strcmp(json_str, "null") != 0)
      {
        yyjson_doc *item_doc = yyjson_read(json_str, strlen(json_str), 0);
        if (item_doc)
        {
          yyjson_val *root = yyjson_doc_get_root(item_doc);
          if (root)
          {
            entries[valid].val = (void *)root;
            entries[valid].index = valid;
            item_docs[valid] = item_doc;
            valid++;
          }
          else
          {
            yyjson_doc_free(item_doc);
          }
        }
      }
    }
    sqlite3_reset(extract_stmt);
    sqlite3_clear_bindings(extract_stmt);
  }

  // ── 7. Aplica filtros de campo ──
  SortEntry *filtered = NULL;
  size_t filtered_count = 0;

  if (valid == 0)
  {
    // Nenhum resultado
    yyjson_mut_doc_free(result_doc);
    free(entries);
    free(item_docs);
    for (int i = 0; i < path_count; i++)
      sqlite3_free(paths[i]);
    free(paths);
    yyjson_doc_free(query_doc);
    return sqlite3_mprintf("[]");
  }

  if (field_filter_count > 0)
  {
    filtered = (SortEntry *)calloc(valid, sizeof(SortEntry));
    for (size_t i = 0; i < valid; i++)
    {
      bool match = true;
      for (int f = 0; f < field_filter_count && match; f++)
      {
        if (!field_filters[f].valid)
          continue;
        match = evaluate_filter((yyjson_val *)entries[i].val, &field_filters[f]);
      }
      if (match)
      {
        filtered[filtered_count].val = entries[i].val;
        filtered[filtered_count].index = entries[i].index;
        filtered_count++;
      }
    }
  }
  else
  {
    filtered = entries;
    filtered_count = valid;
  }

  // ── 8. Ordena (contexto injetado — sem estado global) ──
  if (spec.order_count > 0 && filtered_count > 1)
  {
    he_qsort(filtered, filtered_count, sizeof(SortEntry), compare_entries, spec.orders);
  }

  // ── 9. Aplica skip/take ──
  size_t start = spec.skip < filtered_count ? spec.skip : filtered_count;
  size_t end = (spec.take < filtered_count - start) ? (start + spec.take) : filtered_count;

  // ── 10. Monta array de resultados (com projeção include/exclude) ──
  for (size_t i = start; i < end; i++)
  {
    if (filtered[i].val)
    {
      yyjson_mut_val *copy = he_project_value(result_doc, (yyjson_val *)filtered[i].val, &spec.proj);
      if (copy)
        yyjson_mut_arr_append(result_arr, copy);
    }
  }

  char *json_out = yyjson_mut_write(result_doc, 0, NULL);

  // ── 11. Cleanup ──
  yyjson_mut_doc_free(result_doc);
  for (size_t i = 0; i < valid; i++)
    if (item_docs[i])
      yyjson_doc_free(item_docs[i]);
  free(item_docs);
  if (filtered != entries)
    free(filtered);
  free(entries);
  for (int i = 0; i < path_count; i++)
    sqlite3_free(paths[i]);
  free(paths);
  yyjson_doc_free(query_doc);

  if (!json_out)
  {
    if (err)
      *err = sqlite3_mprintf("Failed to serialize query result");
    return NULL;
  }

  // Normaliza para alocação sqlite3 (o controller usa sqlite3_free
  // como destrutor no sqlite3_result_text).
  char *out = sqlite3_mprintf("%s", json_out);
  free(json_out);
  return out;
}

/* ===========================================================================
 * HE_QUERY_EXECUTE (ponto de entrada do query engine)
 * ===========================================================================
 */

/// Detecta o padrão de coleção "/pai/*" — um ÚNICO wildcard puro "*" no
/// ÚLTIMO segmento, sem variáveis. Para esses padrões, o motor extrai o
/// container pai UMA única vez e itera os filhos em memória (o antigo
/// caminho wildcard fazia 1 extract_json POR filho — O(n×subtree)).
static bool wildcard_is_single_tail(const char *path)
{
  if (!path || !*path)
    return false;
  const char *star = strchr(path, '*');
  if (!star || star[1] != '\0') /* '*' deve ser o último caractere */
    return false;
  if (strchr(path, '$') != NULL) /* sem $var */
    return false;
  if (strchr(path + 1, '/') == NULL) /* precisa de um segmento pai fixo antes do asterisco final */
    return false;
  return true;
}

/// Comparador para ordenar pares (chave, valor) lexicograficamente pela chave
/// — paridade com o ORDER BY path do caminho wildcard. O hashmap do yyjson
/// (modo read) NÃO tem hash table: yyjson_obj_get faz busca LINEAR, então
/// NUNCA usar obj_get por chave em objetos grandes (5000 chaves ≈ 110ms).
/// Coletar os pares na iteração + qsort de structs custa ~3ms.
typedef struct
{
  const char *key;
  yyjson_val *val;
} KeyValPair;

static int cmp_keyval_pair(const void *a, const void *b)
{
  const KeyValPair *pa = (const KeyValPair *)a;
  const KeyValPair *pb = (const KeyValPair *)b;
  return strcmp(pa->key, pb->key);
}

/* ===========================================================================
 * QUERY SINGLE-TAIL COM STREAMING + EARLY EXIT ("/pai/*" sem order)
 * ===========================================================================
 * Otimização do caminho de coleção: em vez de materializar o documento
 * inteiro (extract → parse → coleta → filtro → take), varre as linhas
 * UMA vez em ORDER BY path e reconstrói os filhos diretos do root de forma
 * INCREMENTAL (mesma stack do he_extract_json). Quando um filho direto
 * "fecha" (chegou o próximo filho direto ou o scan terminou), aplica
 * filtro + projeção + skip/take NA HORA e o descarta. Com `take` pequeno
 * e filtro seletivo, o loop para cedo — sem construir o JSON do doc todo.
 *
 * Quando aplicar: single-tail SEM order (com order todos os candidatos são
 * necessários — fallback para o pipeline atual).
 *
 * Retorna string alocada (array JSON) em sucesso; NULL sem *err quando a
 * otimização NÃO se aplica (order presente — o caller usa o caminho atual);
 * NULL com *err em erro real.
 */
static char *do_query_single_tail_stream(HeStmtCache *cache,
                                         const char *parent,
                                         const char *query_str, char **err)
{
  if (err)
    *err = NULL;

  // ── 1. Parseia a query e extrai o spec ──
  yyjson_doc *query_doc = yyjson_read(query_str, strlen(query_str), 0);
  if (!query_doc)
  {
    if (err)
      *err = sqlite3_mprintf("Invalid query JSON in single-tail query");
    return NULL;
  }
  yyjson_val *query_root = yyjson_doc_get_root(query_doc);

  HeQuerySpec spec;
  query_spec_parse(query_root, &spec);

  // Com order, TODOS os candidatos são necessários — a otimização não se
  // aplica (fallback para o pipeline atual, que ordena o conjunto todo).
  if (spec.order_count > 0)
  {
    yyjson_doc_free(query_doc);
    return NULL; /* sem *err → o caller usa o caminho atual */
  }

  // Projeção include/exclude: he_project_value espera valores IMUTÁVEIS
  // (yyjson_val_mut_copy no fast path lê memória contígua de imut) — os
  // filhos do streaming são mut. Cair no fallback (pipeline atual) mantém
  // a paridade sem duplicar a lógica de projeção mut-aware.
  if (spec.proj.include_count > 0 || spec.proj.exclude_count > 0)
  {
    yyjson_doc_free(query_doc);
    return NULL; /* sem *err → o caller usa o caminho atual */
  }

  // ── 2. Prepara o range scan do container pai ──
  //    parent = "/pai" (sem slash). prefix = "/pai/" — como '/' (0x2F) <
  //    '0' (0x30), o upper "incrementa o último byte" do prefixo captura
  //    TODOS os descendentes (range via PRIMARY KEY, ORDER BY path).
  char prefix[1024];
  snprintf(prefix, sizeof(prefix), "%s/", parent);
  char upper[1024];
  make_upper_bound(prefix, upper, sizeof(upper));

  sqlite3_stmt *stmt = cache->scan;
  sqlite3_reset(stmt);
  sqlite3_bind_text(stmt, 1, prefix, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, upper, -1, SQLITE_STATIC);

  // ── 3. Stack de reconstrução (mesma mecânica do he_extract_json) ──
  typedef struct
  {
    const char *path; /* cópia sqlite3_malloc — liberada no pop/fim */
    yyjson_mut_val *val;
    int depth; /* 0 = root; 1 = filho direto; >1 = aninhado */
  } StackNode;
  StackNode stack[2048];
  int stack_top = 0;

  yyjson_mut_doc *mut_doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *result_arr = yyjson_mut_arr(mut_doc);
  yyjson_mut_doc_set_root(mut_doc, result_arr);

  size_t added = 0;   /* filhos aprovados já adicionados */
  size_t skipped = 0; /* filhos processados (p/ skip) */
  bool root_seen = false;
  bool done = false;
  int rc = SQLITE_OK;

  // Root do container (linha "/pai/") — não é um filho, não entra no array
  yyjson_mut_val *root_val = NULL;

  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
  {
    const char *path = (const char *)sqlite3_column_text(stmt, 0);
    int type = sqlite3_column_int(stmt, 1);
    const char *text_val = (const char *)sqlite3_column_text(stmt, 2);
    if (!path)
      continue;

    // Profundidade = nº de segmentos além do prefixo.
    //   "/pai/x"       → 1 (filho direto)
    //   "/pai/x/"      → 1 (container filho — o trailing slash do próprio
    //                      nó NÃO conta como separador de descendente)
    //   "/pai/x/y/"    → 2+ (aninhado)
    int depth = 1;
    {
      const char *s = path + strlen(prefix); /* pula o prefixo "/pai/" */
      if (*s)
      {
        size_t s_len = strlen(s);
        /* remove o trailing slash final (delimitador do próprio nó) */
        if (s_len > 0 && s[s_len - 1] == '/')
          s_len--;
        for (size_t c = 0; c < s_len; c++)
          if (s[c] == '/')
            depth++;
      }
      else
      {
        depth = 0; /* linha do root "/pai/" */
      }
    }

    if (!root_seen)
    {
      // Primeira linha = root do container (pode ser só ele = doc vazio).
      // Modelo objeto-only: type=2 legado é lido como objeto (compat).
      root_seen = true;
      root_val = make_value_from_storage(mut_doc, type, text_val);
      if (!root_val)
        root_val = yyjson_mut_obj(mut_doc);
      stack[stack_top++] = (StackNode){sqlite3_mprintf("%s", path), root_val, 0};
      continue;
    }

    if (depth <= 1)
    {
      // ── FECHAMENTO do filho direto anterior (se houver) ──
      // O child (stack[1].val) é um mut_val DETACHED (árvore completa já
      // reconstruída, sem pai) — avaliar filtro e MOVÊ-LO para o array de
      // resultados (append sem cópia: evita yyjson_val_mut_copy, que só
      // funciona para valores imutáveis).
      if (stack_top > 1)
      {
        yyjson_mut_val *child = stack[1].val;
        bool match = true;
        for (int f = 0; f < spec.filter_count && match; f++)
        {
          if (!spec.filters[f].valid)
            continue;
          match = evaluate_filter_mut(child, &spec.filters[f]);
        }
        if (match)
        {
          if (skipped < spec.skip)
          {
            skipped++;
          }
          else
          {
            yyjson_mut_arr_append(result_arr, child);
            added++;
            if (added >= spec.take)
              done = true; /* early exit */
          }
        }
        // Libera a subtree do filho direto fechado (paths da stack)
        while (stack_top > 1)
          sqlite3_free((void *)stack[--stack_top].path);
        if (done)
          break;
      }

      // ── Novo filho direto ──
      yyjson_mut_val *child = make_value_from_storage(mut_doc, type, text_val);
      if (!child)
        child = yyjson_mut_obj(mut_doc);
      stack[stack_top++] = (StackNode){sqlite3_mprintf("%s", path), child, 1};
      continue;
    }

    // ── Nó aninhado (depth > 1): mesmo fluxo do he_extract_json ──
    // 1. Encontra o pai na stack (pop dos que saíram do caminho)
    while (stack_top > 1)
    {
      const char *sp = stack[stack_top - 1].path;
      size_t sp_len = strlen(sp);
      size_t slen = sp_len;
      while (slen > 0 && sp[slen - 1] == '/')
        slen--;
      if (strncmp(path, sp, slen) == 0 &&
          (path[slen] == '/' || path[slen] == '\0'))
        break;
      sqlite3_free((void *)stack[--stack_top].path);
    }
    yyjson_mut_val *parent = stack_top > 0 ? stack[stack_top - 1].val : root_val;

    // 2. Extrai a chave (último segmento)
    char key_buf[256];
    const char *key = key_buf;
    int path_end = (int)strlen(path);
    if (path_end > 0 && path[path_end - 1] == '/')
      path_end--;
    const char *last_slash = NULL;
    for (int si = path_end - 1; si >= 0; si--)
    {
      if (path[si] == '/')
      {
        last_slash = &path[si];
        break;
      }
    }
    if (last_slash)
    {
      int key_len = path_end - (int)(last_slash - path) - 1;
      if (key_len > 0 && key_len < (int)sizeof(key_buf) - 1)
      {
        memcpy(key_buf, last_slash + 1, key_len);
        key_buf[key_len] = '\0';
      }
      else
        key = path;
    }
    else
      key = path;

    // 3. Cria o valor e adiciona ao pai — modelo OBJETO-ONLY: o pai nunca é
    //    array (dados legados type=2 são lidos como objeto), então obj_add.
    yyjson_mut_val *child = make_value_from_storage(mut_doc, type, text_val);
    if (!child)
      child = yyjson_mut_obj(mut_doc);
    yyjson_mut_obj_add(parent, yyjson_mut_strcpy(mut_doc, key), child);

    // 4. Se container, empilha
    bool is_container = (type == TYPE_OBJECT || type == 2); /* 2 = LEGADO */
    if (is_container && stack_top < 2048)
      stack[stack_top++] = (StackNode){sqlite3_mprintf("%s", path), child, depth};
  }

  // ── Fim do scan: fecha o ÚLTIMO filho direto (se não houve early exit) ──
  if (!done && stack_top > 1)
  {
    yyjson_mut_val *child = stack[1].val;
    bool match = true;
    for (int f = 0; f < spec.filter_count && match; f++)
    {
      if (!spec.filters[f].valid)
        continue;
      match = evaluate_filter_mut(child, &spec.filters[f]);
    }
    if (match)
    {
      if (skipped < spec.skip)
        skipped++;
      else
      {
        yyjson_mut_arr_append(result_arr, child);
        added++;
      }
    }
  }

  sqlite3_reset(stmt);

  // Libera os paths da stack (cópias sqlite3_malloc)
  for (int i = 0; i < stack_top; i++)
    sqlite3_free((void *)stack[i].path);

  // Container pai inexistente → NULL (SQL NULL), idêntico ao caminho atual
  // (o extract devolvia 'null' → data_doc NULL → retorno NULL).
  if (!root_seen)
  {
    yyjson_mut_doc_free(mut_doc);
    yyjson_doc_free(query_doc);
    return NULL;
  }

  // ── Serializa SÓ o array de resultados ──
  // O root do container (stack[0]) está DETACHED do array — o mut_doc tem
  // como root o result_arr. yyjson_mut_write serializa a partir do root
  // definido, então filhos órfãos não são serializados.
  char *json_out = yyjson_mut_write(mut_doc, 0, NULL);
  yyjson_mut_doc_free(mut_doc);
  yyjson_doc_free(query_doc);

  if (!json_out)
  {
    if (err)
      *err = sqlite3_mprintf("Failed to serialize query result");
    return NULL;
  }

  // Normaliza para alocação sqlite3 (destrutor uniforme no controller)
  char *out = sqlite3_mprintf("%s", json_out);
  free(json_out);
  return out;
}

/// query_json(prefix, query_json) — dispatch wildcard vs direto.
/// Retorna string alocada (array JSON) ou NULL (com *err se erro).
char *he_query_execute(HeStmtCache *cache, const char *prefix_input,
                       const char *query_str, char **err)
{
  if (err)
    *err = NULL;

  if (!prefix_input || !query_str)
    return NULL;

  // Verifica se há wildcard multi-nível
  bool has_wildcard = path_has_wildcard(prefix_input);

  if (has_wildcard)
  {
    if (wildcard_is_single_tail(prefix_input))
    {
      // ─── "/pai/*" (coleção) → tenta o caminho STREAMING (early-exit);
      //     com order, cai no caminho direto (fallback).
      char parent[1024];
      wildcard_fixed_prefix(prefix_input, parent, sizeof(parent)); // "/pai/"
      path_without_trailing_slash(parent, parent, sizeof(parent)); // "/pai"
      char *streamed = do_query_single_tail_stream(cache, parent, query_str, err);
      if (streamed || (err && *err))
        return streamed;
      prefix_input = parent; // fallback → pipeline atual abaixo
    }
    else
    {
      // ─── WILDCARD MULTI-NÍVEL: busca paths que casam o padrão ───
      return do_query_wildcard(cache, prefix_input, query_str, err);
    }
  }

  // Normaliza o prefixo
  char query_prefix[1024];
  normalize_path(prefix_input, query_prefix, sizeof(query_prefix));

  // Prepara e executa extract_json via SQL interno
  char extract_arg[1024];
  path_without_trailing_slash(query_prefix, extract_arg, sizeof(extract_arg));

  char *raw = he_repo_extract_json(cache, extract_arg);
  if (!raw)
  {
    if (err)
      *err = sqlite3_mprintf("Failed to prepare extract");
    return NULL;
  }

  yyjson_doc *data_doc = NULL;
  if (strcmp(raw, "null") != 0)
  {
    data_doc = yyjson_read(raw, strlen(raw), 0);
  }
  sqlite3_free(raw);

  if (!data_doc)
  {
    return NULL; // resultado SQL NULL
  }
  yyjson_val *data_root = yyjson_doc_get_root(data_doc);

  // Parse a query JSON
  yyjson_doc *query_doc = yyjson_read(query_str, strlen(query_str), 0);
  if (!query_doc)
  {
    yyjson_doc_free(data_doc);
    if (err)
      *err = sqlite3_mprintf("Invalid query JSON");
    return NULL;
  }
  yyjson_val *query_root = yyjson_doc_get_root(query_doc);

  // Extrai filtros, ordenação e paginação
  HeQuerySpec spec;
  query_spec_parse(query_root, &spec);

  // Coleta os valores do array/objeto de dados
  SortEntry *entries = NULL;
  size_t total = 0;

  if (yyjson_is_arr(data_root))
  {
    total = yyjson_arr_size(data_root);
    entries = (SortEntry *)calloc(total, sizeof(SortEntry));
    for (size_t i = 0; i < total; i++)
    {
      entries[i].val = (void *)yyjson_arr_get(data_root, i);
      entries[i].index = i;
    }
  }
  else if (yyjson_is_obj(data_root))
  {
    // Coleta os pares (chave, valor) na iteração e ordena lexicograficamente
    // pela chave — paridade com o ORDER BY path do caminho wildcard (a ordem
    // do hashmap do yyjson é arbitrária). NUNCA usar yyjson_obj_get aqui:
    // sem hash table, cada lookup é O(n) (5000 chaves ≈ 110ms).
    total = yyjson_obj_size(data_root);
    entries = (SortEntry *)calloc(total, sizeof(SortEntry));
    KeyValPair *pairs = (KeyValPair *)calloc(total, sizeof(KeyValPair));
    if (!pairs)
    {
      free(entries);
      entries = NULL;
      total = 0;
    }
    else
    {
      size_t i = 0;
      yyjson_obj_iter iter;
      yyjson_obj_iter_init(data_root, &iter);
      yyjson_val *k;
      while ((k = yyjson_obj_iter_next(&iter)) && i < total)
      {
        pairs[i].key = yyjson_get_str(k);
        pairs[i].val = yyjson_obj_iter_get_val(k);
        i++;
      }
      qsort(pairs, total, sizeof(KeyValPair), cmp_keyval_pair);
      for (size_t idx = 0; idx < total; idx++)
      {
        entries[idx].val = (void *)pairs[idx].val;
        entries[idx].index = idx;
      }
      free(pairs);
    }
  }
  else
  {
    // Valor único
    total = 1;
    entries = (SortEntry *)calloc(1, sizeof(SortEntry));
    entries[0].val = (void *)data_root;
    entries[0].index = 0;
  }

  // Aplica filtros
  SortEntry *filtered = NULL;
  size_t filtered_count = 0;

  if (spec.filter_count > 0)
  {
    filtered = (SortEntry *)calloc(total, sizeof(SortEntry));
    for (size_t i = 0; i < total; i++)
    {
      bool match = true;
      for (int f = 0; f < spec.filter_count && match; f++)
      {
        if (!spec.filters[f].valid)
          continue;
        match = evaluate_filter((yyjson_val *)entries[i].val, &spec.filters[f]);
      }
      if (match)
      {
        filtered[filtered_count].val = entries[i].val;
        filtered[filtered_count].index = entries[i].index;
        filtered_count++;
      }
    }
  }
  else
  {
    filtered = entries;
    filtered_count = total;
  }

  // Ordena (contexto injetado — sem estado global)
  if (spec.order_count > 0 && filtered_count > 1)
  {
    he_qsort(filtered, filtered_count, sizeof(SortEntry), compare_entries, spec.orders);
  }

  // Aplica skip/take
  size_t start = spec.skip < filtered_count ? spec.skip : filtered_count;
  size_t end = (spec.take < filtered_count - start) ? (start + spec.take) : filtered_count;

  // Monta resultado como array JSON (com projeção include/exclude)
  yyjson_mut_doc *result_doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *result_arr = yyjson_mut_arr(result_doc);
  yyjson_mut_doc_set_root(result_doc, result_arr);

  for (size_t i = start; i < end; i++)
  {
    if (filtered[i].val)
    {
      yyjson_mut_val *copy = he_project_value(result_doc, (yyjson_val *)filtered[i].val, &spec.proj);
      if (copy)
        yyjson_mut_arr_append(result_arr, copy);
    }
  }

  char *json_out = yyjson_mut_write(result_doc, 0, NULL);

  // Limpa
  yyjson_mut_doc_free(result_doc);
  if (filtered != entries)
    free(filtered);
  free(entries);
  yyjson_doc_free(data_doc);
  yyjson_doc_free(query_doc);

  if (!json_out)
  {
    if (err)
      *err = sqlite3_mprintf("Failed to serialize query result");
    return NULL;
  }

  // Normaliza para alocação sqlite3 (o controller usa sqlite3_free
  // como destrutor no sqlite3_result_text).
  char *out = sqlite3_mprintf("%s", json_out);
  free(json_out);
  return out;
}
