#include <sqlite3ext.h>
#include <yyjson.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <time.h>

SQLITE_EXTENSION_INIT1

// =====================================================================
// TYPE SYSTEM (alinhado com o MDE JavaScript em ivipbase)
// =====================================================================
// TYPE_EMPTY=0, TYPE_OBJECT=1, TYPE_ARRAY=2, TYPE_NUMBER=3,
// TYPE_BOOLEAN=4, TYPE_STRING=5, TYPE_DATETIME=6, TYPE_BIGINT=7,
// TYPE_BINARY=8, TYPE_REFERENCE=9
//
// Convenção de paths:
//   - Containers (OBJECT, ARRAY) terminam com '/'  → ex: "/users/100/"
//   - Primitivos NÃO terminam com '/'               → ex: "/users/100/name"
//
// text_value:
//   - Para containers: JSON com os filhos inline (ex: '{"name":"John","age":30}')
//   - Para STRING: valor JSON-escapeado (ex: '"John"')
//   - Para NUMBER: representação textual (ex: "30" ou "3.14")
//   - Para BOOLEAN: "true" ou "false"
//   - Para DATETIME/BIGINT/BINARY/REFERENCE: representação textual
#define TYPE_EMPTY 0
#define TYPE_OBJECT 1
#define TYPE_ARRAY 2
#define TYPE_NUMBER 3
#define TYPE_BOOLEAN 4
#define TYPE_STRING 5
#define TYPE_DATETIME 6
#define TYPE_BIGINT 7
#define TYPE_BINARY 8
#define TYPE_REFERENCE 9

#define DEFAULT_MAX_INLINE_SIZE 0
#define UUID_STR_LEN 37

// =====================================================================
// UUID GENERATION (v4)
// =====================================================================

/// Gera um UUID v4 string: "f6df933d8d624407aaf96bacdc0ad1fc"
static void generate_uuid_v4(char *buf, size_t buf_size)
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

// =====================================================================
// PATH NORMALIZATION (para paths concretos, sem wildcard)
// =====================================================================

/// Normaliza um path concreto: garante '/' inicial e '/' final.
/// NÃO suporta wildcards — use path_has_wildcard() primeiro.
///
/// Exemplos:
///   "/people"     → "/people/"
///   "people"      → "/people/"
///   "/people/"    → "/people/"
static void normalize_path(const char *input, char *out_path, size_t out_size)
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
        snprintf(out_path, out_size, "%.*s/", (int)len, input);
    else
        snprintf(out_path, out_size, "/%.*s/", (int)len, input);
}

/// Remove o trailing '/' de um path normalizado.
/// Ex: "/people/" → "/people"
static void path_without_trailing_slash(const char *normalized, char *out, size_t out_size)
{
    size_t len = strlen(normalized);
    if (len > 1 && normalized[len - 1] == '/')
        len--;
    if (len >= out_size)
        len = out_size - 1;
    memcpy(out, normalized, len);
    out[len] = '\0';
}

// =====================================================================
// WILDCARD PATTERN MATCHING (para query/update multi-nível)
// =====================================================================
//
// Suporta:
//   "*"         → qualquer segmento (ex: "/users/*/posts")
//   "$nome"     → variável nomeada (ex: "/users/$uid/posts")
//
// Uso em query:
//   query("/users/*/transactions/*/history/*", {...})
//   → encontra TODOS os paths que casam o padrão e aplica filtros
//
// Uso em update:
//   update("/users/*/status", { online: true })
//   → aplica o merge a TODOS os nós que casam o padrão

#define MAX_WC_SEGMENTS 64

typedef struct
{
    char segments[MAX_WC_SEGMENTS][256];
    bool is_wildcard[MAX_WC_SEGMENTS];
    int count;
} WildcardPattern;

/// Verifica se um path contém wildcards (* ou $)
static bool path_has_wildcard(const char *path)
{
    if (!path)
        return false;
    return strchr(path, '*') != NULL || strchr(path, '$') != NULL;
}

/// Parseia um padrão de path em segmentos.
/// "/users/*/history/*" → ["users", "*", "history", "*"]
static void wildcard_parse(const char *path, WildcardPattern *pattern)
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
static bool wildcard_matches(const char *path, const WildcardPattern *pattern)
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
static void wildcard_fixed_prefix(const char *pattern_path, char *out, size_t out_size)
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
static void make_upper_bound(const char *prefix, char *upper, size_t up_size)
{
    strncpy(upper, prefix, up_size - 1);
    upper[up_size - 1] = '\0';
    size_t len = strlen(upper);
    if (len > 0)
        upper[len - 1]++;
}

// =====================================================================
// HELPERS DE STRING JSON
// =====================================================================

/// Escapa uma string raw para o formato JSON com aspas: `"raw content"`
static void json_escape_string(char *buf, size_t buf_size, const char *raw)
{
    size_t i = 0, j = 1;
    buf[0] = '"';
    while (raw[i] != '\0' && j < buf_size - 2)
    {
        switch (raw[i])
        {
        case '"':
            buf[j++] = '\\';
            if (j < buf_size - 1)
                buf[j++] = '"';
            break;
        case '\\':
            buf[j++] = '\\';
            if (j < buf_size - 1)
                buf[j++] = '\\';
            break;
        case '\n':
            buf[j++] = '\\';
            if (j < buf_size - 1)
                buf[j++] = 'n';
            break;
        case '\r':
            buf[j++] = '\\';
            if (j < buf_size - 1)
                buf[j++] = 'r';
            break;
        case '\t':
            buf[j++] = '\\';
            if (j < buf_size - 1)
                buf[j++] = 't';
            break;
        default:
            buf[j++] = raw[i];
            break;
        }
        i++;
    }
    buf[j++] = '"';
    buf[j] = '\0';
}

/// Faz o unescape de uma string JSON com aspas: `"escaped"` → raw
static void json_unescape_string(const char *quoted, char *buf, size_t buf_size)
{
    size_t len = strlen(quoted);
    if (len < 2 || quoted[0] != '"' || quoted[len - 1] != '"')
    {
        strncpy(buf, quoted, buf_size - 1);
        buf[buf_size - 1] = '\0';
        return;
    }
    size_t i, j;
    for (i = 1, j = 0; i < len - 1 && j < buf_size - 1; i++)
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
                if (j < buf_size - 1)
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
}

// =====================================================================
// HELPERS DE BANCO
// =====================================================================

/// Insere um nó na tabela usando o statement preparado (com revision)
static void insert_node_rev(sqlite3_stmt *stmt, const char *path, int type, const char *text_value,
                            const char *revision, int revision_nr)
{
    sqlite3_reset(stmt);
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, type);
    if (text_value)
    {
        sqlite3_bind_text(stmt, 3, text_value, -1, SQLITE_TRANSIENT);
    }
    else
    {
        sqlite3_bind_null(stmt, 3);
    }
    sqlite3_bind_text(stmt, 4, revision, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 5, revision_nr);
    sqlite3_step(stmt);
}

/// Versão sem revision (usa rev_1 / nr=1 para compatibilidade)
static void insert_node(sqlite3_stmt *stmt, const char *path, int type, const char *text_value)
{
    insert_node_rev(stmt, path, type, text_value, "rev_1", 1);
}

