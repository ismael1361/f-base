/*
 * he_services.c — Camada de USE-CASES do motor SQLite (Clean Architecture).
 *
 * Implementa as operações de negócio: set, update, extract, query,
 * export/import CSV. Orquestra as camadas repo (persistência), mapper
 * (JSON↔nodes), query (filtros/ordenação) e csv (parser).
 *
 * Todos os retornos de sucesso são alocados com sqlite3_malloc — o
 * controller usa sqlite3_free como destrutor. Erros são reportados via
 * *err (mensagem alocada com sqlite3_malloc).
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
#include "he_repo.h"
#include "he_mapper.h"
#include "he_csv.h"
#include "he_query.h"
#include "he_services.h"
#include "he_stmt_cache.h"

/* Forward declarations (fast path de update de folhas) */
static bool he_json_is_leaf_object(yyjson_val *obj);
static char *he_update_json_leaf_fast(HeStmtCache *cache, const char *doc_id,
                                      const char *root_path, yyjson_val *update_root,
                                      size_t max_inline_size, char **err);

/* ===========================================================================
 * SET_JSON (JSON -> Nodes, replace completo)
 * ===========================================================================
 */

/// set_json(doc_id, json_text, max_inline_size) — Substitui completamente
/// o documento. Se json_text for JSON null, remove o documento.
///
/// DEDUP (escrita idempotente): se o conteúdo + max_inline_size forem
/// idênticos ao último write do path (tabela doc_hashes, conferida contra
/// a revision atual do nó raiz), a escrita é PULADA e a revision existente
/// é devolvida — evita delete + re-flatten de milhares de nós para dados
/// inalterados. update_json/import_csv atualizam o hash para manter o dedup
/// sempre correto.
char *he_set_json(HeStmtCache *cache, const char *doc_id, const char *json_str,
                  size_t max_inline_size, char **err)
{
  if (err)
    *err = NULL;

  // Normaliza root_path
  char root_path[1024];
  normalize_path(doc_id, root_path, sizeof(root_path));

  // DEDUP ANTES DO PARSE: se o conteúdo for byte-idêntico ao último write,
  // não há necessidade de yyjson_read (que custa ~3.5ms para um doc de
  // 0.5MB) — o SHA-256 (~1ms) + lookup de hash é suficiente. O hash usa a
  // string bruta (mesmo critério do original), então um JSON inválido nunca
  // dedupa com um conteúdo previamente validado (colisão SHA-256 prática
  // impossível).
  bool is_null_doc = false;
  {
    // Detecta o literal "null" textualmente (equivalente ao root null do
    // yyjson) para pular hash/dedup — "null" deleta o doc, sem hash.
    const char *p = json_str;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
      p++;
    if (p[0] == 'n' && p[1] == 'u' && p[2] == 'l' && p[3] == 'l' &&
        (p[4] == '\0' || p[4] == ' ' || p[4] == '\t' || p[4] == '\n' || p[4] == '\r'))
      is_null_doc = true;
  }

  char content_hash[65];
  bool has_content_hash = false;
  if (!is_null_doc)
  {
    char hash_suffix[40];
    int suffix_len = snprintf(hash_suffix, sizeof(hash_suffix),
                              "\x01%zu", max_inline_size);
    const char *segs[2] = {json_str, hash_suffix};
    size_t lens[2] = {strlen(json_str), (size_t)suffix_len};
    sha256_hex_segments(segs, lens, 2, content_hash);
    has_content_hash = true;

    // DEDUP: conteúdo idêntico → retorna a revision existente (sem write)
    char stored_hash[65];
    char stored_rev[UUID_STR_LEN];
    if (he_repo_get_doc_hash(cache, root_path, stored_hash, stored_rev) &&
        strcmp(stored_hash, content_hash) == 0)
    {
      return sqlite3_mprintf("%s", stored_rev);
    }
  }

  // Parse JSON (só quando é um write real — o dedup-skip não paga mais
  // o yyjson_read de um doc grande)
  yyjson_doc *doc = yyjson_read(json_str, strlen(json_str), 0);
  if (!doc)
  {
    if (err)
      *err = sqlite3_mprintf("Invalid JSON");
    return NULL;
  }
  yyjson_val *root = yyjson_doc_get_root(doc);

  // Gera revision UUID (alocado — caller/destrutor usa sqlite3_free)
  char rev_buf[UUID_STR_LEN];
  generate_uuid_v4(rev_buf, sizeof(rev_buf));
  char *revision = sqlite3_mprintf("%s", rev_buf);

  // === Transação (aninhável via transaction() externo) ===
  int owned_tx = he_repo_begin_write_tx(cache);

  // Deleta subtree existente
  he_repo_delete_subtree(cache, root_path);

  if (yyjson_is_null(root))
  {
    he_repo_clear_doc_hash(cache, root_path);
    he_repo_end_write_tx(cache, owned_tx);
    yyjson_doc_free(doc);
    return revision;
  }

  // Garante paths intermediários
  he_repo_ensure_intermediate_paths(cache, doc_id);

  // Calcula revision_nr
  int rev_nr = he_repo_get_revision_nr(cache, root_path) + 1;

  // Insere container root
  he_repo_insert_node_rev(cache, root_path, TYPE_OBJECT, "{}", revision, rev_nr, 0);

  // Achata o valor
  he_mapper_flatten_value(cache, root, root_path, revision, rev_nr, max_inline_size);

  // Registra hash para o próximo set idempotente
  if (has_content_hash)
    he_repo_set_doc_hash(cache, root_path, content_hash, revision);

  he_repo_end_write_tx(cache, owned_tx);

  yyjson_doc_free(doc);

  return revision;
}

