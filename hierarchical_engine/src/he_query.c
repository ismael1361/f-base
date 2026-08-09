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
} HeQuerySpec;

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
        strncpy(flt->key, yyjson_get_str(key_v), sizeof(flt->key) - 1);
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
        strncpy(ord->key, yyjson_get_str(k), sizeof(ord->key) - 1);
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
}

/* ===========================================================================
 * AVALIAÇÃO DE FILTROS
 * ===========================================================================
 */

/// Avalia um filtro em um valor JSON
static bool evaluate_filter(yyjson_val *obj, QueryFilter *filter)
{
  // Navega pela chain de chaves (ex: "address.city")
  yyjson_val *val = obj;
  char key_copy[256];
  strncpy(key_copy, filter->key, sizeof(key_copy) - 1);
  key_copy[sizeof(key_copy) - 1] = '\0';

  char *part = strtok(key_copy, ".");
  while (part && val)
  {
    if (yyjson_is_obj(val))
      val = yyjson_obj_get(val, part);
    else
      val = NULL;
    part = strtok(NULL, ".");
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
    {
      // like: converte % para .* e faz regex (simplificado: fnmatch-style)
      size_t clen = strlen(filter->compare);
      size_t slen = strlen(str_val);
      if (clen == 0)
        return (slen == 0);
      // Verifica match simples com % wildcard
      if (filter->compare[0] == '%' && filter->compare[clen - 1] == '%')
      {
        // Extrai substring entre os % sem strndup
        size_t sub_len = clen - 2;
        char sub[256];
        if (sub_len < sizeof(sub))
        {
          memcpy(sub, filter->compare + 1, sub_len);
          sub[sub_len] = '\0';
          bool match = strstr(str_val, sub) != NULL;
          return match;
        }
        return false;
      }
      if (filter->compare[0] == '%')
      {
        const char *suffix = filter->compare + 1;
        size_t sl = strlen(str_val);
        size_t sul = strlen(suffix);
        return sl >= sul && strcmp(str_val + sl - sul, suffix) == 0;
      }
      if (filter->compare[clen - 1] == '%')
      {
        size_t prefix_len = clen - 1;
        if (prefix_len < 256)
        {
          char prefix[256];
          memcpy(prefix, filter->compare, prefix_len);
          prefix[prefix_len] = '\0';
          return strncmp(str_val, prefix, prefix_len) == 0;
        }
        return false;
      }
      return strcmp(str_val, filter->compare) == 0;
    }
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

/// Compara duas SortEntry de acordo com as QueryOrder.
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
    // Navega pela chain de chaves
    yyjson_val *va = (yyjson_val *)ea->val;
    yyjson_val *vb = (yyjson_val *)eb->val;
    char key_copy[256];
    strncpy(key_copy, orders[i].key, sizeof(key_copy) - 1);
    key_copy[sizeof(key_copy) - 1] = '\0';

    char *part = strtok(key_copy, ".");
    while (part && va && vb)
    {
      if (yyjson_is_obj(va))
        va = yyjson_obj_get(va, part);
      else
        va = NULL;
      if (yyjson_is_obj(vb))
        vb = yyjson_obj_get(vb, part);
      else
        vb = NULL;
      part = strtok(NULL, ".");
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

  // ── 10. Monta array de resultados ──
  for (size_t i = start; i < end; i++)
  {
    if (filtered[i].val)
    {
      yyjson_mut_val *copy = yyjson_val_mut_copy(result_doc, (yyjson_val *)filtered[i].val);
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
    // ─── WILDCARD MULTI-NÍVEL: busca paths que casam o padrão ───
    return do_query_wildcard(cache, prefix_input, query_str, err);
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
    total = yyjson_obj_size(data_root);
    entries = (SortEntry *)calloc(total, sizeof(SortEntry));
    size_t i = 0;
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(data_root, &iter);
    yyjson_val *k, *v;
    while ((k = yyjson_obj_iter_next(&iter)))
    {
      v = yyjson_obj_iter_get_val(k);
      if (i < total)
      {
        entries[i].val = (void *)v;
        entries[i].index = i;
        i++;
      }
    }
    total = i;
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

  // Monta resultado como array JSON
  yyjson_mut_doc *result_doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *result_arr = yyjson_mut_arr(result_doc);
  yyjson_mut_doc_set_root(result_doc, result_arr);

  for (size_t i = start; i < end; i++)
  {
    if (filtered[i].val)
    {
      yyjson_mut_val *copy = yyjson_val_mut_copy(result_doc, (yyjson_val *)filtered[i].val);
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