/// Obtém o revision_nr atual para um path (0 se não existir)
static int get_current_revision_nr(sqlite3 *db, const char *path)
{
    sqlite3_stmt *stmt;
    int nr = 0;
    if (sqlite3_prepare_v2(db, "SELECT revision_nr FROM nodes WHERE path = ?1", -1, &stmt, 0) == SQLITE_OK)
    {
        sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW)
        {
            nr = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return nr;
}

/// Deleta todos os nós cujo path esteja no range [prefix, prefix+1)
/// Ex: delete_subtree("/users/100/") → deleta paths >= "/users/100/" e < "/users/1000"
static void delete_subtree(sqlite3 *db, const char *prefix)
{
    if (!prefix || prefix[0] == '\0')
        return;
    char upper[1024];
    strncpy(upper, prefix, sizeof(upper) - 1);
    upper[sizeof(upper) - 1] = '\0';
    size_t len = strlen(upper);
    if (len > 0)
        upper[len - 1]++;

    sqlite3_stmt *del;
    sqlite3_prepare_v2(db, "DELETE FROM nodes WHERE path >= ?1 AND path < ?2", -1, &del, 0);
    if (!del)
        return;
    sqlite3_bind_text(del, 1, prefix, -1, SQLITE_STATIC);
    sqlite3_bind_text(del, 2, upper, -1, SQLITE_STATIC);
    sqlite3_step(del);
    sqlite3_finalize(del);
}

/// Garante que todos os paths intermediários entre "/" e o documento existam.
/// Ex: doc_id = "users/100" → cria "/" (se não existir), "/users/", "/users/100/"
static void ensure_intermediate_paths(sqlite3 *db,
                                      sqlite3_stmt *check,
                                      sqlite3_stmt *insert,
                                      const char *doc_id)
{
    // Garante que "/" existe
    sqlite3_reset(check);
    sqlite3_bind_text(check, 1, "/", -1, SQLITE_STATIC);
    if (sqlite3_step(check) != SQLITE_ROW)
    {
        insert_node(insert, "/", 1, "{}");
    }

    char full_path[1024] = "/";
    char *copy = sqlite3_mprintf("%s", doc_id);
    if (!copy)
        return;

    // Tokeniza doc_id em um array
    char tokens[64][256];
    int token_count = 0;
    char *token = strtok(copy, "/");
    while (token && token_count < 64)
    {
        strncpy(tokens[token_count], token, 255);
        tokens[token_count][255] = '\0';
        token_count++;
        token = strtok(NULL, "/");
    }

    // Cria APENAS os paths INTERMEDIÁRIOS (exclui o último token = nome do documento)
    // Ex: doc_id "users/100" → cria "/users/" (NÃO cria "/users/100/")
    for (int i = 0; i < token_count - 1; i++)
    {
        size_t cur = strlen(full_path);
        snprintf(full_path + cur, sizeof(full_path) - cur, "%s/", tokens[i]);

        sqlite3_reset(check);
        sqlite3_bind_text(check, 1, full_path, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(check) != SQLITE_ROW)
        {
            insert_node(insert, full_path, 1, "{}");
        }
    }

    sqlite3_free(copy);
}

// =====================================================================
// VALUE FITS INLINE (alinhado com valueFitsInline do MDE JavaScript)
// =====================================================================

/// Verifica se um valor JSON pode ser armazenado inline no nó pai
/// em vez de virar um nó filho dedicado.
/// Retorna true para: number, boolean, string curta, objeto/array vazio
static bool value_fits_inline(yyjson_val *val, size_t max_inline_size)
{
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

// =====================================================================
// FLATTEN_AND_INSERT (recursivo) — com inline optimization + UUID arrays
// =====================================================================

// Forward declarations
static void flatten_value(sqlite3 *db, sqlite3_stmt *stmt,
                          yyjson_val *val, const char *parent_path,
                          const char *revision, int revision_nr,
                          size_t max_inline_size);
static void flatten_array_as_object(sqlite3 *db, sqlite3_stmt *stmt,
                                    yyjson_val *arr, const char *parent_path,
                                    const char *revision, int revision_nr,
                                    size_t max_inline_size);

/// Serializa um valor primitivo yyjson como texto para text_value
static void serialize_primitive_value(yyjson_val *val, char *buf, size_t buf_size, int *out_type)
{
    if (yyjson_is_str(val))
    {
        *out_type = TYPE_STRING;
        json_escape_string(buf, buf_size, yyjson_get_str(val));
    }
    else if (yyjson_is_int(val))
    {
        *out_type = TYPE_NUMBER;
        snprintf(buf, buf_size, "%lld", yyjson_get_sint(val));
    }
    else if (yyjson_is_real(val))
    {
        *out_type = TYPE_NUMBER;
        snprintf(buf, buf_size, "%.17g", yyjson_get_real(val));
    }
    else if (yyjson_is_bool(val))
    {
        *out_type = TYPE_BOOLEAN;
        strcpy(buf, yyjson_get_bool(val) ? "true" : "false");
    }
    else
    {
        *out_type = TYPE_EMPTY;
        buf[0] = '\0';
    }
}

/// Achata um valor JSON em nós da tabela (SET mode).
/// Usa inline optimization: primitivos pequenos ficam no text_value do pai.
/// Arrays são convertidos para objetos com chaves UUID (Firebase-style).
/// parent_path SEMPRE termina com '/' (ex: "/users/100/").
static void flatten_value(sqlite3 *db, sqlite3_stmt *stmt,
                          yyjson_val *node, const char *parent_path,
                          const char *revision, int revision_nr,
                          size_t max_inline_size)
{
    char current_path[2048];
    yyjson_mut_doc *tmp_doc = NULL; // usado apenas para construir JSON inline

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
                delete_subtree(db, del_path);
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
                    // vazio
                    if (yyjson_is_obj(val))
                        yyjson_mut_obj_add_obj(inline_doc, inline_obj, k);
                    else
                        yyjson_mut_obj_add_arr(inline_doc, inline_obj, k);
                }
                has_inline = true;
            }
            else
            {
                // === DEDICADO: vira nó filho ===
                if (yyjson_is_obj(val))
                {
                    strncat(current_path, "/", sizeof(current_path) - strlen(current_path) - 1);
                    insert_node_rev(stmt, current_path, TYPE_OBJECT, NULL, revision, revision_nr);
                    flatten_value(db, stmt, val, current_path, revision, revision_nr, max_inline_size);
                }
                else if (yyjson_is_arr(val))
                {
                    strncat(current_path, "/", sizeof(current_path) - strlen(current_path) - 1);
                    insert_node_rev(stmt, current_path, TYPE_ARRAY, NULL, revision, revision_nr);
                    flatten_array_as_object(db, stmt, val, current_path, revision, revision_nr, max_inline_size);
                }
                else
                {
                    // Primitivo dedicado (string longa)
                    int prim_type;
                    char text_buf[1024];
                    serialize_primitive_value(val, text_buf, sizeof(text_buf), &prim_type);
                    insert_node_rev(stmt, current_path, prim_type, text_buf, revision, revision_nr);
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

            sqlite3_stmt *upd;
            if (sqlite3_prepare_v2(db,
                                   "UPDATE nodes SET text_value = ?1 WHERE path = ?2", -1, &upd, 0) == SQLITE_OK)
            {
                sqlite3_bind_text(upd, 1, container_text, -1,
                                  has_inline ? free : SQLITE_STATIC);
                sqlite3_bind_text(upd, 2, parent_path, -1, SQLITE_STATIC);
                sqlite3_step(upd);
                sqlite3_finalize(upd);
            }
            else if (inline_json)
            {
                free(inline_json);
            }
        }

        yyjson_mut_doc_free(inline_doc);
    }
    else if (yyjson_is_arr(node))
    {
        // Array → objeto com chaves UUID
        flatten_array_as_object(db, stmt, node, parent_path, revision, revision_nr, max_inline_size);
    }
}

