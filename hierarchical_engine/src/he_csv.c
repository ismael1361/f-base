/*
 * he_csv.c — Camada CSV do motor SQLite (Clean Architecture).
 *
 * Parser/serializador RFC 4180: escape, split de linhas lógicas,
 * parse de linhas, e reconstrução da árvore JSON a partir das linhas.
 * Sem execução de SQL.
 */

#include <sqlite3ext.h>
/* sqlite3_api é definida em he_extension.c (SQLITE_EXTENSION_INIT1/INIT2).
 * As macros do sqlite3ext.h usam esta variável — declarar extern aqui. */
extern const sqlite3_api_routines *sqlite3_api;

#include <yyjson.h>
#include <string.h>
#include <stdlib.h>

#include "he_types.h"
#include "he_utils.h"
#include "he_csv.h"

/* ===========================================================================
 * ESCAPE (RFC 4180)
 * ===========================================================================
 */

/// Escapa um valor para CSV (RFC 4180).
/// Retorna string alocada com sqlite3_malloc — caller deve sqlite3_free().
char *he_csv_escape(const char *value)
{
  if (!value)
    return sqlite3_mprintf("");

  // Se não precisa de escape, retorna cópia simples
  if (!strpbrk(value, ",\"\n\r"))
    return sqlite3_mprintf("%s", value);

  // Precisa de quoting com escaping de "
  sqlite3_str *s = sqlite3_str_new(NULL);
  if (!s)
    return sqlite3_mprintf("");

  sqlite3_str_append(s, "\"", 1);
  while (*value)
  {
    if (*value == '"')
      sqlite3_str_append(s, "\"\"", 2);
    else
      sqlite3_str_append(s, value, 1);
    value++;
  }
  sqlite3_str_append(s, "\"", 1);
  return sqlite3_str_finish(s);
}

/// Append de um valor CSV diretamente em um sqlite3_str (RFC 4180),
/// sem alocação intermediária — usado no hot path do export_csv (uma
/// alocação por linha era o custo dominante: 2 × sqlite3_malloc + cópia
/// para cada uma das N linhas do documento).
void he_csv_escape_append(sqlite3_str *out, const char *value)
{
  if (!value)
    return;

  // Fast path: sem caracteres especiais → append direto (1 cópia)
  if (!strpbrk(value, ",\"\n\r"))
  {
    sqlite3_str_append(out, value, (int)strlen(value));
    return;
  }

  // RFC 4180: quoting com escaping de aspas
  sqlite3_str_append(out, "\"", 1);
  while (*value)
  {
    if (*value == '"')
      sqlite3_str_append(out, "\"\"", 2);
    else
      sqlite3_str_append(out, value, 1);
    value++;
  }
  sqlite3_str_append(out, "\"", 1);
}

/* ===========================================================================
 * PARSE (RFC 4180)
 * ===========================================================================
 */

/// Comparador para qsort — ordena por path
static int compare_csv_rows(const void *a, const void *b)
{
  const CsvRow *ra = (const CsvRow *)a;
  const CsvRow *rb = (const CsvRow *)b;
  return strcmp(ra->path, rb->path);
}

/// Parseia uma linha CSV respeitando RFC 4180 (aspas, escaping de "").
/// Retorna o número de campos ou -1 em erro.
/// out_fields será alocado com sqlite3_malloc — caller deve free cada
/// elemento e o array.
///
/// Perf: copia CHUNKS inteiros entre caracteres especiais (1 append por
/// segmento) em vez de 1 append por caractere — o antigo custava ~180ms
/// para 50k linhas (~10MB/s de parse) e dominava o import CSV.
static int parse_csv_line(const char *line, char ***out_fields)
{
  int cap = 8, count = 0;
  *out_fields = (char **)sqlite3_malloc64((size_t)cap * sizeof(char *));
  if (!*out_fields)
    return -1;

  const char *p = line;
  sqlite3_str *buf = sqlite3_str_new(NULL);
  if (!buf)
  {
    sqlite3_free(*out_fields);
    *out_fields = NULL;
    return -1;
  }
  int in_quotes = 0;

  while (*p)
  {
    if (*p == '"')
    {
      if (in_quotes && *(p + 1) == '"')
      {
        sqlite3_str_append(buf, "\"", 1);
        p += 2;
        continue;
      }
      in_quotes = !in_quotes;
      p++;
    }
    else if (*p == ',' && !in_quotes)
    {
      char *field = sqlite3_str_finish(buf);
      buf = sqlite3_str_new(NULL);
      if (!buf)
      {
        sqlite3_free(field);
        for (int i = 0; i < count; i++)
          sqlite3_free((*out_fields)[i]);
        sqlite3_free(*out_fields);
        *out_fields = NULL;
        return -1;
      }
      if (count >= cap)
      {
        cap *= 2;
        *out_fields = (char **)sqlite3_realloc64(*out_fields,
                                                 (size_t)cap * sizeof(char *));
        if (!*out_fields)
        {
          sqlite3_free(field);
          return -1;
        }
      }
      (*out_fields)[count++] = field ? field : sqlite3_mprintf("");
      p++;
    }
    else
    {
      // Fast path: copia um chunk inteiro até o próximo caractere especial
      // ('"' ou ',' fora de aspas). 1 append por segmento.
      const char *start = p;
      while (*p && *p != '"' && (*p != ',' || in_quotes))
        p++;
      sqlite3_str_append(buf, start, (int)(p - start));
    }
  }

  char *last = sqlite3_str_finish(buf);
  if (count >= cap)
  {
    cap *= 2;
    *out_fields = (char **)sqlite3_realloc64(*out_fields,
                                             (size_t)cap * sizeof(char *));
    if (!*out_fields)
    {
      sqlite3_free(last);
      return -1;
    }
  }
  (*out_fields)[count++] = last ? last : sqlite3_mprintf("");

  return count;
}

