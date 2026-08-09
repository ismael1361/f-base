/*
 * he_mapper.c — Camada de MAPPER do motor SQLite (Clean Architecture).
 *
 * Converte JSON ↔ nós da tabela `nodes`: flatten (JSON → nodes), merge
 * profundo (update parcial). A persistência é delegada ao he_repo.
 */

#include <sqlite3ext.h>
/* sqlite3_api é definida em he_extension.c (SQLITE_EXTENSION_INIT1/INIT2).
 * As macros do sqlite3ext.h usam esta variável — declarar extern aqui. */
extern const sqlite3_api_routines *sqlite3_api;

#include <yyjson.h>
#include <string.h>
#include <stdlib.h>

#include "he_types.h"
#include "he_repo.h"
#include "he_utils.h"
#include "he_mapper.h"
#include "he_stmt_cache.h"

/* ===========================================================================
 * FLATTEN_AND_INSERT (recursivo) — com inline optimization + chaves únicas
 * em coleções (modelo Firebase objeto-only): arrays do JSON de entrada viram
 * objetos com chaves UUID (push ID) — NUNCA arrays; o storage só conhece
 * objetos (TYPE_OBJECT).
 * ===========================================================================
 */

/// Achata um valor JSON em nós da tabela (SET mode).
/// Usa inline optimization: primitivos pequenos ficam no text_value do pai.
/// parent_path SEMPRE termina com '/' (ex: "/users/100/").
void he_mapper_flatten_value(HeStmtCache *cache, void *node_ptr,
                             const char *parent_path, const char *revision,
                             int revision_nr, size_t max_inline_size)
{
  yyjson_val *node = (yyjson_val *)node_ptr;
  char current_path[2048];

  if (yyjson_is_obj(node))
  {
    // Coleta inline children em um JSON temporário
    yyjson_mut_doc *inline_doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *inline_obj = yyjson_mut_obj(inline_doc);
    yyjson_mut_doc_set_root(inline_doc, inline_obj);
    bool has_inline = false;

    yyjson_obj_iter iter;
    yyjson_obj_iter_init(node, &iter);
    yyjson_val *key, *val;

    while ((key = yyjson_obj_iter_next(&iter)))
    {
      val = yyjson_obj_iter_get_val(key);
      const char *k = yyjson_get_str(key);

      snprintf(current_path, sizeof(current_path), "%s%s", parent_path, k);

      if (yyjson_is_null(val))
      {
        // JSON null → deleta a chave e descendentes
        char del_path[2048];
        snprintf(del_path, sizeof(del_path), "%s/", current_path);
        he_repo_delete_subtree(cache, del_path);
        continue;
      }

      if (value_fits_inline(val, max_inline_size))
      {
        // === INLINE: adiciona ao JSON do pai ===
        if (yyjson_is_str(val))
        {
          yyjson_mut_obj_add_str(inline_doc, inline_obj, k, yyjson_get_str(val));
        }
        else if (yyjson_is_int(val))
        {
          yyjson_mut_obj_add_int(inline_doc, inline_obj, k, yyjson_get_sint(val));
        }
        else if (yyjson_is_real(val))
        {
          yyjson_mut_obj_add_real(inline_doc, inline_obj, k, yyjson_get_real(val));
        }
        else if (yyjson_is_bool(val))
        {
          yyjson_mut_obj_add_bool(inline_doc, inline_obj, k, yyjson_get_bool(val));
        }
        else if (yyjson_is_null(val))
        {
          yyjson_mut_obj_add_null(inline_doc, inline_obj, k);
        }
        else if (yyjson_is_obj(val) || yyjson_is_arr(val))
        {
          // Container vazio → OBJETO vazio (modelo objeto-only: não existe
          // array no storage — array vazio também vira objeto vazio)
          yyjson_mut_obj_add_obj(inline_doc, inline_obj, k);
        }
        has_inline = true;
      }
      else
      {
        // === DEDICADO: vira nó filho ===
        if (yyjson_is_obj(val))
        {
          strncat(current_path, "/", sizeof(current_path) - strlen(current_path) - 1);
          // Container nasce com "{}" (text_value) — sem UPDATE posterior
          // quando não há inline children (Fase 3 perf).
          he_repo_insert_node_rev(cache, current_path, TYPE_OBJECT, "{}", revision, revision_nr, 0);
          he_mapper_flatten_value(cache, val, current_path, revision, revision_nr, max_inline_size);
        }
        else if (yyjson_is_arr(val))
        {
          strncat(current_path, "/", sizeof(current_path) - strlen(current_path) - 1);
          he_repo_insert_node_rev(cache, current_path, TYPE_OBJECT, "{}", revision, revision_nr, 0);
          he_mapper_flatten_array_as_object(cache, val, current_path, revision, revision_nr, max_inline_size);
        }
        else
        {
          // Primitivo dedicado (string longa) — serialização DINÂMICA
          // (o buffer fixo de 1KB truncava strings > ~1KB silenciosamente)
          int prim_type;
          char *text_buf = serialize_primitive_value_alloc(val, &prim_type);
          he_repo_insert_node_rev(cache, current_path, prim_type, text_buf,
                                  revision, revision_nr, 0);
          sqlite3_free(text_buf);
        }
      }
    }

    // Salva text_value do container com os inline children
    {
      const char *container_text = has_inline ? NULL : "{}";
      char *inline_json = NULL;

      if (has_inline)
      {
        inline_json = yyjson_mut_write(inline_doc, 0, NULL);
        if (inline_json)
          container_text = inline_json;
        else
          container_text = "{}";
      }

      // Fase 3 (perf): UPDATE só quando há inline children. Sem inline, o
      // INSERT já criou o nó com text_value NULL — make_value_from_storage
      // trata NULL como objeto vazio ("{}"), então o UPDATE é desnecessário.
      if (has_inline)
      {
        // O destrutor free do bind (is_dynamic=1) libera inline_json —
        // NÃO liberar manualmente (evita double-free).
        he_repo_update_text(cache, parent_path, container_text, 1);
      }
    }

    yyjson_mut_doc_free(inline_doc);
  }
  else if (yyjson_is_arr(node))
  {
    // Array → objeto com chaves UUID (modelo Firebase objeto-only): arrays
    // do JSON de entrada viram objetos com chaves únicas; o storage só
    // conhece objetos. O leitor devolve objeto (nunca array).
    he_mapper_flatten_array_as_object(cache, node, parent_path, revision, revision_nr, max_inline_size);
  }
}