/* ===========================================================================
 * UPDATE_JSON (JSON -> Nodes, merge)
 * ===========================================================================
 */

/// update_json(doc_id, json_text, max_inline_size) — deep merge do
/// json_text no documento existente (preserva chaves não mencionadas).
///
/// FAST PATH: quando o update contém apenas chaves de PRIMITIVOS
/// (number/bool/string) no primeiro nível, aplica UPDATE direto nos nós
/// existentes — evita o custo de read-modify-write (extract + merge +
/// serialize + delete + re-flatten) e não reescreve o documento inteiro.
char *he_update_json(HeStmtCache *cache, const char *doc_id, const char *json_str,
                     size_t max_inline_size, char **err)
{
  if (err)
    *err = NULL;

  // Normaliza root_path e detecta wildcards
  bool has_wildcard = path_has_wildcard(doc_id);
  char root_path[1024];
  // Para wildcard, root_path = parte fixa antes do primeiro wildcard
  if (has_wildcard)
  {
    wildcard_fixed_prefix(doc_id, root_path, sizeof(root_path));
  }
  else
  {
    normalize_path(doc_id, root_path, sizeof(root_path));
  }

  // Parse o JSON de update (valida + usado em ambos os caminhos)
  yyjson_doc *update_doc = yyjson_read(json_str, strlen(json_str), 0);
  if (!update_doc)
  {
    if (err)
      *err = sqlite3_mprintf("Invalid JSON in update");
    return NULL;
  }
  yyjson_val *update_root = yyjson_doc_get_root(update_doc);

  if (!yyjson_is_obj(update_root) && !yyjson_is_null(update_root))
  {
    yyjson_doc_free(update_doc);
    if (err)
      *err = sqlite3_mprintf("UPDATE only supports JSON objects or null");
    return NULL;
  }

  // ── FAST PATH: update de folhas (sem wildcard, só primitivos) ──
  // Aplica o merge preservando os demais nós — sem reescrever o doc.
  if (!has_wildcard && yyjson_is_obj(update_root) &&
      he_json_is_leaf_object(update_root))
  {
    char *rev = he_update_json_leaf_fast(cache, doc_id, root_path,
                                         update_root, max_inline_size, err);
    yyjson_doc_free(update_doc);
    return rev;
  }

  // Se update é null, remove (via delete_range do cache)
  if (yyjson_is_null(update_root))
  {
    yyjson_doc_free(update_doc);
    char upper_del[1024];
    strncpy(upper_del, root_path, sizeof(upper_del) - 1);
    upper_del[sizeof(upper_del) - 1] = '\0';
    size_t dlen = strlen(upper_del);
    if (dlen > 0)
      upper_del[dlen - 1]++;

    sqlite3_stmt *del_stmt = cache->delete_range;
    sqlite3_reset(del_stmt);
    sqlite3_bind_text(del_stmt, 1, root_path, -1, SQLITE_STATIC);
    sqlite3_bind_text(del_stmt, 2, upper_del, -1, SQLITE_STATIC);
    sqlite3_step(del_stmt);

    // Documento removido → invalida o dedup
    he_repo_clear_doc_hash(cache, root_path);

    char rev[UUID_STR_LEN];
    generate_uuid_v4(rev, sizeof(rev));
    return sqlite3_mprintf("%s", rev);
  }

  // ── CAMINHO COMPLETO (merge estrutural) ──
  // Carrega o JSON atual via extract_json
  char extract_path[1024];
  path_without_trailing_slash(root_path, extract_path, sizeof(extract_path));

  // Extrai direto no C (he_extract_json) — o wrapper SQL interno
  // (SELECT COALESCE(extract_json(...),'null')) copiava o JSON inteiro
  // uma 2ª vez (column text → mprintf) além do dispatch SQL. NULL aqui
  // significa "documento não existe" (mesma semântica do 'null' do wrapper).
  char *raw = he_extract_json(cache, extract_path, err);
  if (err && *err)
  {
    yyjson_doc_free(update_doc);
    return NULL;
  }

  yyjson_doc *current_doc = NULL;
  if (raw && strcmp(raw, "null") != 0)
  {
    current_doc = yyjson_read(raw, strlen(raw), 0);
  }
  sqlite3_free(raw);

  yyjson_mut_doc *merge_doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *merged = NULL;

  if (has_wildcard)
  {
    // =============================================================
    // WILDCARD UPDATE: aplica merge a CADA filho direto do container
    // =============================================================
    // current_doc é o objeto pai: {"alice":{...},"bob":{...}}
    // Para cada filho, faz deep_merge(child_val, update_root)
    if (current_doc)
    {
      yyjson_val *parent_root = yyjson_doc_get_root(current_doc);
      if (yyjson_is_obj(parent_root))
      {
        merged = yyjson_mut_obj(merge_doc);
        yyjson_obj_iter iter;
        yyjson_obj_iter_init(parent_root, &iter);
        yyjson_val *k, *v;
        while ((k = yyjson_obj_iter_next(&iter)))
        {
          v = yyjson_obj_iter_get_val(k);
          const char *key = yyjson_get_str(k);
          yyjson_mut_val *child_merged = he_mapper_deep_merge(merge_doc, v, update_root);
          if (child_merged)
          {
            yyjson_mut_obj_add(merged, yyjson_mut_strcpy(merge_doc, key), child_merged);
          }
        }
      }
      yyjson_mut_doc_set_root(merge_doc, merged ? merged : yyjson_mut_obj(merge_doc));
    }
    else
    {
      // Pai não existe → não há o que atualizar
      yyjson_mut_doc_free(merge_doc);
      yyjson_doc_free(update_doc);
      return NULL; // resultado NULL (sem erro)
    }
  }
  else
  {
    // =============================================================
    // SINGLE UPDATE: merge normal em um documento específico
    // =============================================================
    if (current_doc)
    {
      yyjson_val *current_root = yyjson_doc_get_root(current_doc);
      merged = he_mapper_deep_merge(merge_doc, current_root, update_root);
    }
    else
    {
      merged = yyjson_val_mut_copy(merge_doc, update_root);
    }
    yyjson_mut_doc_set_root(merge_doc, merged ? merged : yyjson_mut_obj(merge_doc));
  }

  char *merged_json = yyjson_mut_write(merge_doc, 0, NULL);
  if (!merged_json)
  {
    yyjson_mut_doc_free(merge_doc);
    if (current_doc)
      yyjson_doc_free(current_doc);
    yyjson_doc_free(update_doc);
    if (err)
      *err = sqlite3_mprintf("Failed to serialize merged JSON");
    return NULL;
  }

  // Hash do conteúdo final (permite dedup correto no próximo set)
  char merged_hash[65];
  {
    char hash_suffix[40];
    int suffix_len = snprintf(hash_suffix, sizeof(hash_suffix),
                              "\x01%zu", max_inline_size);
    const char *segs[2] = {merged_json, hash_suffix};
    size_t lens[2] = {strlen(merged_json), (size_t)suffix_len};
    sha256_hex_segments(segs, lens, 2, merged_hash);
  }

  yyjson_mut_doc_free(merge_doc);
  if (current_doc)
    yyjson_doc_free(current_doc);
  yyjson_doc_free(update_doc);

  // Aplica SET com o JSON merged
  yyjson_doc *final_doc = yyjson_read(merged_json, strlen(merged_json), 0);
  free(merged_json);
  if (!final_doc)
  {
    if (err)
      *err = sqlite3_mprintf("Failed to parse merged JSON");
    return NULL;
  }
  yyjson_val *final_root = yyjson_doc_get_root(final_doc);

  char rev_buf[UUID_STR_LEN];
  generate_uuid_v4(rev_buf, sizeof(rev_buf));
  char *revision = sqlite3_mprintf("%s", rev_buf);

  int owned_tx2 = he_repo_begin_write_tx(cache);

  he_repo_delete_subtree(cache, root_path);
  // Para wildcard, doc_id é o path original (ex: "/people/*") → ensure_intermediate_paths
  // precisa do doc_id sem wildcard para criar os paths intermediários corretos
  char clean_doc_id[1024];
  path_without_trailing_slash(root_path, clean_doc_id, sizeof(clean_doc_id));
  // Remove leading / para ensure_intermediate_paths
  const char *id_for_ensure = clean_doc_id;
  if (id_for_ensure[0] == '/')
    id_for_ensure++;
  he_repo_ensure_intermediate_paths(cache, id_for_ensure);

  int rev_nr = he_repo_get_revision_nr(cache, root_path) + 1;
  he_repo_insert_node_rev(cache, root_path, TYPE_OBJECT, "{}", revision, rev_nr, 0);
  he_mapper_flatten_value(cache, final_root, root_path, revision, rev_nr, max_inline_size);

  // Registra hash do conteúdo merged (próximo set idêntico → dedup)
  he_repo_set_doc_hash(cache, root_path, merged_hash, revision);

  he_repo_end_write_tx(cache, owned_tx2);

  yyjson_doc_free(final_doc);

  return revision;
}