/// Conta linhas lógicas do CSV respeitando quoted newlines.
/// Retorna o número de linhas e aloca arrays de ponteiros e comprimentos.
static int split_csv_lines(const char *csv_text, const char ***out_lines, size_t **out_lengths)
{
  int cap = 128, count = 0;
  *out_lines = (const char **)sqlite3_malloc64((size_t)cap * sizeof(const char *));
  *out_lengths = (size_t *)sqlite3_malloc64((size_t)cap * sizeof(size_t));
  if (!*out_lines || !*out_lengths)
  {
    sqlite3_free(*out_lines);
    sqlite3_free(*out_lengths);
    *out_lines = NULL;
    *out_lengths = NULL;
    return -1;
  }

  const char *start = csv_text;
  int in_quotes = 0;

  for (const char *p = csv_text;; p++)
  {
    if (*p == '"')
      in_quotes = !in_quotes;

    if ((*p == '\n' && !in_quotes) || *p == '\0')
    {
      if (count >= cap)
      {
        cap *= 2;
        *out_lines = (const char **)sqlite3_realloc64(*out_lines,
                                                      (size_t)cap * sizeof(const char *));
        *out_lengths = (size_t *)sqlite3_realloc64(*out_lengths,
                                                   (size_t)cap * sizeof(size_t));
        if (!*out_lines || !*out_lengths)
          return -1;
      }
      (*out_lines)[count] = start;
      (*out_lengths)[count] = (size_t)(p - start);
      count++;
      start = p + 1;

      if (*p == '\0')
        break;
    }
  }

  return count;
}

/// Parseia um CSV completo em array de CsvRow.
/// Suporta campos com quebra de linha entre aspas (RFC 4180).
/// Retorna o número de linhas (excluindo header) ou -1 em erro.
int he_csv_parse(const char *csv_text, CsvRow **out_rows)
{
  *out_rows = NULL;

  // Divide em linhas lógicas (respeitando quoted newlines)
  const char **lines = NULL;
  size_t *line_lengths = NULL;
  int line_count = split_csv_lines(csv_text, &lines, &line_lengths);
  if (line_count < 0)
    return -1;
  if (line_count == 0)
  {
    sqlite3_free(lines);
    sqlite3_free(line_lengths);
    return 0;
  }

  int cap = line_count > 1 ? line_count - 1 : 64;
  int count = 0;
  *out_rows = (CsvRow *)sqlite3_malloc64((size_t)cap * sizeof(CsvRow));
  if (!*out_rows)
  {
    sqlite3_free(lines);
    sqlite3_free(line_lengths);
    return -1;
  }

  for (int i = 1; i < line_count; i++)
  {
    const char *line = lines[i];
    size_t llen = line_lengths[i];
    // Apenas remove \r do final, NÃO remove espaços (são significativos em CSV)
    while (llen > 0 && line[llen - 1] == '\r')
      llen--;

    if (llen == 0)
      continue;

    // Cria cópia sem \r no final para parse_csv_line
    char *line_copy = (char *)sqlite3_malloc64(llen + 1);
    if (!line_copy)
    {
      for (int j = 0; j < count; j++)
      {
        sqlite3_free((*out_rows)[j].path);
        sqlite3_free((*out_rows)[j].text_value);
      }
      sqlite3_free(*out_rows);
      *out_rows = NULL;
      sqlite3_free(lines);
      sqlite3_free(line_lengths);
      return -1;
    }
    memcpy(line_copy, line, llen);
    line_copy[llen] = '\0';

    char **fields = NULL;
    int nf = parse_csv_line(line_copy, &fields);
    sqlite3_free(line_copy);

    if (nf < 2)
    {
      if (fields)
      {
        for (int j = 0; j < nf; j++)
          sqlite3_free(fields[j]);
        sqlite3_free(fields);
      }
      continue;
    }

    if (count >= cap)
    {
      cap = cap * 2 + 1;
      *out_rows = (CsvRow *)sqlite3_realloc64(*out_rows,
                                              (size_t)cap * sizeof(CsvRow));
      if (!*out_rows)
      {
        for (int j = 0; j < count; j++)
        {
          sqlite3_free((*out_rows)[j].path);
          sqlite3_free((*out_rows)[j].text_value);
        }
        sqlite3_free(*out_rows);
        sqlite3_free(lines);
        sqlite3_free(line_lengths);
        return -1;
      }
    }

    CsvRow *row = &(*out_rows)[count];
    memset(row, 0, sizeof(CsvRow));
    row->path = sqlite3_mprintf("%s", fields[0]);
    row->type = atoi(fields[1]);
    if (nf > 2 && fields[2] && fields[2][0] != '\0')
      row->text_value = sqlite3_mprintf("%s", fields[2]);

    for (int j = 0; j < nf; j++)
      sqlite3_free(fields[j]);
    sqlite3_free(fields);

    count++;
  }

  sqlite3_free(lines);
  sqlite3_free(line_lengths);
  return count;
}

