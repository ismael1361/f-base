#include <sqlite3ext.h>
#include <yyjson.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

SQLITE_EXTENSION_INIT1

// =====================================================================
// FUNÇÃO 1: INGEST_JSON (JSON -> Nodes)
// =====================================================================

// Função recursiva para achatar o JSON e inserir na tabela
static void flatten_and_insert(sqlite3 *db, sqlite3_stmt *stmt, yyjson_val *node, const char *parent_path)
{
    char current_path[1024];

    if (yyjson_is_obj(node))
    {
        size_t idx, max;
        yyjson_val *key, *val;
        yyjson_obj_iter iter;
        yyjson_obj_iter_init(node, &iter);

        while ((key = yyjson_obj_iter_next(&iter)))
        {
            val = yyjson_obj_iter_get_val(key);
            const char *k = yyjson_get_str(key);

            snprintf(current_path, sizeof(current_path), "%s/%s", parent_path, k);

            // Lógica de Heurística Híbrida (Exemplo simplificado)
            if (!yyjson_is_obj(val) && !yyjson_is_arr(val))
            {
                // Insere como nó primitivo
                sqlite3_reset(stmt);
                sqlite3_bind_text(stmt, 1, current_path, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(stmt, 2, 5); // type 5 = string/primitive
                sqlite3_bind_text(stmt, 3, yyjson_get_str(val), -1, SQLITE_TRANSIENT);
                sqlite3_step(stmt);
            }
            else
            {
                // Insere nó pai (object/array) e recurse
                sqlite3_reset(stmt);
                sqlite3_bind_text(stmt, 1, current_path, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(stmt, 2, yyjson_is_arr(val) ? 2 : 1); // 1=obj, 2=arr
                sqlite3_bind_null(stmt, 3);
                sqlite3_step(stmt);

                flatten_and_insert(db, stmt, val, current_path);
            }
        }
    }
    else if (yyjson_is_arr(node))
    {
        size_t idx, max;
        yyjson_val *val;
        yyjson_arr_iter iter;
        yyjson_arr_iter_init(node, &iter);
        int i = 0;

        while ((val = yyjson_arr_iter_next(&iter)))
        {
            snprintf(current_path, sizeof(current_path), "%s/%d", parent_path, i++);
            // Lógica similar para arrays... (omissão por brevidade, segue o mesmo padrão)
            flatten_and_insert(db, stmt, val, current_path);
        }
    }
}

static void ingest_json_func(sqlite3_context *context, int argc, sqlite3_value **argv)
{
    const char *doc_id = (const char *)sqlite3_value_text(argv[0]);
    const char *json_str = (const char *)sqlite3_value_text(argv[1]);
    sqlite3 *db = sqlite3_context_db_handle(context);

    // 1. Parse ultrarrápido com yyjson
    yyjson_doc *doc = yyjson_read(json_str, strlen(json_str), 0);
    if (!doc)
    {
        sqlite3_result_error(context, "Invalid JSON", -1);
        return;
    }

    // 2. Prepara o statement de inserção (Reutilização é a chave!)
    sqlite3_stmt *stmt;
    const char *sql = "INSERT OR REPLACE INTO nodes (path, type, text_value, created, modified, revision_nr, revision) "
                      "VALUES (?1, ?2, ?3, unixepoch(), unixepoch(), 1, 'rev_1');";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK)
    {
        sqlite3_result_error(context, sqlite3_errmsg(db), -1);
        yyjson_doc_free(doc);
        return;
    }

    // 3. Inicia Transação Atômica
    sqlite3_exec(db, "BEGIN IMMEDIATE;", 0, 0, 0);

    // 4. Executa o flattening recursivo
    char root_path[1024];
    snprintf(root_path, sizeof(root_path), "/%s", doc_id);
    flatten_and_insert(db, stmt, yyjson_doc_get_root(doc), root_path);

    // 5. Commit e Limpeza
    sqlite3_exec(db, "COMMIT;", 0, 0, 0);
    sqlite3_finalize(stmt);
    yyjson_doc_free(doc);

    sqlite3_result_int(context, 1); // Sucesso
}

// =====================================================================
// FUNÇÃO 2: EXTRACT_JSON (Nodes -> JSON)
// =====================================================================

static void extract_json_func(sqlite3_context *context, int argc, sqlite3_value **argv)
{
    const char *prefix = (const char *)sqlite3_value_text(argv[0]);
    sqlite3 *db = sqlite3_context_db_handle(context);

    // Truque do Prefix Range para forçar Index Seek na B-Tree
    char upper_bound[1024];
    strncpy(upper_bound, prefix, sizeof(upper_bound) - 1);
    upper_bound[strlen(upper_bound) - 1]++; // Ex: '/a/' vira '/a0'

    sqlite3_stmt *stmt;
    const char *sql = "SELECT path, type, text_value FROM nodes WHERE path >= ?1 AND path < ?2 ORDER BY path;";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK)
    {
        sqlite3_result_error(context, sqlite3_errmsg(db), -1);
        return;
    }

    sqlite3_bind_text(stmt, 1, prefix, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, upper_bound, -1, SQLITE_STATIC);

    // Cria o documento JSON mutável em memória nativa
    yyjson_mut_doc *mut_doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root_obj = yyjson_mut_obj(mut_doc);
    yyjson_mut_doc_set_root(mut_doc, root_obj);

    // Stack para reconstrução hierárquica (Aproveitando a ordem do ORDER BY path)
    typedef struct
    {
        char *path;
        yyjson_mut_val *val;
    } StackNode;
    StackNode *stack = malloc(1000 * sizeof(StackNode)); // Alocação simplificada
    int stack_top = 0;
    stack[stack_top++] = (StackNode){strdup(prefix), root_obj};

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char *path = (const char *)sqlite3_column_text(stmt, 0);
        int type = sqlite3_column_int(stmt, 1);
        const char *text_val = (const char *)sqlite3_column_text(stmt, 2);

        // Encontra o pai na stack (o prefixo mais longo que bate com o path atual)
        yyjson_mut_val *parent = root_obj;
        for (int i = stack_top - 1; i >= 0; i--)
        {
            if (strncmp(path, stack[i].path, strlen(stack[i].path)) == 0)
            {
                parent = stack[i].val;
                break;
            }
        }

        // Cria o nó filho
        yyjson_mut_val *child = NULL;
        if (type == 5)
            child = yyjson_mut_str(mut_doc, text_val ? text_val : "");
        else if (type == 1)
            child = yyjson_mut_obj(mut_doc);
        else if (type == 2)
            child = yyjson_mut_arr(mut_doc);

        // Adiciona ao pai (lógica simplificada para objetos)
        char *key = strrchr(path, '/');
        if (key)
        {
            yyjson_mut_obj_add(parent, yyjson_mut_str(mut_doc, key + 1), child);
        }

        // Empilha se for objeto/array para receber filhos futuros
        if (type == 1 || type == 2)
        {
            stack[stack_top++] = (StackNode){strdup(path), child};
        }
    }

    sqlite3_finalize(stmt);

    // Serializa para string
    char *json_out = yyjson_mut_write(mut_doc, 0, NULL);
    if (json_out)
    {
        sqlite3_result_text(context, json_out, -1, free); // SQLite gerencia o free()
    }
    else
    {
        sqlite3_result_null(context);
    }

    // Limpeza da stack
    for (int i = 0; i < stack_top; i++)
        free(stack[i].path);
    free(stack);
    yyjson_mut_doc_free(mut_doc);
}

// =====================================================================
// REGISTRO DA EXTENSÃO
// =====================================================================
#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_hierarchical_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi)
{
    SQLITE_EXTENSION_INIT2(pApi);

    sqlite3_create_function(db, "ingest_json", 2, SQLITE_UTF8, 0, ingest_json_func, 0, 0);
    sqlite3_create_function(db, "extract_json", 1, SQLITE_UTF8, 0, extract_json_func, 0, 0);

    return SQLITE_OK;
}