/* ===========================================================================
 * EXTRACT_JSON (Nodes -> JSON) com tipos corrigidos + inline
 * ===========================================================================
 */

/// Verifica se um objeto JSON contém APENAS valores primitivos no primeiro
/// nível (number/boolean/string/null) — sem objetos/arrays aninhados.
static bool he_json_is_leaf_object(yyjson_val *obj)
{
  if (!yyjson_is_obj(obj))
    return false;
  yyjson_obj_iter iter;
  yyjson_obj_iter_init(obj, &iter);
  yyjson_val *k, *v;
  while ((k = yyjson_obj_iter_next(&iter)))
  {
    v = yyjson_obj_iter_get_val(k);
    if (yyjson_is_obj(v) || yyjson_is_arr(v))
      return false;
  }
  return true;
}

/// FAST PATH de update: aplica o merge de PRIMITIVOS diretamente nos nós,
/// sem reescrever o documento inteiro. Cada chave do update vira um UPDATE
/// (ou INSERT OR REPLACE se o nó ainda não existe) no nó filho. Chaves com
/// valor null deletam o nó. Preserva todos os demais nós do documento.
static char *he_update_json_leaf_fast(HeStmtCache *cache, const char *doc_id,
                                      const char *root_path, yyjson_val *update_root,
                                      size_t max_inline_size, char **err)
{
  (void)doc_id;
  (void)max_inline_size;

  char root_no_slash[1024];
  path_without_trailing_slash(root_path, root_no_slash, sizeof(root_no_slash));

  char rev_buf[UUID_STR_LEN];
  generate_uuid_v4(rev_buf, sizeof(rev_buf));
  char *revision = sqlite3_mprintf("%s", rev_buf);

  int owned_tx = he_repo_begin_write_tx(cache);

  // Conteúdo mudou parcialmente → invalida o dedup (próximo set reescreve)
  he_repo_clear_doc_hash(cache, root_path);

  // Garante paths intermediários (ex: "/users/" para "/users/100")
  const char *id_for_ensure = root_no_slash;
  if (id_for_ensure[0] == '/')
    id_for_ensure++;
  he_repo_ensure_intermediate_paths(cache, id_for_ensure);

  // Incrementa revision_nr do root (container)
  int rev_nr = he_repo_get_revision_nr(cache, root_path) + 1;
  he_repo_insert_node_rev(cache, root_path, TYPE_OBJECT, "{}", revision, rev_nr, 0);

  yyjson_obj_iter iter;
  yyjson_obj_iter_init(update_root, &iter);
  yyjson_val *k, *v;
  while ((k = yyjson_obj_iter_next(&iter)))
  {
    v = yyjson_obj_iter_get_val(k);
    const char *key = yyjson_get_str(k);

    char child_path[2048];
    snprintf(child_path, sizeof(child_path), "%s/%s", root_no_slash, key);

    if (yyjson_is_null(v))
    {
      // null → deleta o nó filho exato e seus descendentes.
      // delete_exact_subtree delimita o subtree por '/': remove o nó exato
      // (path = ?) e "/path/*" (range [path/, path0)) — NUNCA alcança
      // chaves irmãs que apenas compartilham prefixo de texto. O antigo
      // delete_subtree SEM o '/' final varria [path, path+1) e apagava
      // irmãs (ex.: "user" apagava "username" — bug de integridade).
      he_repo_delete_exact_subtree(cache, child_path);
      continue;
    }

    // Se o nó atual é container (armazenado com trailing slash), o mesmo
    // delete_exact_subtree apaga o subtree antes do insert — senão ficam
    // nós órfãos.
    {
      char container_path[2048];
      snprintf(container_path, sizeof(container_path), "%s/", child_path);
      int cur_type = he_repo_get_type(cache, container_path);
      if (cur_type == TYPE_OBJECT || cur_type == 2) /* 2 = LEGADO */
      {
        he_repo_delete_exact_subtree(cache, child_path);
      }
    }

    // Serializa o primitivo e faz INSERT OR REPLACE (cria ou atualiza).
    // Serialização DINÂMICA — o buffer fixo truncava strings > 1KB.
    int prim_type;
    char *text_buf = serialize_primitive_value_alloc(v, &prim_type);
    // keep_created=1: nó pode existir — preserva created via subquery
    he_repo_insert_node_rev(cache, child_path, prim_type, text_buf,
                            revision, rev_nr, 1);
    sqlite3_free(text_buf);
  }

  he_repo_end_write_tx(cache, owned_tx);

  return revision;
}