/// Libera array de CsvRow (count linhas)
void he_csv_free_rows(CsvRow *rows, int count)
{
  if (!rows)
    return;
  for (int i = 0; i < count; i++)
  {
    sqlite3_free(rows[i].path);
    sqlite3_free(rows[i].text_value);
  }
  sqlite3_free(rows);
}

/* ===========================================================================
 * ÁRVORE JSON (CSV → JSON)
 * ===========================================================================
 */

/// Reconstrói uma árvore JSON a partir das linhas CSV usando stack.
void *he_csv_rows_to_tree(void *doc_ptr, CsvRow *rows, int count)
{
  yyjson_mut_doc *doc = (yyjson_mut_doc *)doc_ptr;
  if (count == 0)
    return yyjson_mut_obj(doc);

  qsort(rows, (size_t)count, sizeof(CsvRow), compare_csv_rows);

  CsvRow *root_row = &rows[0];
  yyjson_mut_val *root = make_value_from_storage(doc, root_row->type,
                                                 root_row->text_value);
  if (!root)
    root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);

  typedef struct
  {
    const char *path;
    yyjson_mut_val *val;
  } StackNode;

  StackNode stack[2048];
  int stack_top = 0;
  stack[stack_top++] = (StackNode){root_row->path, root};

  for (int i = 1; i < count; i++)
  {
    CsvRow *row = &rows[i];
    const char *path = row->path;
    int type = row->type;
    const char *text_val = row->text_value;

    // 1. Encontra o pai na stack.
    // As linhas vêm ordenadas por path (qsort acima), então o pai é o
    // container mais recente da stack que seja prefixo do path atual.
    // Desempilha (pop) os containers que já saíram do caminho — O(1)
    // amortizado por linha em vez de varrer a stack inteira (o antigo
    // loop varria sem nunca desempilhar, virando O(n²) com muitos
    // containers: ~2.1s em 50k linhas vs ~15ms após o fix).
    while (stack_top > 0)
    {
      const char *sp = stack[stack_top - 1].path;
      size_t sp_len = strlen(sp);
      size_t slen = sp_len;
      while (slen > 0 && sp[slen - 1] == '/')
        slen--;
      if (strncmp(path, sp, slen) == 0 &&
          (path[slen] == '/' || path[slen] == '\0'))
      {
        break; // topo é ancestral → pai encontrado
      }
      stack_top--; // POP (sai do caminho atual)
    }
    yyjson_mut_val *parent = stack_top > 0 ? stack[stack_top - 1].val : root;

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
        memcpy(key_buf, last_slash + 1, (size_t)key_len);
        key_buf[key_len] = '\0';
      }
      else
      {
        key = path;
      }
    }
    else
    {
      key = path;
    }

    // 4. Adiciona ao pai — modelo OBJETO-ONLY: o pai nunca é array
    bool is_container = (type == TYPE_OBJECT || type == 2); /* 2 = LEGADO */
    yyjson_mut_val *child = make_value_from_storage(doc, type, text_val);

    yyjson_mut_obj_add(parent, yyjson_mut_strcpy(doc, key), child);

    // 5. Se container, empilha
    if (is_container && stack_top < 2048)
      stack[stack_top++] = (StackNode){path, child};
  }

  return root;
}