/// Achata um array usando índices numéricos.
static void flatten_array_as_object(sqlite3 *db, sqlite3_stmt *stmt,
                                    yyjson_val *arr, const char *parent_path,
                                    const char *revision, int revision_nr,
                                    size_t max_inline_size)
{
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(arr, &iter);
    yyjson_val *val;
    char current_path[2048];
    char idx_key[64];
    int idx = 0;

    // Coleta inline children em JSON temporário
    yyjson_mut_doc *inline_doc = NULL;
    yyjson_mut_val *inline_obj = NULL;

    while ((val = yyjson_arr_iter_next(&iter)))
    {
        snprintf(idx_key, sizeof(idx_key), "%d", idx);

        if (yyjson_is_null(val))
        {
            idx++;
            continue; // null em array: skip (Firebase behavior)
        }

        if (value_fits_inline(val, max_inline_size))
        {
            // Inline: adiciona ao JSON do pai
            if (!inline_doc)
            {
                inline_doc = yyjson_mut_doc_new(NULL);
                inline_obj = yyjson_mut_obj(inline_doc);
                yyjson_mut_doc_set_root(inline_doc, inline_obj);
            }

            if (yyjson_is_str(val))
            {
                yyjson_mut_obj_add_str(inline_doc, inline_obj, idx_key, yyjson_get_str(val));
            }
            else if (yyjson_is_int(val))
            {
                yyjson_mut_obj_add_int(inline_doc, inline_obj, idx_key, yyjson_get_sint(val));
            }
            else if (yyjson_is_real(val))
            {
                yyjson_mut_obj_add_real(inline_doc, inline_obj, idx_key, yyjson_get_real(val));
            }
            else if (yyjson_is_bool(val))
            {
                yyjson_mut_obj_add_bool(inline_doc, inline_obj, idx_key, yyjson_get_bool(val));
            }
            else if (yyjson_is_obj(val) || yyjson_is_arr(val))
            {
                yyjson_mut_val *empty;
                if (yyjson_is_obj(val))
                    empty = yyjson_mut_obj(inline_doc);
                else
                    empty = yyjson_mut_arr(inline_doc);
                yyjson_mut_obj_add(inline_obj, yyjson_mut_strcpy(inline_doc, idx_key), empty);
            }
            idx++;
        }
        else
        {
            // Dedicado
            snprintf(current_path, sizeof(current_path), "%s%s", parent_path, idx_key);

            if (yyjson_is_obj(val))
            {
                strncat(current_path, "/", sizeof(current_path) - strlen(current_path) - 1);
                insert_node_rev(stmt, current_path, TYPE_OBJECT, NULL, revision, revision_nr);
                flatten_value(db, stmt, val, current_path, revision, revision_nr, max_inline_size);
            }
            else if (yyjson_is_arr(val))
            {
                strncat(current_path, "/", sizeof(current_path) - strlen(current_path) - 1);
                insert_node_rev(stmt, current_path, TYPE_ARRAY, NULL, revision, revision_nr);
                flatten_array_as_object(db, stmt, val, current_path, revision, revision_nr, max_inline_size);
            }
            else
            {
                int prim_type;
                char text_buf[1024];
                serialize_primitive_value(val, text_buf, sizeof(text_buf), &prim_type);
                insert_node_rev(stmt, current_path, prim_type, text_buf, revision, revision_nr);
            }
            idx++;
        }
    }

    if (inline_doc)
    {
        char *inline_json = yyjson_mut_write(inline_doc, 0, NULL);
        if (inline_json)
        {
            sqlite3_stmt *upd;
            if (sqlite3_prepare_v2(db,
                                   "UPDATE nodes SET text_value = ?1 WHERE path = ?2", -1, &upd, 0) == SQLITE_OK)
            {
                sqlite3_bind_text(upd, 1, inline_json, -1, free);
                sqlite3_bind_text(upd, 2, parent_path, -1, SQLITE_STATIC);
                sqlite3_step(upd);
                sqlite3_finalize(upd);
            }
            else
            {
                free(inline_json);
            }
        }
        yyjson_mut_doc_free(inline_doc);
    }
    else
    {
        // Sem inline children: garante text_value = "{}" para arrays vazios
        sqlite3_stmt *upd;
        if (sqlite3_prepare_v2(db,
                               "UPDATE nodes SET text_value = '{}' WHERE path = ?1 AND text_value IS NULL", -1, &upd, 0) == SQLITE_OK)
        {
            sqlite3_bind_text(upd, 1, parent_path, -1, SQLITE_STATIC);
            sqlite3_step(upd);
            sqlite3_finalize(upd);
        }
    }
}

// =====================================================================
// FUNÇÃO: SET_JSON (JSON -> Nodes, replace completo)
// =====================================================================

/// set_json(doc_id, json_text) — Substitui completamente o documento.
/// Se json_text for JSON null, remove o documento.
/// Usa inline optimization + array→UUID + revision tracking.
static void set_json_func(sqlite3_context *context, int argc, sqlite3_value **argv)
{
    if (argc != 2 && argc != 3)
    {
        sqlite3_result_error(context, "Usage: set_json(doc_id, json_text [, max_inline_size])", -1);
        return;
    }

    const char *doc_id = (const char *)sqlite3_value_text(argv[0]);
    const char *json_str = (const char *)sqlite3_value_text(argv[1]);
    if (!doc_id || !json_str)
    {
        sqlite3_result_error(context, "Arguments must not be NULL", -1);
        return;
    }

    size_t max_inline_size = DEFAULT_MAX_INLINE_SIZE;
    if (argc == 3 && sqlite3_value_type(argv[2]) == SQLITE_INTEGER)
    {
        int val = sqlite3_value_int(argv[2]);
        if (val > 0)
            max_inline_size = (size_t)val;
    }

    sqlite3 *db = sqlite3_context_db_handle(context);

    // Parse JSON
    yyjson_doc *doc = yyjson_read(json_str, strlen(json_str), 0);
    if (!doc)
    {
        sqlite3_result_error(context, "Invalid JSON", -1);
        return;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);

    // Normaliza root_path
    char root_path[1024];
    normalize_path(doc_id, root_path, sizeof(root_path));

    // Gera revision UUID
    char revision[UUID_STR_LEN];
    generate_uuid_v4(revision, sizeof(revision));

    // Prepara statements
    const char *sql_insert =
        "INSERT OR REPLACE INTO nodes (path, type, text_value, created, modified, revision_nr, revision) "
        "VALUES (?1, ?2, ?3, "
        "  COALESCE((SELECT created FROM nodes WHERE path = ?1), unixepoch()), "
        "  unixepoch(), ?5, ?4)";
    sqlite3_stmt *insert_stmt;
    if (sqlite3_prepare_v2(db, sql_insert, -1, &insert_stmt, 0) != SQLITE_OK)
    {
        sqlite3_result_error(context, sqlite3_errmsg(db), -1);
        yyjson_doc_free(doc);
        return;
    }

    sqlite3_stmt *check_stmt;
    sqlite3_prepare_v2(db, "SELECT 1 FROM nodes WHERE path = ?1", -1, &check_stmt, 0);

    // === Transação ===
    sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);

    // Deleta subtree existente
    delete_subtree(db, root_path);

    if (yyjson_is_null(root))
    {
        sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
        sqlite3_finalize(insert_stmt);
        sqlite3_finalize(check_stmt);
        yyjson_doc_free(doc);
        sqlite3_result_text(context, revision, -1, SQLITE_TRANSIENT);
        return;
    }

    // Garante paths intermediários
    ensure_intermediate_paths(db, check_stmt, insert_stmt, doc_id);

    // Calcula revision_nr
    int rev_nr = get_current_revision_nr(db, root_path) + 1;

    // Insere container root
    insert_node_rev(insert_stmt, root_path, TYPE_OBJECT, NULL, revision, rev_nr);

    // Achata o valor
    flatten_value(db, insert_stmt, root, root_path, revision, rev_nr, max_inline_size);

    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);

    sqlite3_finalize(insert_stmt);
    sqlite3_finalize(check_stmt);
    yyjson_doc_free(doc);

    sqlite3_result_text(context, revision, -1, SQLITE_TRANSIENT);
}