/// extract_json(prefix) — Reconstrói o JSON a partir dos nodes armazenados.
/// Suporta inline children (text_value do container) e tipos do MDE.
char *he_extract_json(HeStmtCache *cache, const char *input, char **err)
{
  if (err)
    *err = NULL;

  if (!input || input[0] == '\0')
    return NULL;

  // Normaliza prefixo
  char prefix[1024];
  normalize_path(input, prefix, sizeof(prefix));

  // Upper bound
  char upper[1024];
  strncpy(upper, prefix, sizeof(upper) - 1);
  upper[sizeof(upper) - 1] = '\0';
  size_t plen = strlen(upper);
  if (plen > 0)
    upper[plen - 1]++;

  sqlite3_stmt *stmt = cache->scan;
  sqlite3_reset(stmt);
  sqlite3_bind_text(stmt, 1, prefix, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, upper, -1, SQLITE_STATIC);

  yyjson_mut_doc *mut_doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = NULL;

  // Stack para reconstrução hierárquica
  typedef struct
  {
    const char *path;
    yyjson_mut_val *val;
  } StackNode;
  StackNode stack[2048];
  int stack_top = 0;

  int rows_count = 0;

  while (sqlite3_step(stmt) == SQLITE_ROW)
  {
    rows_count++;
    const char *path = (const char *)sqlite3_column_text(stmt, 0);
    int type = sqlite3_column_int(stmt, 1);
    const char *text_val = (const char *)sqlite3_column_text(stmt, 2);

    if (rows_count == 1)
    {
      // Primeira linha: nó raiz
      root = make_value_from_storage(mut_doc, type, text_val);
      if (!root)
      {
        root = yyjson_mut_obj(mut_doc);
      }
      yyjson_mut_doc_set_root(mut_doc, root);
      stack[stack_top++] = (StackNode){sqlite3_mprintf("%s", path), root};
      continue;
    }

    // === Linhas subsequentes: ===

    // 1. Encontra o pai na stack. As linhas vêm ordenadas por path
    //    (ORDER BY path), então o pai é o container mais recente da stack
    //    que seja prefixo do path atual. Desempilha (pop) os containers que
    //    já saíram do caminho — O(1) amortizado por linha em vez de O(stack)
    //    (o antigo loop varria a stack inteira sem nunca desempilhar,
    //    virando O(n²) em documentos com muitos containers).
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
      sqlite3_free((void *)stack[--stack_top].path); // POP (libera cópia)
    }
    yyjson_mut_val *parent = stack_top > 0 ? stack[stack_top - 1].val : root;

    // 2. Extrai a chave
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
      {
        key = path;
      }
    }
    else
    {
      key = path;
    }

    // 3. Cria o valor
    yyjson_mut_val *child = NULL;
    bool is_container = false;

    if (type == TYPE_OBJECT || type == 2) /* 2 = LEGADO (antigo TYPE_ARRAY) */
    {
      child = make_value_from_storage(mut_doc, type, text_val);
      is_container = true;
    }
    else
    {
      child = make_value_from_storage(mut_doc, type, text_val);
    }

    // 4. Adiciona ao pai — o modelo é OBJETO-ONLY: o pai nunca é array
    //    (dados legados type=2 são lidos como objeto), então adiciona por
    //    chave sempre.
    yyjson_mut_obj_add(parent,
                       yyjson_mut_strcpy(mut_doc, key), child);

    // 5. Se container, empilha
    if (is_container)
    {
      if (stack_top < 2048)
      {
        stack[stack_top++] = (StackNode){sqlite3_mprintf("%s", path), child};
      }
    }
  }

  sqlite3_reset(stmt);

  if (rows_count == 0)
  {
    yyjson_mut_doc_free(mut_doc);
    return NULL;
  }

  char *json_out = yyjson_mut_write(mut_doc, 0, NULL);
  for (int i = 0; i < stack_top; i++)
    sqlite3_free((void *)stack[i].path);
  yyjson_mut_doc_free(mut_doc);

  if (!json_out)
    return NULL;

  // Normaliza para alocação sqlite3 (destrutor uniforme no controller)
  char *out = sqlite3_mprintf("%s", json_out);
  free(json_out);
  return out;
}