/// Achata um array como objeto com chaves UUID (push ID estilo Firebase).
/// Cada elemento vira um filho do container (path = parent_path + uuid) ou
/// um inline child (text_value do pai) com a MESMA chave UUID. null é
/// pulado (sem reindexação — não há índice).
void he_mapper_flatten_array_as_object(HeStmtCache *cache, void *arr_ptr,
                                       const char *parent_path,
                                       const char *revision, int revision_nr,
                                       size_t max_inline_size)
{
  yyjson_val *arr = (yyjson_val *)arr_ptr;
  yyjson_arr_iter iter;
  yyjson_arr_iter_init(arr, &iter);
  yyjson_val *val;
  char current_path[2048];
  char uuid_key[UUID_STR_LEN];

  // Coleta inline children em JSON temporário
  yyjson_mut_doc *inline_doc = NULL;
  yyjson_mut_val *inline_obj = NULL;

  while ((val = yyjson_arr_iter_next(&iter)))
  {
    if (yyjson_is_null(val))
      continue; // null em array: skip (Firebase behavior — sem índice)

    // Chave única por elemento (push ID): gera ANTES de decidir inline,
    // para inline e dedicado usarem a mesma chave.
    generate_uuid_v4(uuid_key, sizeof(uuid_key));

    if (value_fits_inline(val, max_inline_size))
    {
      // Inline: adiciona ao JSON do pai. ⚠️ A chave (uuid_key) é um buffer
      // local: o yyjson_mut_obj_add_* armazena o PONTEIRO sem copiar
      // (yyjson.h yyjson_mut_obj_add_func) — copiar a chave para o doc
      // (yyjson_mut_strcpy) é OBRIGATÓRIO, senão todas as chaves inline
      // apontam para o mesmo buffer e colidem na serialização.
      if (!inline_doc)
      {
        inline_doc = yyjson_mut_doc_new(NULL);
        inline_obj = yyjson_mut_obj(inline_doc);
        yyjson_mut_doc_set_root(inline_doc, inline_obj);
      }

      yyjson_mut_val *key_val = yyjson_mut_strcpy(inline_doc, uuid_key);
      if (yyjson_is_str(val))
      {
        yyjson_mut_obj_add(inline_obj, key_val,
                           yyjson_mut_strcpy(inline_doc, yyjson_get_str(val)));
      }
      else if (yyjson_is_int(val))
      {
        yyjson_mut_obj_add(inline_obj, key_val,
                           yyjson_mut_int(inline_doc, yyjson_get_sint(val)));
      }
      else if (yyjson_is_real(val))
      {
        yyjson_mut_obj_add(inline_obj, key_val,
                           yyjson_mut_real(inline_doc, yyjson_get_real(val)));
      }
      else if (yyjson_is_bool(val))
      {
        yyjson_mut_obj_add(inline_obj, key_val,
                           yyjson_mut_bool(inline_doc, yyjson_get_bool(val)));
      }
      else if (yyjson_is_obj(val) || yyjson_is_arr(val))
      {
        // Objeto/array vazio → objeto vazio (modelo objeto-only)
        yyjson_mut_obj_add(inline_obj, key_val, yyjson_mut_obj(inline_doc));
      }
    }
    else
    {
      // Dedicado
      snprintf(current_path, sizeof(current_path), "%s%s", parent_path, uuid_key);

      if (yyjson_is_obj(val))
      {
        strncat(current_path, "/", sizeof(current_path) - strlen(current_path) - 1);
        he_repo_insert_node_rev(cache, current_path, TYPE_OBJECT, "{}", revision, revision_nr, 0);
        he_mapper_flatten_value(cache, val, current_path, revision, revision_nr, max_inline_size);
      }
      else if (yyjson_is_arr(val))
      {
        strncat(current_path, "/", sizeof(current_path) - strlen(current_path) - 1);
        he_repo_insert_node_rev(cache, current_path, TYPE_OBJECT, "{}", revision, revision_nr, 0);
        he_mapper_flatten_array_as_object(cache, val, current_path, revision, revision_nr, max_inline_size);
      }
      else
      {
        // Serialização DINÂMICA (o buffer fixo truncava strings longas)
        int prim_type;
        char *text_buf = serialize_primitive_value_alloc(val, &prim_type);
        he_repo_insert_node_rev(cache, current_path, prim_type, text_buf,
                                revision, revision_nr, 0);
        sqlite3_free(text_buf);
      }
    }
  }

  if (inline_doc)
  {
    char *inline_json = yyjson_mut_write(inline_doc, 0, NULL);
    if (inline_json)
    {
      // O destrutor free do bind (is_dynamic=1) libera inline_json.
      he_repo_update_text(cache, parent_path, inline_json, 1);
    }
    yyjson_mut_doc_free(inline_doc);
  }
  else
  {
    // Sem inline children: garante text_value = "{}" para coleções vazias
    he_repo_update_empty(cache, parent_path);
  }
}