// =====================================================================
// FUNÇÃO: UPDATE_JSON (JSON -> Nodes, merge)
// =====================================================================

/// Faz deep merge de dois valores yyjson.
/// Retorna um mut_val alocado em mut_doc.
static yyjson_mut_val *deep_merge_values(yyjson_mut_doc *mut_doc,
                                         yyjson_val *current,
                                         yyjson_val *update)
{
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
                yyjson_mut_val *merged = deep_merge_values(mut_doc, cur_val, upd_val);
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

/// update_json(doc_id, json_text [, max_inline_size])
/// Faz merge do json_text no documento existente.
/// Preserva chaves existentes não mencionadas no json_text.
static void update_json_func(sqlite3_context *context, int argc, sqlite3_value **argv)
{
    if (argc != 2 && argc != 3)
    {
        sqlite3_result_error(context, "Usage: update_json(doc_id, json_text [, max_inline_size])", -1);
        return;
    }

    const char *doc_id = (const char *)sqlite3_value_text(argv[0]);
    const char *json_str = (const char *)sqlite3_value_text(argv[1]);
    if (!doc_id || !json_str)
    {
        sqlite3_result_error(context, "Arguments must not be NULL", -1);
        return;
    }

    size_t max_inline_size = DEFAULT_MAX_INLINE_SIZE;
    if (argc == 3 && sqlite3_value_type(argv[2]) == SQLITE_INTEGER)
    {
        int val = sqlite3_value_int(argv[2]);
        if (val > 0)
            max_inline_size = (size_t)val;
    }

    sqlite3 *db = sqlite3_context_db_handle(context);

    // Normaliza root_path e detecta wildcards
    bool has_wildcard = path_has_wildcard(doc_id);
    char root_path[1024];
    // Para wildcard, root_path = parte fixa antes do primeiro wildcard
    if (has_wildcard) {
        wildcard_fixed_prefix(doc_id, root_path, sizeof(root_path));
    } else {
        normalize_path(doc_id, root_path, sizeof(root_path));
    }

    // Carrega o JSON atual via extract_json
    char extract_path[1024];
    path_without_trailing_slash(root_path, extract_path, sizeof(extract_path));

    sqlite3_stmt *extract_stmt;
    const char *extract_sql = "SELECT COALESCE(extract_json(?1), 'null')";
    if (sqlite3_prepare_v2(db, extract_sql, -1, &extract_stmt, 0) != SQLITE_OK)
    {
        sqlite3_result_error(context, "Failed to prepare extract", -1);
        return;
    }
    sqlite3_bind_text(extract_stmt, 1, extract_path, -1, SQLITE_STATIC);

    yyjson_doc *current_doc = NULL;
    if (sqlite3_step(extract_stmt) == SQLITE_ROW)
    {
        const char *json_str = (const char *)sqlite3_column_text(extract_stmt, 0);
        if (json_str && strcmp(json_str, "null") != 0)
        {
            current_doc = yyjson_read(json_str, strlen(json_str), 0);
        }
    }
    sqlite3_finalize(extract_stmt);

    // Parse o JSON de update
    yyjson_doc *update_doc = yyjson_read(json_str, strlen(json_str), 0);
    if (!update_doc)
    {
        if (current_doc) yyjson_doc_free(current_doc);
        sqlite3_result_error(context, "Invalid JSON in update", -1);
        return;
    }
    yyjson_val *update_root = yyjson_doc_get_root(update_doc);

    if (!yyjson_is_obj(update_root) && !yyjson_is_null(update_root))
    {
        yyjson_doc_free(update_doc);
        if (current_doc) yyjson_doc_free(current_doc);
        sqlite3_result_error(context, "UPDATE only supports JSON objects or null", -1);
        return;
    }

    // Se update é null, remove
    if (yyjson_is_null(update_root))
    {
        yyjson_doc_free(update_doc);
        if (current_doc) yyjson_doc_free(current_doc);
        char upper_del[1024];
        strncpy(upper_del, root_path, sizeof(upper_del) - 1);
        upper_del[sizeof(upper_del) - 1] = '\0';
        size_t dlen = strlen(upper_del);
        if (dlen > 0) upper_del[dlen - 1]++;
        sqlite3_stmt *del_stmt;
        if (sqlite3_prepare_v2(db, "DELETE FROM nodes WHERE path >= ?1 AND path < ?2", -1, &del_stmt, 0) == SQLITE_OK)
        {
            sqlite3_bind_text(del_stmt, 1, root_path, -1, SQLITE_STATIC);
            sqlite3_bind_text(del_stmt, 2, upper_del, -1, SQLITE_STATIC);
            sqlite3_step(del_stmt);
            sqlite3_finalize(del_stmt);
        }
        char rev[UUID_STR_LEN];
        generate_uuid_v4(rev, sizeof(rev));
        sqlite3_result_text(context, rev, -1, SQLITE_TRANSIENT);
        return;
    }

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
                    yyjson_mut_val *child_merged = deep_merge_values(merge_doc, v, update_root);
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
            sqlite3_result_null(context);
            return;
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
            merged = deep_merge_values(merge_doc, current_root, update_root);
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
        if (current_doc) yyjson_doc_free(current_doc);
        yyjson_doc_free(update_doc);
        sqlite3_result_error(context, "Failed to serialize merged JSON", -1);
        return;
    }

    yyjson_mut_doc_free(merge_doc);
    if (current_doc) yyjson_doc_free(current_doc);
    yyjson_doc_free(update_doc);

    // Aplica SET com o JSON merged
    yyjson_doc *final_doc = yyjson_read(merged_json, strlen(merged_json), 0);
    free(merged_json);
    if (!final_doc)
    {
        sqlite3_result_error(context, "Failed to parse merged JSON", -1);
        return;
    }
    yyjson_val *final_root = yyjson_doc_get_root(final_doc);

    char revision[UUID_STR_LEN];
    generate_uuid_v4(revision, sizeof(revision));

    const char *sql_insert2 =
        "INSERT OR REPLACE INTO nodes (path, type, text_value, created, modified, revision_nr, revision) "
        "VALUES (?1, ?2, ?3, "
        "  COALESCE((SELECT created FROM nodes WHERE path = ?1), unixepoch()), "
        "  unixepoch(), ?5, ?4)";
    sqlite3_stmt *insert_stmt2;
    if (sqlite3_prepare_v2(db, sql_insert2, -1, &insert_stmt2, 0) != SQLITE_OK)
    {
        sqlite3_result_error(context, sqlite3_errmsg(db), -1);
        yyjson_doc_free(final_doc);
        return;
    }

    sqlite3_stmt *check_stmt2;
    sqlite3_prepare_v2(db, "SELECT 1 FROM nodes WHERE path = ?1", -1, &check_stmt2, 0);

    sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);

    delete_subtree(db, root_path);
    // Para wildcard, doc_id é o path original (ex: "/people/*") → ensure_intermediate_paths
    // precisa do doc_id sem wildcard para criar os paths intermediários corretos
    char clean_doc_id[1024];
    path_without_trailing_slash(root_path, clean_doc_id, sizeof(clean_doc_id));
    // Remove leading / para ensure_intermediate_paths
    const char *id_for_ensure = clean_doc_id;
    if (id_for_ensure[0] == '/') id_for_ensure++;
    ensure_intermediate_paths(db, check_stmt2, insert_stmt2, id_for_ensure);

    int rev_nr = get_current_revision_nr(db, root_path) + 1;
    insert_node_rev(insert_stmt2, root_path, TYPE_OBJECT, NULL, revision, rev_nr);
    flatten_value(db, insert_stmt2, final_root, root_path, revision, rev_nr, max_inline_size);

    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);

    sqlite3_finalize(insert_stmt2);
    sqlite3_finalize(check_stmt2);
    yyjson_doc_free(final_doc);

    sqlite3_result_text(context, revision, -1, SQLITE_TRANSIENT);
}