/* ===========================================================================
 * QUERY_JSON (Query com filtros, ordenação, paginação)
 * ===========================================================================
 */

/// query_json(prefix, query_json) — delega ao query engine.
char *he_query_json(HeStmtCache *cache, const char *prefix, const char *query_str,
                    char **err)
{
  return he_query_execute(cache, prefix, query_str, err);
}

/* ===========================================================================
 * EXPORT_CSV (Nodes -> CSV)
 * ===========================================================================
 */

/// export_csv(prefix) → CSV text com todos os nós descendentes do prefixo.
char *he_export_csv(HeStmtCache *cache, const char *input, char **err)
{
  if (err)
    *err = NULL;

  if (!input || input[0] == '\0')
    return sqlite3_mprintf("%s", CSV_HEADER);

  char prefix[1024];
  normalize_path(input, prefix, sizeof(prefix));

  char upper[1024];
  snprintf(upper, sizeof(upper), "%s", prefix);
  size_t plen = strlen(upper);
  if (plen > 0)
    upper[plen - 1]++;

  sqlite3_stmt *stmt = cache->scan;
  sqlite3_reset(stmt);
  sqlite3_bind_text(stmt, 1, prefix, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, upper, -1, SQLITE_STATIC);

  sqlite3_str *out = sqlite3_str_new(cache->db);
  if (!out)
  {
    if (err)
      *err = sqlite3_mprintf("OOM in export_csv");
    return NULL;
  }
  sqlite3_str_append(out, CSV_HEADER, (int)strlen(CSV_HEADER));

  while (sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *path = (const char *)sqlite3_column_text(stmt, 0);
    int type = sqlite3_column_int(stmt, 1);
    const char *text_val = (const char *)sqlite3_column_text(stmt, 2);

    // Escape inline direto no sqlite3_str — zero alocação por linha
    // (o antigo he_csv_escape alocava 2 buffers por linha: epath+etext).
    he_csv_escape_append(out, path);
    // type ∈ [0,9] (TYPE_EMPTY..TYPE_REFERENCE) — append direto do dígito
    // (o antigo sqlite3_str_appendf rodava printf por linha do CSV)
    sqlite3_str_append(out, ",", 1);
    sqlite3_str_appendchar(out, 1, (char)('0' + type));
    sqlite3_str_append(out, ",", 1);
    he_csv_escape_append(out, text_val);
    sqlite3_str_append(out, "\n", 1);
  }
  sqlite3_reset(stmt);

  char *csv_out = sqlite3_str_finish(out);
  if (!csv_out)
    return sqlite3_mprintf("%s", CSV_HEADER);
  return csv_out;
}

