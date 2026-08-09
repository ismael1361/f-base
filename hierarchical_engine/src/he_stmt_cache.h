#ifndef HE_STMT_CACHE_H
#define HE_STMT_CACHE_H

/*
 * he_stmt_cache.h — Cache de prepared statements por conexão.
 *
 * Os services (set/update/extract/query/export/import) e o mapper
 * preparavam o MESMO SQL repetidamente (até por container no flatten).
 * Este módulo prepara uma vez por statement SQL externo e reutiliza via
 * sqlite3_get_auxdata/set_auxdata — o ciclo de vida é gerenciado pelo
 * SQLite (destrutor chamado quando o statement externo é finalizado).
 *
 * MULTI-TABELA: cada conexão pode usar N pares de tabelas
 * `{table}_nodes` / `{table}_doc_hashes` (além do par default
 * `nodes`/`doc_hashes`). Um HeStmtRegistry (ancorado no auxdata slot 0)
 * guarda um HeStmtCache POR NOME DE TABELA, preparado no primeiro uso.
 * As funções SQL recebem o nome da tabela como 1º argumento (identificado
 * por NÃO começar com '/') e selecionam o cache correspondente — zero
 * prepares no hot path por tabela.
 *
 * As funções SQL registradas (set_json, update_json, extract_json,
 * query_json, export_csv, import_csv) compartilham o mesmo registry
 * porque cada função tem seu próprio sqlite3_context.
 */

#include "he_types.h"

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_context sqlite3_context;
typedef struct sqlite3_stmt sqlite3_stmt;

/* ===========================================================================
 * ESTRUTURA DO CACHE (por tabela)
 * ===========================================================================
 */
typedef struct
{
  sqlite3 *db;                /* conexão dona dos statements */
  sqlite3_stmt *insert;       /* INSERT OR REPLACE (com subquery created — fast path update) */
  sqlite3_stmt *insert_new;   /* INSERT OR REPLACE SEM subquery (subtree já deletado) */
  sqlite3_stmt *check;        /* SELECT 1 FROM {t}_nodes WHERE path = ? */
  sqlite3_stmt *update_text;  /* UPDATE {t}_nodes SET text_value=? WHERE path=? */
  sqlite3_stmt *update_empty; /* UPDATE {t}_nodes SET text_value='{}' WHERE path=? AND text_value IS NULL */
  sqlite3_stmt *delete_range; /* DELETE FROM {t}_nodes WHERE path>=? AND path<? */
  sqlite3_stmt *delete_exact; /* DELETE ... path=? OR (path>=?||'/' AND path<?||'0') */
  sqlite3_stmt *revision;     /* SELECT revision_nr FROM {t}_nodes WHERE path = ? */
  sqlite3_stmt *type;         /* SELECT type FROM {t}_nodes WHERE path = ? */
  sqlite3_stmt *extract;      /* SELECT COALESCE(extract_json(?1),'null') */
  sqlite3_stmt *scan;         /* SELECT path,type,text_value FROM {t}_nodes ... range */
  sqlite3_stmt *scan_path;    /* SELECT path FROM {t}_nodes ... range (wildcard) */

  /* Dedup de set ({t}_doc_hashes): hash de conteúdo + revision do último write */
  sqlite3_stmt *doc_hash_get; /* SELECT h.hash,h.revision FROM {t}_doc_hashes h JOIN {t}_nodes ... */
  sqlite3_stmt *doc_hash_set; /* INSERT OR REPLACE INTO {t}_doc_hashes ... */
  sqlite3_stmt *doc_hash_del; /* DELETE FROM {t}_doc_hashes WHERE path = ? */
} HeStmtCache;

/* ===========================================================================
 * REGISTRY (registry de caches por tabela — auxdata slot 0)
 * ===========================================================================
 */
#define HE_MAX_TABLES 16

typedef struct
{
  sqlite3 *db;                         /* conexão dona dos caches */
  char table_names[HE_MAX_TABLES][32]; /* "" = default (nodes/doc_hashes) */
  HeStmtCache *caches[HE_MAX_TABLES];
  int count;
} HeStmtRegistry;

/* ===========================================================================
 * ACESSO
 * ===========================================================================
 */
/* Obtém (criando se necessário) o cache de statements para a tabela
 * indicada. table_name == NULL ou "" usa o par default (nodes/doc_hashes).
 * O nome é VALIDADO como identificador [A-Za-z_][A-Za-z0-9_]* (máx 31
 * chars) antes de ser interpolado no SQL — nunca confie no caller.
 * O cache fica associado ao registry (auxdata slot 0) e é liberado
 * automaticamente pelo SQLite. Em falha, define *err (alocado com
 * sqlite3_malloc) e retorna NULL. */
HeStmtCache *he_stmt_cache_get(sqlite3 *db, sqlite3_context *ctx,
                               const char *table_name, char **err);

/* Valida um nome de tabela (1 = válido/default, 0 = inválido). */
int he_validate_table_name(const char *table_name);

/* Destrutor de um cache individual (finaliza os statements e libera). */
void he_stmt_cache_destroy(void *ptr);

#endif /* HE_STMT_CACHE_H */