/* ===========================================================================
 * DEEP MERGE (update parcial)
 * ===========================================================================
 */

/// Faz deep merge de dois valores yyjson.
/// Retorna um mut_val alocado em mut_doc.
void *he_mapper_deep_merge(void *doc_ptr, void *current_ptr, void *update_ptr)
{
  yyjson_mut_doc *mut_doc = (yyjson_mut_doc *)doc_ptr;
  yyjson_val *current = (yyjson_val *)current_ptr;
  yyjson_val *update = (yyjson_val *)update_ptr;

  if (yyjson_is_null(update))
    return NULL; // null → delete

  if (!yyjson_is_obj(current) || !yyjson_is_obj(update))
  {
    // Se algum não é objeto, substitui
    return yyjson_val_mut_copy(mut_doc, update);
  }

  // Ambos são objetos: merge
  yyjson_mut_val *result = yyjson_mut_obj(mut_doc);

  // Copia chaves do current que não serão sobrescritas
  yyjson_obj_iter cur_iter;
  yyjson_obj_iter_init(current, &cur_iter);
  yyjson_val *cur_key, *cur_val;
  while ((cur_key = yyjson_obj_iter_next(&cur_iter)))
  {
    cur_val = yyjson_obj_iter_get_val(cur_key);
    const char *k = yyjson_get_str(cur_key);

    // Verifica se update tem esta chave
    yyjson_val *upd_val = yyjson_obj_get(update, k);
    if (upd_val)
    {
      // Update tem a chave: faz merge recursivo
      if (yyjson_is_null(upd_val))
      {
        // null → delete, não copia
        continue;
      }
      if (yyjson_is_obj(cur_val) && yyjson_is_obj(upd_val))
      {
        yyjson_mut_val *merged = he_mapper_deep_merge(mut_doc, cur_val, upd_val);
        if (merged)
          yyjson_mut_obj_add(result, yyjson_mut_strcpy(mut_doc, k), merged);
      }
      else
      {
        yyjson_mut_obj_add(result, yyjson_mut_strcpy(mut_doc, k),
                           yyjson_val_mut_copy(mut_doc, upd_val));
      }
    }
    else
    {
      // Update não tem esta chave: preserva do current
      yyjson_mut_obj_add(result, yyjson_mut_strcpy(mut_doc, k),
                         yyjson_val_mut_copy(mut_doc, cur_val));
    }
  }

  // Adiciona chaves de update que não estavam em current
  yyjson_obj_iter upd_iter;
  yyjson_obj_iter_init(update, &upd_iter);
  yyjson_val *upd_key;
  while ((upd_key = yyjson_obj_iter_next(&upd_iter)))
  {
    const char *k = yyjson_get_str(upd_key);
    yyjson_val *upd_val = yyjson_obj_iter_get_val(upd_key);

    if (!yyjson_obj_get(current, k) && !yyjson_is_null(upd_val))
    {
      yyjson_mut_obj_add(result, yyjson_mut_strcpy(mut_doc, k),
                         yyjson_val_mut_copy(mut_doc, upd_val));
    }
  }

  return result;
}