/* ===========================================================================
 * IMPORT_CSV (CSV -> Nodes) — o trabalho pesado em C
 * ===========================================================================
 */

/// import_csv(prefix, csv_text, max_inline_size, options_json) → revision UUID
char *he_import_csv(HeStmtCache *cache, const char *prefix_input,
                    const char *csv_text, size_t max_inline_size,
                    const char *options_json, char **err)
{
  if (err)
    *err = NULL;

  // ── 1. Parseia CSV ──
  CsvRow *rows = NULL;
  int row_count = he_csv_parse(csv_text, &rows);

  if (row_count < 0)
  {
    if (err)
      *err = sqlite3_mprintf("Failed to parse CSV");
    return NULL;
  }

  // ── 1b. Filtra linhas por include/exclude de path (padrões wildcard) ──
  if (options_json && options_json[0] != '\0')
  {
    HeProjection proj;
    memset(&proj, 0, sizeof(proj));
    yyjson_doc *opt_doc = yyjson_read(options_json, strlen(options_json), 0);
    if (opt_doc)
    {
      he_projection_parse(yyjson_doc_get_root(opt_doc), &proj);
      yyjson_doc_free(opt_doc);
    }

    if (proj.include_count > 0 || proj.exclude_count > 0)
    {
      int kept = 0;
      for (int i = 0; i < row_count; i++)
      {
        bool inc_ok = proj.include_count == 0;
        if (!inc_ok)
        {
          for (int p = 0; p < proj.include_count && !inc_ok; p++)
          {
            WildcardPattern pat;
            wildcard_parse(proj.include[p], &pat);
            if (wildcard_matches(rows[i].path, &pat))
              inc_ok = true;
          }
        }
        bool exc_ok = true;
        for (int p = 0; p < proj.exclude_count && exc_ok; p++)
        {
          WildcardPattern pat;
          wildcard_parse(proj.exclude[p], &pat);
          if (wildcard_matches(rows[i].path, &pat))
            exc_ok = false;
        }

        if (inc_ok && exc_ok)
        {
          if (kept != i)
            rows[kept] = rows[i];
          kept++;
        }
        else
        {
          sqlite3_free(rows[i].path);
          sqlite3_free(rows[i].text_value);
        }
      }
      row_count = kept;
    }
  }

  if (row_count == 0)
  {
    he_csv_free_rows(rows, 0);
    char rev[UUID_STR_LEN];
    generate_uuid_v4(rev, sizeof(rev));
    return sqlite3_mprintf("%s", rev);
  }

  // ── 2. Normaliza o prefixo de destino e identifica o root da árvore ──
  // O CSV carrega paths absolutos; o root da árvore é a 1ª linha. Os nós
  // são remapeados para sob o prefixo informado (paridade com o antigo
  // fluxo tree→flatten: as chaves finais viram filhos do root_path).
  char root_path[1024];
  normalize_path(prefix_input, root_path, sizeof(root_path));
  const char *base = rows[0].path;
  size_t base_len = strlen(base);

  // Hash do conteúdo importado (mantém o dedup de set correto após import).
  // Só o hash precisa da árvore canônica — o write é feito por inserção
  // direta das linhas (sem re-parse + flatten: -40% do custo total).
  char imported_hash[65];
  {
    yyjson_mut_doc *tree_doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *tree_root = he_csv_rows_to_tree(tree_doc, rows, row_count);
    if (!tree_root)
    {
      he_csv_free_rows(rows, row_count);
      yyjson_mut_doc_free(tree_doc);
      if (err)
        *err = sqlite3_mprintf("Failed to build tree from CSV");
      return NULL;
    }
    char *json_str = yyjson_mut_write(tree_doc, 0, NULL);
    yyjson_mut_doc_free(tree_doc);
    if (!json_str)
    {
      he_csv_free_rows(rows, row_count);
      if (err)
        *err = sqlite3_mprintf("Failed to serialize tree");
      return NULL;
    }
    char hash_suffix[40];
    int suffix_len = snprintf(hash_suffix, sizeof(hash_suffix),
                              "\x01%zu", max_inline_size);
    const char *segs[2] = {json_str, hash_suffix};
    size_t lens[2] = {strlen(json_str), (size_t)suffix_len};
    sha256_hex_segments(segs, lens, 2, imported_hash);
    free(json_str);
  }

  // ── 3. Escreve no banco (delete + inserção direta das linhas) ──
  char rev_buf[UUID_STR_LEN];
  generate_uuid_v4(rev_buf, sizeof(rev_buf));
  char *revision = sqlite3_mprintf("%s", rev_buf);

  int owned_tx3 = he_repo_begin_write_tx(cache);
  he_repo_delete_subtree(cache, root_path);

  char clean_id[1024];
  path_without_trailing_slash(root_path, clean_id, sizeof(clean_id));
  const char *id_for_ensure = clean_id;
  if (id_for_ensure[0] == '/')
    id_for_ensure++;
  he_repo_ensure_intermediate_paths(cache, id_for_ensure);

  int rev_nr = he_repo_get_revision_nr(cache, root_path) + 1;

  he_repo_insert_node_rev(cache, root_path, TYPE_OBJECT, NULL,
                          revision, rev_nr, 0);

  for (int i = 0; i < row_count; i++)
  {
    const char *lp = rows[i].path;
    char newpath[2048];

    // base termina com '/' (ex: "/imported/") — strncmp já garante o limite
    // de segmento: "/importedX" difere no char base_len-1. O sufixo
    // (lp + base_len) é o restante do path sob o root da árvore.
    if (strncmp(lp, base, base_len) == 0)
    {
      snprintf(newpath, sizeof(newpath), "%s%s", root_path, lp + base_len);
    }
    else
    {
      // Linha fora do root (CSV malformado): o antigo flatten tratava a
      // chave final como filha direta do root da árvore.
      const char *last = strrchr(lp, '/');
      snprintf(newpath, sizeof(newpath), "%s%s", root_path, last ? last + 1 : lp);
    }

    // A linha raiz (path == root) já foi inserida acima
    if (strcmp(newpath, root_path) == 0)
      continue;

    he_repo_insert_node_rev(cache, newpath, rows[i].type, rows[i].text_value,
                            revision, rev_nr, 0);
  }

  he_repo_set_doc_hash(cache, root_path, imported_hash, revision);

  he_repo_end_write_tx(cache, owned_tx3);

  he_csv_free_rows(rows, row_count);

  return revision;
}