// =====================================================================
// HELPER: Reconstrói um valor a partir do type e text_value
// =====================================================================

/// Cria um mut_val a partir do type e text_value armazenados.
/// Se type for container com inline children em text_value, faz merge.
static yyjson_mut_val *make_value_from_storage(yyjson_mut_doc *doc,
                                               int type, const char *text_val)
{
    switch (type)
    {
    case TYPE_OBJECT:
    {
        yyjson_mut_val *obj = yyjson_mut_obj(doc);
        // Se há inline children, faz merge
        if (text_val && text_val[0] != '\0')
        {
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
    case TYPE_ARRAY:
    {
        yyjson_mut_val *arr = yyjson_mut_arr(doc);
        // Se há inline children, adiciona como elementos
        if (text_val && text_val[0] != '\0')
        {
            yyjson_doc *inline_doc = yyjson_read(text_val, strlen(text_val), 0);
            if (inline_doc)
            {
                yyjson_val *inline_root = yyjson_doc_get_root(inline_doc);
                if (inline_root && yyjson_is_obj(inline_root))
                {
                    // Inline children são armazenados como objeto no text_value
                    // Precisamos ordenar pelas chaves e adicionar como elementos
                    yyjson_obj_iter iter;
                    yyjson_obj_iter_init(inline_root, &iter);
                    yyjson_val *k, *v;
                    while ((k = yyjson_obj_iter_next(&iter)))
                    {
                        v = yyjson_obj_iter_get_val(k);
                        yyjson_mut_arr_append(arr, yyjson_val_mut_copy(doc, v));
                    }
                }
                yyjson_doc_free(inline_doc);
            }
        }
        return arr;
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
            char unesc[2048];
            json_unescape_string(text_val, unesc, sizeof(unesc));
            return yyjson_mut_strcpy(doc, unesc);
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

// =====================================================================
// FUNÇÃO: EXTRACT_JSON (Nodes -> JSON) com tipos corrigidos + inline
// =====================================================================

/// extract_json(prefix) — Reconstrói o JSON a partir dos nodes armazenados.
/// Agora com suporte a inline children (text_value do container) e tipos
/// alinhados com o MDE JavaScript.
static void extract_json_func(sqlite3_context *context, int argc, sqlite3_value **argv)
{
    if (argc != 1 && argc != 2)
    {
        sqlite3_result_error(context, "Usage: extract_json(prefix [, options_json])", -1);
        return;
    }

    const char *input = (const char *)sqlite3_value_text(argv[0]);
    if (!input || input[0] == '\0')
    {
        sqlite3_result_null(context);
        return;
    }

    // Options opcionais (include/exclude)
    yyjson_doc *opt_doc = NULL;
    yyjson_val *opt_include = NULL;
    yyjson_val *opt_exclude = NULL;
    if (argc == 2 && sqlite3_value_type(argv[1]) == SQLITE_TEXT)
    {
        const char *opt_str = (const char *)sqlite3_value_text(argv[1]);
        if (opt_str)
        {
            opt_doc = yyjson_read(opt_str, strlen(opt_str), 0);
            if (opt_doc)
            {
                yyjson_val *root = yyjson_doc_get_root(opt_doc);
                if (root)
                {
                    opt_include = yyjson_obj_get(root, "include");
                    opt_exclude = yyjson_obj_get(root, "exclude");
                }
            }
        }
    }

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

    sqlite3 *db = sqlite3_context_db_handle(context);

    sqlite3_stmt *stmt;
    const char *sql =
        "SELECT path, type, text_value FROM nodes "
        "WHERE path >= ?1 AND path < ?2 ORDER BY path";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK)
    {
        sqlite3_result_error(context, sqlite3_errmsg(db), -1);
        if (opt_doc)
            yyjson_doc_free(opt_doc);
        return;
    }
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

    // Helper inline: verifica se uma chave deve ser incluída
    // (aplicação de include/exclude)
    // Nota: como esta é uma versão simplificada, ignoramos include/exclude
    // por enquanto — a filtragem é feita no query_json

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

        // 1. Encontra o pai na stack
        yyjson_mut_val *parent = NULL;
        for (int i = stack_top - 1; i >= 0; i--)
        {
            size_t sp_len = strlen(stack[i].path);
            size_t slen = sp_len;
            while (slen > 0 && stack[i].path[slen - 1] == '/')
                slen--;
            if (strncmp(path, stack[i].path, slen) == 0 &&
                (path[slen] == '/' || path[slen] == '\0'))
            {
                parent = stack[i].val;
                break;
            }
        }
        if (!parent)
            parent = root;

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

        if (type == TYPE_OBJECT || type == TYPE_ARRAY)
        {
            child = make_value_from_storage(mut_doc, type, text_val);
            is_container = true;
        }
        else
        {
            child = make_value_from_storage(mut_doc, type, text_val);
        }

        // 4. Adiciona ao pai
        if (yyjson_mut_is_arr(parent))
        {
            // Array pai: mantém ordenação com append
            yyjson_mut_arr_append(parent, child);
        }
        else
        {
            // Objeto pai: adiciona com chave
            yyjson_mut_obj_add(parent,
                               yyjson_mut_strcpy(mut_doc, key), child);
        }

        // 5. Se container, empilha
        if (is_container)
        {
            if (stack_top < 2048)
            {
                stack[stack_top++] = (StackNode){sqlite3_mprintf("%s", path), child};
            }
        }
    }

    sqlite3_finalize(stmt);

    if (rows_count == 0)
    {
        yyjson_mut_doc_free(mut_doc);
        if (opt_doc)
            yyjson_doc_free(opt_doc);
        sqlite3_result_null(context);
        return;
    }

    char *json_out = yyjson_mut_write(mut_doc, 0, NULL);
    if (json_out)
    {
        sqlite3_result_text(context, json_out, -1, free);
    }
    else
    {
        sqlite3_result_null(context);
    }

    for (int i = 0; i < stack_top; i++)
        sqlite3_free((void *)stack[i].path);

    yyjson_mut_doc_free(mut_doc);
    if (opt_doc)
        yyjson_doc_free(opt_doc);
}

// =====================================================================
// FUNÇÃO: QUERY_JSON (Query com filtros, ordenação, paginação)
// =====================================================================

/// Estrutura para representar um filtro de query
typedef struct
{
    char key[256];           // Campo para filtrar (suporta notação aninhada: "address.city")
    char op[16];             // Operador: <, <=, ==, !=, >=, >, like, exists, in, between
    char compare[512];       // Valor para comparar (como string)
    yyjson_val *compare_val; // Referência ao valor JSON parseado
    bool valid;
} QueryFilter;

/// Estrutura para representar um campo de ordenação
typedef struct
{
    char key[256];
    bool ascending;
} QueryOrder;

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

    // Operadores de comparação numérica
    if (is_num && filter->compare_val && yyjson_is_num(filter->compare_val))
    {
        double cmp = yyjson_get_num(filter->compare_val);

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
        if (strcmp(op, "between") == 0 && yyjson_is_arr(filter->compare_val))
        {
            yyjson_val *lo = yyjson_arr_get(filter->compare_val, 0);
            yyjson_val *hi = yyjson_arr_get(filter->compare_val, 1);
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
        if (strcmp(op, "in") == 0 && filter->compare_val && yyjson_is_arr(filter->compare_val))
        {
            size_t n = yyjson_arr_size(filter->compare_val);
            for (size_t i = 0; i < n; i++)
            {
                yyjson_val *item = yyjson_arr_get(filter->compare_val, i);
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

/// Comparador para qsort
typedef struct
{
    yyjson_val *val;
    size_t index;
} SortEntry;

// Ponteiro estático para orders (usado pelo qsort comparator)
// Safe em extensão SQLite porque executa em uma única thread
static QueryOrder *g_sort_orders = NULL;

static int compare_entries(const void *a, const void *b)
{
    const SortEntry *ea = (const SortEntry *)a;
    const SortEntry *eb = (const SortEntry *)b;
    QueryOrder *orders = g_sort_orders;

    if (!orders)
        return 0;

    for (int i = 0; orders[i].key[0] != '\0'; i++)
    {
        // Navega pela chain de chaves
        yyjson_val *va = ea->val;
        yyjson_val *vb = eb->val;
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

// =====================================================================
// HELPERS PARA WILDCARD COM CAPTURA DE VARIÁVEIS
// =====================================================================

/// Extrai um segmento específico de um path.
/// Ex: extract_path_segment("/users/alice/posts/p123", 3) → "p123"
static bool extract_path_segment(const char *path, int seg_idx,
                                 char *out, size_t out_size)
{
    if (!path || !*path || out_size == 0) return false;
    out[0] = '\0';
    const char *p = path;
    if (*p == '/') p++;
    int current = 0;
    while (*p && current < seg_idx)
    {
        const char *end = strchr(p, '/');
        if (!end) return false;
        p = end + 1;
        current++;
    }
    if (!*p || current != seg_idx) return false;
    const char *end = strchr(p, '/');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

/// Avalia um filtro de variável (ex: $postId == "abc" ou {key} == "abc")
/// comparando o valor capturado com o valor do filtro.
static bool evaluate_var_filter(const char *captured_val,
                                const QueryFilter *filter)
{
    if (!captured_val) return false;

    if (strcmp(filter->op, "==") == 0)
        return strcmp(captured_val, filter->compare) == 0;
    if (strcmp(filter->op, "!=") == 0)
        return strcmp(captured_val, filter->compare) != 0;
    if (strcmp(filter->op, "like") == 0)
        return strstr(captured_val, filter->compare) != NULL;
    return false;
}

// =====================================================================
// DO_QUERY_WILDCARD: executa query_json em paths com wildcard multi-nível
// =====================================================================

/// Processa uma query onde o path contém wildcards (* ou $var).
/// Ex: query_json("/users/*/transactions/*/history/*", { filters: [...] })
/// 1. Escaneia a tabela nodes por paths que casam o padrão
/// 2. Para cada match, obtém o valor via extract_json
/// 3. Aplica filtros, ordenação e paginação
/// 4. Retorna array JSON com os resultados
static void do_query_wildcard(sqlite3_context *context, sqlite3 *db,
                              const char *pattern, const char *query_str)
{
    // ── 1. Parseia o JSON da query ──
    yyjson_doc *query_doc = yyjson_read(query_str, strlen(query_str), 0);
    if (!query_doc)
    {
        sqlite3_result_error(context, "Invalid query JSON in wildcard query", -1);
        return;
    }
    yyjson_val *query_root = yyjson_doc_get_root(query_doc);
    if (!query_root)
    {
        yyjson_doc_free(query_doc);
        sqlite3_result_error(context, "Empty query JSON", -1);
        return;
    }

    // ── 2. Extrai filtros ──
    QueryFilter filters[64];
    int filter_count = 0;
    memset(filters, 0, sizeof(filters));

    yyjson_val *filters_val = yyjson_obj_get(query_root, "filters");
    if (filters_val && yyjson_is_arr(filters_val))
    {
        size_t n = yyjson_arr_size(filters_val);
        for (size_t i = 0; i < n && filter_count < 64; i++)
        {
            yyjson_val *f = yyjson_arr_get(filters_val, i);
            if (!yyjson_is_obj(f)) continue;

            yyjson_val *key_v = yyjson_obj_get(f, "key");
            yyjson_val *op_v = yyjson_obj_get(f, "op");
            yyjson_val *cmp_v = yyjson_obj_get(f, "compare");

            if (key_v && yyjson_is_str(key_v) && op_v && yyjson_is_str(op_v))
            {
                strncpy(filters[filter_count].key, yyjson_get_str(key_v),
                        sizeof(filters[filter_count].key) - 1);
                strncpy(filters[filter_count].op, yyjson_get_str(op_v),
                        sizeof(filters[filter_count].op) - 1);
                if (cmp_v)
                {
                    filters[filter_count].compare_val = cmp_v;
                    if (yyjson_is_str(cmp_v))
                        strncpy(filters[filter_count].compare, yyjson_get_str(cmp_v),
                                sizeof(filters[filter_count].compare) - 1);
                    else if (yyjson_is_num(cmp_v))
                        snprintf(filters[filter_count].compare, sizeof(filters[filter_count].compare),
                                 "%.17g", yyjson_get_num(cmp_v));
                    else if (yyjson_is_bool(cmp_v))
                        strcpy(filters[filter_count].compare,
                               yyjson_get_bool(cmp_v) ? "true" : "false");
                }
                filters[filter_count].valid = true;
                filter_count++;
            }
        }
    }

    // ── 3. Extrai ordenação ──
    QueryOrder orders[16];
    int order_count = 0;
    memset(orders, 0, sizeof(orders));

    yyjson_val *orders_val = yyjson_obj_get(query_root, "order");
    if (orders_val && yyjson_is_arr(orders_val))
    {
        size_t n = yyjson_arr_size(orders_val);
        for (size_t i = 0; i < n && order_count < 16; i++)
        {
            yyjson_val *o = yyjson_arr_get(orders_val, i);
            if (!yyjson_is_obj(o)) continue;
            yyjson_val *k = yyjson_obj_get(o, "key");
            yyjson_val *a = yyjson_obj_get(o, "ascending");
            if (k && yyjson_is_str(k))
            {
                strncpy(orders[order_count].key, yyjson_get_str(k),
                        sizeof(orders[order_count].key) - 1);
                orders[order_count].ascending = !a ||
                    (yyjson_is_bool(a) && yyjson_get_bool(a)) ||
                    (yyjson_is_int(a) && yyjson_get_int(a) == 1);
                order_count++;
            }
        }
    }

    // ── 4. Extrai paginação ──
    yyjson_val *skip_val = yyjson_obj_get(query_root, "skip");
    yyjson_val *take_val = yyjson_obj_get(query_root, "take");
    size_t skip = skip_val && yyjson_is_int(skip_val) ? (size_t)yyjson_get_int(skip_val) : 0;
    size_t take = take_val && yyjson_is_int(take_val) ? (size_t)yyjson_get_int(take_val) : 0;
    if (take == 0) take = (size_t)-1;

    // ── 5. Obtém prefixo fixo e parseia o padrão wildcard ──
    char fixed[1024];
    wildcard_fixed_prefix(pattern, fixed, sizeof(fixed));

    char upper[1024];
    make_upper_bound(fixed, upper, sizeof(upper));

    WildcardPattern wc_pat;
    wildcard_parse(pattern, &wc_pat);

    // ── 5b. Identifica capturas $variable no padrão ──
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

    // ── 5c. Separa filtros de variável de filtros de campo ──
    QueryFilter field_filters[64];
    int field_filter_count = 0;
    QueryFilter var_filters[16];
    int var_filter_count = 0;

    for (int f = 0; f < filter_count; f++)
    {
        const char *k = filters[f].key;
        if (k[0] == '$' || strcmp(k, "{key}") == 0)
        {
            if (var_filter_count < 16)
                var_filters[var_filter_count++] = filters[f];
        }
        else
        {
            if (field_filter_count < 64)
                field_filters[field_filter_count++] = filters[f];
        }
    }

    // ── 6. Escaneia a tabela nodes por paths que casam o padrão ──
    sqlite3_stmt *scan_stmt;
    const char *scan_sql = "SELECT path FROM nodes WHERE path >= ?1 AND path < ?2 ORDER BY path";
    if (sqlite3_prepare_v2(db, scan_sql, -1, &scan_stmt, 0) != SQLITE_OK)
    {
        yyjson_doc_free(query_doc);
        sqlite3_result_error(context, "Failed to prepare wildcard scan", -1);
        return;
    }

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
        if (!p) continue;
        if (!wildcard_matches(p, &wc_pat)) continue;

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
            if (!passes) continue;
        }

        if (path_count >= path_cap)
        {
            path_cap *= 2;
            paths = (char **)realloc(paths, path_cap * sizeof(char *));
        }
        paths[path_count] = sqlite3_mprintf("%s", p);
        path_count++;
    }
    sqlite3_finalize(scan_stmt);

    // ── 7. Para cada path match, obtém o valor JSON ──
    sqlite3_stmt *extract_stmt;
    const char *extract_sql = "SELECT COALESCE(extract_json(?1), 'null')";
    if (sqlite3_prepare_v2(db, extract_sql, -1, &extract_stmt, 0) != SQLITE_OK)
    {
        for (int i = 0; i < path_count; i++) sqlite3_free(paths[i]);
        free(paths);
        yyjson_doc_free(query_doc);
        sqlite3_result_error(context, "Failed to prepare extract in wildcard", -1);
        return;
    }

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
                        entries[valid].val = root;
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
    sqlite3_finalize(extract_stmt);

    // ── 8. Aplica filtros de campo ──
    SortEntry *filtered = NULL;
    size_t filtered_count = 0;

    if (valid == 0)
    {
        // Nenhum resultado
        yyjson_mut_doc_free(result_doc);
        free(entries);
        free(item_docs);
        for (int i = 0; i < path_count; i++) sqlite3_free(paths[i]);
        free(paths);
        yyjson_doc_free(query_doc);
        sqlite3_result_text(context, "[]", -1, SQLITE_STATIC);
        return;
    }

    if (field_filter_count > 0)
    {
        filtered = (SortEntry *)calloc(valid, sizeof(SortEntry));
        for (size_t i = 0; i < valid; i++)
        {
            bool match = true;
            for (int f = 0; f < field_filter_count && match; f++)
            {
                if (!field_filters[f].valid) continue;
                match = evaluate_filter(entries[i].val, &field_filters[f]);
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

    // ── 9. Ordena ──
    if (order_count > 0 && filtered_count > 1)
    {
        g_sort_orders = orders;
        qsort(filtered, filtered_count, sizeof(SortEntry), compare_entries);
        g_sort_orders = NULL;
    }

    // ── 10. Aplica skip/take ──
    size_t start = skip < filtered_count ? skip : filtered_count;
    size_t end = (take < filtered_count - start) ? (start + take) : filtered_count;

    // ── 11. Monta array de resultados ──
    for (size_t i = start; i < end; i++)
    {
        if (filtered[i].val)
        {
            yyjson_mut_val *copy = yyjson_val_mut_copy(result_doc, filtered[i].val);
            if (copy)
                yyjson_mut_arr_append(result_arr, copy);
        }
    }

    char *json_out = yyjson_mut_write(result_doc, 0, NULL);
    if (json_out)
        sqlite3_result_text(context, json_out, -1, free);
    else
        sqlite3_result_null(context);

    // ── 12. Cleanup ──
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
}

/// query_json(prefix, query_json)
/// Executa uma consulta com filtros, ordenação e paginação.
/// query_json formato:
///   {
///     "filters": [{"key": "age", "op": ">", "compare": 25}],
///     "order": [{"key": "name", "ascending": true}],
///     "skip": 0,
///     "take": 10
///   }
static void query_json_func(sqlite3_context *context, int argc, sqlite3_value **argv)
{
    if (argc != 2)
    {
        sqlite3_result_error(context, "Usage: query_json(prefix, query_json)", -1);
        return;
    }

    const char *prefix_input = (const char *)sqlite3_value_text(argv[0]);
    const char *query_str = (const char *)sqlite3_value_text(argv[1]);

    if (!prefix_input || !query_str)
    {
        sqlite3_result_null(context);
        return;
    }

    // 1. Extrai o JSON completo no prefixo (todos os filhos)
    //    Usamos extract_json para obter os dados
    sqlite3 *db = sqlite3_context_db_handle(context);

    // Verifica se há wildcard multi-nível
    bool has_wildcard = path_has_wildcard(prefix_input);
    
    if (has_wildcard) {
        // ─── WILDCARD MULTI-NÍVEL: busca paths que casam o padrão ───
        do_query_wildcard(context, db, prefix_input, query_str);
        return;
    }

    // Normaliza o prefixo
    char query_prefix[1024];
    normalize_path(prefix_input, query_prefix, sizeof(query_prefix));

    // Prepara e executa extract_json via SQL interno
    sqlite3_stmt *extract_stmt;
    const char *extract_sql = "SELECT COALESCE(extract_json(?1), 'null')";
    if (sqlite3_prepare_v2(db, extract_sql, -1, &extract_stmt, 0) != SQLITE_OK)
    {
        sqlite3_result_error(context, "Failed to prepare extract", -1);
        return;
    }
    // Passa o path sem trailing / para o extract_json
    char extract_arg[1024];
    path_without_trailing_slash(query_prefix, extract_arg, sizeof(extract_arg));
    sqlite3_bind_text(extract_stmt, 1, extract_arg, -1, SQLITE_STATIC);

    yyjson_doc *data_doc = NULL;
    if (sqlite3_step(extract_stmt) == SQLITE_ROW)
    {
        const char *json_str = (const char *)sqlite3_column_text(extract_stmt, 0);
        if (json_str && strcmp(json_str, "null") != 0)
        {
            data_doc = yyjson_read(json_str, strlen(json_str), 0);
        }
    }
    sqlite3_finalize(extract_stmt);

    if (!data_doc)
    {
        sqlite3_result_null(context);
        return;
    }

    yyjson_val *data_root = yyjson_doc_get_root(data_doc);

    // 2. Parse a query JSON
    yyjson_doc *query_doc = yyjson_read(query_str, strlen(query_str), 0);
    if (!query_doc)
    {
        yyjson_doc_free(data_doc);
        sqlite3_result_error(context, "Invalid query JSON", -1);
        return;
    }
    yyjson_val *query_root = yyjson_doc_get_root(query_doc);

    // 3. Extrai filtros
    QueryFilter filters[64];
    int filter_count = 0;
    memset(filters, 0, sizeof(filters));

    yyjson_val *filters_val = yyjson_obj_get(query_root, "filters");
    if (filters_val && yyjson_is_arr(filters_val))
    {
        size_t n = yyjson_arr_size(filters_val);
        for (size_t i = 0; i < n && filter_count < 64; i++)
        {
            yyjson_val *f = yyjson_arr_get(filters_val, i);
            if (!yyjson_is_obj(f))
                continue;

            yyjson_val *key_v = yyjson_obj_get(f, "key");
            yyjson_val *op_v = yyjson_obj_get(f, "op");
            yyjson_val *cmp_v = yyjson_obj_get(f, "compare");

            if (key_v && yyjson_is_str(key_v) && op_v && yyjson_is_str(op_v))
            {
                strncpy(filters[filter_count].key, yyjson_get_str(key_v), sizeof(filters[filter_count].key) - 1);
                strncpy(filters[filter_count].op, yyjson_get_str(op_v), sizeof(filters[filter_count].op) - 1);
                if (cmp_v)
                {
                    filters[filter_count].compare_val = cmp_v;
                    if (yyjson_is_str(cmp_v))
                        strncpy(filters[filter_count].compare, yyjson_get_str(cmp_v), sizeof(filters[filter_count].compare) - 1);
                    else if (yyjson_is_num(cmp_v))
                        snprintf(filters[filter_count].compare, sizeof(filters[filter_count].compare), "%.17g", yyjson_get_num(cmp_v));
                    else if (yyjson_is_bool(cmp_v))
                        strcpy(filters[filter_count].compare, yyjson_get_bool(cmp_v) ? "true" : "false");
                }
                filters[filter_count].valid = true;
                filter_count++;
            }
        }
    }

    // 4. Extrai ordenação
    QueryOrder orders[16];
    int order_count = 0;
    memset(orders, 0, sizeof(orders));

    yyjson_val *orders_val = yyjson_obj_get(query_root, "order");
    if (orders_val && yyjson_is_arr(orders_val))
    {
        size_t n = yyjson_arr_size(orders_val);
        for (size_t i = 0; i < n && order_count < 16; i++)
        {
            yyjson_val *o = yyjson_arr_get(orders_val, i);
            if (!yyjson_is_obj(o))
                continue;
            yyjson_val *k = yyjson_obj_get(o, "key");
            yyjson_val *a = yyjson_obj_get(o, "ascending");
            if (k && yyjson_is_str(k))
            {
                strncpy(orders[order_count].key, yyjson_get_str(k), sizeof(orders[order_count].key) - 1);
                orders[order_count].ascending = !a || (yyjson_is_bool(a) && yyjson_get_bool(a)) || (yyjson_is_int(a) && yyjson_get_int(a) == 1);
                order_count++;
            }
        }
    }

    // 5. Extrai paginação
    yyjson_val *skip_val = yyjson_obj_get(query_root, "skip");
    yyjson_val *take_val = yyjson_obj_get(query_root, "take");
    size_t skip = skip_val && yyjson_is_int(skip_val) ? (size_t)yyjson_get_int(skip_val) : 0;
    size_t take = take_val && yyjson_is_int(take_val) ? (size_t)yyjson_get_int(take_val) : 0;

    // Se não tem take, retorna tudo
    if (take == 0)
        take = (size_t)-1;

    // 6. Coleta os valores do array/objeto de dados
    //    O resultado do extract_json para um prefixo geralmente é um objeto
    //    Se for array, usamos os elementos; se for objeto, usamos os valores
    SortEntry *entries = NULL;
    size_t total = 0;

    if (yyjson_is_arr(data_root))
    {
        total = yyjson_arr_size(data_root);
        entries = (SortEntry *)calloc(total, sizeof(SortEntry));
        for (size_t i = 0; i < total; i++)
        {
            entries[i].val = yyjson_arr_get(data_root, i);
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
                entries[i].val = v;
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
        entries[0].val = data_root;
        entries[0].index = 0;
    }

    // 7. Aplica filtros
    SortEntry *filtered = NULL;
    size_t filtered_count = 0;

    if (filter_count > 0)
    {
        filtered = (SortEntry *)calloc(total, sizeof(SortEntry));
        for (size_t i = 0; i < total; i++)
        {
            bool match = true;
            for (int f = 0; f < filter_count && match; f++)
            {
                if (!filters[f].valid)
                    continue;
                match = evaluate_filter(entries[i].val, &filters[f]);
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

    // 8. Ordena
    if (order_count > 0 && filtered_count > 1)
    {
        g_sort_orders = orders;
        qsort(filtered, filtered_count, sizeof(SortEntry), compare_entries);
        g_sort_orders = NULL;
    }

    // 9. Aplica skip/take
    size_t start = skip < filtered_count ? skip : filtered_count;
    size_t end = (take < filtered_count - start) ? (start + take) : filtered_count;
    size_t result_count = end - start;

    // 10. Monta resultado como array JSON
    yyjson_mut_doc *result_doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *result_arr = yyjson_mut_arr(result_doc);
    yyjson_mut_doc_set_root(result_doc, result_arr);

    for (size_t i = start; i < end; i++)
    {
        if (filtered[i].val)
        {
            yyjson_mut_val *copy = yyjson_val_mut_copy(result_doc, filtered[i].val);
            if (copy)
                yyjson_mut_arr_append(result_arr, copy);
        }
    }

    char *json_out = yyjson_mut_write(result_doc, 0, NULL);
    if (json_out)
    {
        sqlite3_result_text(context, json_out, -1, free);
    }
    else
    {
        sqlite3_result_null(context);
    }

    // Limpa
    yyjson_mut_doc_free(result_doc);
    if (filtered != entries)
        free(filtered);
    free(entries);
    yyjson_doc_free(data_doc);
    yyjson_doc_free(query_doc);
}

// =====================================================================
// REGISTRO DA EXTENSÃO
// =====================================================================
#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_extension_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi)
{
    SQLITE_EXTENSION_INIT2(pApi);

    sqlite3_create_function(db, "set_json", -1, SQLITE_UTF8, 0, set_json_func, 0, 0);
    sqlite3_create_function(db, "update_json", -1, SQLITE_UTF8, 0, update_json_func, 0, 0);
    sqlite3_create_function(db, "extract_json", -1, SQLITE_UTF8, 0, extract_json_func, 0, 0);
    sqlite3_create_function(db, "query_json", 2, SQLITE_UTF8, 0, query_json_func, 0, 0);

    // Mantém ingest_json como alias para compatibilidade reversa
    sqlite3_create_function(db, "ingest_json", -1, SQLITE_UTF8, 0, set_json_func, 0, 0);

    return SQLITE_OK;
}