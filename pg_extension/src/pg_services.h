#ifndef PG_SERVICES_H
#define PG_SERVICES_H

/*
 * pg_services.h — Declarações da camada de SERVICES (src/pg_services.c).
 *
 * Services são a camada de NEGÓCIO: gerenciam SPI_connect/SPI_finish e
 * orquestram as operações de banco (flatten/insert, merge, extract, query,
 * CSV). Recebem strings C e retornam strings palloc'd no contexto fn_mcxt
 * (o caller é dono da memória). Em caso de erro, lançam ERROR (ereport).
 */

#include "postgres.h"

#define CSV_HEADER "path,type,text_value\n"

/* set_json — replace completo; retorna revision UUID (nunca NULL). */
char *service_set_json(const char *doc_id, const char *json_str, MemoryContext fn_mcxt);

/* update_json — deep merge; retorna revision UUID ou NULL (wildcard sem alvo). */
char *service_update_json(const char *doc_id, const char *json_str, MemoryContext fn_mcxt);

/* extract_json — reconstrói o JSON do subtree; retorna NULL se não existir. */
char *service_extract_json(const char *input, MemoryContext fn_mcxt);

/* query_json — filtros/ordenação/paginação (suporta wildcard); retorna array JSON ou NULL. */
char *service_query_json(const char *prefix, const char *query_str, MemoryContext fn_mcxt);

/* export_csv — CSV (RFC 4180) do subtree. */
char *service_export_csv(const char *prefix, MemoryContext fn_mcxt);

/* import_csv — CSV → nodes; retorna revision UUID. */
char *service_import_csv(const char *prefix, const char *csv_text, MemoryContext fn_mcxt);

#endif /* PG_SERVICES_H */
