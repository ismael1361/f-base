import type Database from "libsql";
import type { Readable } from "stream";

// =====================================================================
// TIPOS AUXILIARES (paridade com src/pg-store.ts)
// =====================================================================

/** Definição de um filtro para consulta */
export interface QueryFilter {
  key: string;
  op: "<" | "<=" | "==" | "!=" | ">=" | ">" | "like" | "exists" | "!exists" | "in" | "between";
  compare: unknown;
}

/** Definição de um campo de ordenação */
export interface QueryOrder {
  key: string;
  ascending?: boolean;
}

/** Opções completas para a query */
export interface QueryOptions {
  filters?: QueryFilter[];
  order?: QueryOrder[];
  skip?: number;
  take?: number;
}

/** Header do CSV de exportação (alinhado com a constante C CSV_HEADER) */
export const CSV_HEADER = "path,type,text_value\n";

// =====================================================================
// HELPERS LEVES (Node.js) — trabalho pesado fica na extensão C
// =====================================================================

/** Converte Buffer | Readable para Buffer completo */
async function bufferFromReadable(data: Buffer | Readable): Promise<Buffer> {
  if (Buffer.isBuffer(data)) return data;
  const chunks: Buffer[] = [];
  for await (const chunk of data) {
    chunks.push(Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk));
  }
  return Buffer.concat(chunks);
}

/** Incrementa o último byte de um prefixo para gerar o upper bound do range scan. */
function upperBound(prefix: string): string {
  if (prefix.length === 0) return "\uFFFF";
  const last = prefix.charCodeAt(prefix.length - 1);
  return prefix.slice(0, -1) + String.fromCharCode(last + 1);
}

/**
 * Opções de configuração do store libSQL.
 */
export interface LibsqlStoreOptions {
  /**
   * Nº máximo de documentos mantidos no cache de leitura (FIFO).
   * Cada entrada guarda o JSON bruto reconstruído por `extract_json`
   * (um doc de ~10k nós ocupa ~1MB). Default: 64. Use 0 para desligar.
   */
  readCacheSize?: number;

  /**
   * Nome lógico da tabela (multi-tabela). Quando definido, o store opera
   * sobre `{tableName}_nodes` / `{tableName}_doc_hashes` — tabelas criadas
   * automaticamente via `register_table` (DDL idempotente no C).
   * Deve ser um identificador `[A-Za-z_][A-Za-z0-9_]{0,31}`.
   * Default: undefined → tabelas legadas `nodes`/`doc_hashes`.
   *
   * @example
   * ```ts
   * const users = new LibsqlHierarchicalStore(db, { tableName: "users" });
   * const posts = new LibsqlHierarchicalStore(db, { tableName: "posts" });
   * users.set("/x", { name: "Alice" });   // em users_nodes
   * posts.set("/x", { title: "Post" });    // em posts_nodes (mesmo path)
   * ```
   */
  tableName?: string;
}

/**
 * Store hierárquico JSON↔tabela sobre libSQL (Turso embedded), usando a
 * extensão C `hierarchical_engine` (que roda NO MESMO processo — zero IPC).
 *
 * API IDÊNTICA ao `PgHierarchicalStore`, porém SÍNCRONA: o driver libSQL
 * é in-process (better-sqlite3-compatible), então não há Promises nem
 * round-trips de rede. A exceção é `import()`, que permanece async para
 * aceitar `Readable` streams.
 *
 * ⚠️ Diferente do PG, este store NÃO faz `await` — a aplicação que migrar
 * deve remover os `await` das chamadas (set/get/update/query/export).
 *
 * @example
 * ```ts
 * const engine = new LibsqlEngine();
 * engine.start({ databasePath: "./db/f-base.db" });
 * const store = new LibsqlHierarchicalStore(engine.db);
 * store.set("/users/100", { name: "Alice" });   // síncrono
 * const user = store.get("/users/100");
 * engine.stop();
 * ```
 */
export class LibsqlHierarchicalStore {
  private db: Database.Database;
  /** Nome lógico da tabela ("" = par default nodes/doc_hashes). */
  private readonly table: string;
  /** Nome físico da tabela de nós (ex.: "users_nodes"). */
  private readonly nodesTable: string;
  private stmtSet: Database.Statement<unknown[]>;
  private stmtSetInline: Database.Statement<unknown[]>;
  private stmtGet: Database.Statement<unknown[]>;
  private stmtGetNode: Database.Statement<unknown[]>;
  private stmtGetRevision: Database.Statement<unknown[]>;
  private stmtUpdate: Database.Statement<unknown[]>;
  private stmtQuery: Database.Statement<unknown[]>;
  private stmtSearch: Database.Statement<unknown[]>;
  private stmtImportCsv: Database.Statement<unknown[]>;
  private stmtExportCsv: Database.Statement<unknown[]>;
  private stmtExportJson: Database.Statement<unknown[]>;

  /**
   * Cache de leitura (mini-cache temporário): chave = path do container
   * raiz (com trailing slash), valor = revision daquele nó + JSON bruto
   * devolvido por `extract_json`. Como TODA escrita reescreve o nó raiz
   * do documento com uma revision NOVA (set/update/import), a comparação
   * de revision invalida o cache automaticamente — e o dedup do `set`
   * (mesmo conteúdo → mesma revision) preserva o cache.
   */
  private readCache: Map<string, { rev: string | null; json: string | null }>;
  private readonly readCacheSize: number;

  /**
   * @param db - Instância `Database` do libSQL (ver LibsqlEngine).
   * @param options - Opções (cache de leitura e nome da tabela).
   *
   * Os statements são preparados UMA VEZ no construtor e reutilizados —
   * elimina o custo de prepare/finalize por chamada (overhead N-API).
   * Com `tableName`, o par `{t}_nodes`/`{t}_doc_hashes` é criado via
   * `register_table` (DDL idempotente) e a tabela é passada como 1º
   * argumento das funções de extensão.
   */
  constructor(db: Database.Database, options: LibsqlStoreOptions = {}) {
    this.db = db;

    // Valida o nome da tabela como identificador (paridade com o C)
    const t = options.tableName ?? "main";
    if (t !== "" && !/^[A-Za-z_][A-Za-z0-9_]{0,31}$/.test(t)) {
      throw new Error(`Invalid tableName: ${t}`);
    }
    this.table = t;
    this.nodesTable = t !== "" ? `${t}_nodes` : "nodes";

    // DDL idempotente das tabelas customizadas (feito no C)
    db.exec(`SELECT register_table('${t}')`);

    // Statements de extensão — tabela como 1º argumento quando customizada.
    // Sem tabela: mantém a assinatura legada (compatibilidade total).
    this.stmtSet = db.prepare(t !== "" ? "SELECT set_json(?, ?, ?) AS revision" : "SELECT set_json(?, ?) AS revision");
    this.stmtSetInline = db.prepare(t !== "" ? "SELECT set_json(?, ?, ?, ?) AS revision" : "SELECT set_json(?, ?, ?) AS revision");
    this.stmtGet = db.prepare(t !== "" ? "SELECT extract_json(?, ?) AS json_data" : "SELECT extract_json(?) AS json_data");
    this.stmtGetNode = db.prepare(`SELECT type, text_value FROM ${this.nodesTable} WHERE path = ?`);
    this.stmtGetRevision = db.prepare(`SELECT revision FROM ${this.nodesTable} WHERE path = ?`);
    this.stmtUpdate = db.prepare(t !== "" ? "SELECT update_json(?, ?, ?) AS revision" : "SELECT update_json(?, ?) AS revision");
    this.stmtQuery = db.prepare(t !== "" ? "SELECT query_json(?, ?, ?) AS json_data" : "SELECT query_json(?, ?) AS json_data");
    this.stmtSearch = db.prepare(`SELECT path, type, text_value FROM ${this.nodesTable} WHERE path >= ? AND path < ? ORDER BY path`);
    this.stmtImportCsv = db.prepare(t !== "" ? "SELECT import_csv(?, ?, ?)" : "SELECT import_csv(?, ?)");
    this.stmtExportCsv = db.prepare(t !== "" ? "SELECT export_csv(?, ?) AS csv_data" : "SELECT export_csv(?) AS csv_data");
    this.stmtExportJson = db.prepare(t !== "" ? "SELECT extract_json(?, ?) AS json_data" : "SELECT extract_json(?) AS json_data");
    this.readCacheSize = Math.max(0, Math.floor(options.readCacheSize ?? 64));
    this.readCache = new Map();
  }

  /**
   * Define (substitui) um documento JSON em um caminho.
   * O path DEVE começar com "/". `null` remove o documento.
   *
   * O motor C aplica DEDUP: se o conteúdo (e maxInlineSize) forem
   * idênticos ao último write do path, a escrita é pulada e a revision
   * existente é devolvida (escrita idempotente).
   *
   * @param maxInlineSize - (opcional) Tamanho máximo de string para
   *   armazenar inline no nó pai (default 0 = cada primitivo vira nó
   *   dedicado). Valores >0 reduzem drasticamente o nº de linhas para
   *   documentos grandes (ex.: 128 → ~5× menos nós), acelerando
   *   set/get/query. ATENÇÃO: primitivos inline NÃO têm path próprio.
   *
   * @example
   * ```ts
   * store.set("/users/100", { name: "Alice", age: 30 });
   * store.set("/users/100", { name: "Alice", age: 30 }, 128); // inline
   * store.set("/users/100", null); // remove o documento
   * ```
   */
  public set<T = Record<string, unknown>>(prefix: string, data: T | null, maxInlineSize?: number): string {
    const json = JSON.stringify(data);
    const key = prefix.endsWith("/") ? prefix : prefix + "/";
    // Lê a revision ATUAL antes do write (para detectar dedup e manter o cache)
    const old = this.stmtGetRevision.get(key) as { revision: string | null } | undefined;
    const oldRev = old?.revision ?? null;

    const hasT = this.table !== "";
    const row = maxInlineSize
      ? hasT
        ? (this.stmtSetInline.get(this.table, prefix, json, maxInlineSize) as { revision: string } | undefined)
        : (this.stmtSetInline.get(prefix, json, maxInlineSize) as { revision: string } | undefined)
      : hasT
        ? (this.stmtSet.get(this.table, prefix, json) as { revision: string } | undefined)
        : (this.stmtSet.get(prefix, json) as { revision: string } | undefined);
    const revision = row?.revision ?? "";

    // Dedup (conteúdo idêntico → mesma revision): o cache continua válido.
    // Caso contrário, invalida o path escrito e todos os ancestrais
    // (o JSON de um container pai muda quando um descendente muda).
    if (oldRev === null || oldRev !== revision) this.invalidateCacheKeys(key);

    return revision;
  }

  /**
   * Obtém um documento JSON de um caminho.
   * O path DEVE começar com "/".
   *
   * Usa o mini-cache de leitura: 1 lookup B-tree pela revision do nó raiz
   * do container; se a revision casar com a entrada em cache, devolve o
   * JSON parseado sem re-extrair do banco (O(1) em vez do range scan de
   * todos os nós do documento).
   *
   * @example
   * ```ts
   * const user = store.get("/users/100");
   * console.log(user.name); // "Alice"
   * ```
   */
  public get<T = Record<string, unknown>>(prefix: string): T | null {
    // Chave do cache: path do container raiz (normalizado com trailing slash)
    const key = prefix.endsWith("/") ? prefix : prefix + "/";
    const hasT = this.table !== "";

    // 1 lookup B-tree: revision do container raiz (invalidação automática)
    const revRow = this.stmtGetRevision.get(key) as { revision: string | null } | undefined;
    if (revRow && revRow.revision !== null) {
      const hit = this.readCache.get(key);
      if (hit && hit.rev === revRow.revision) {
        return hit.json !== null ? (JSON.parse(hit.json) as T) : null;
      }
      const row = (hasT ? this.stmtGet.get(this.table, prefix) : this.stmtGet.get(prefix)) as { json_data: string | null } | undefined;
      const jsonData = row?.json_data ?? null;
      this.cacheSet(key, { rev: revRow.revision, json: jsonData });
      return jsonData !== null ? (JSON.parse(jsonData) as T) : null;
    }

    // Fallback (sem cache): o path pode apontar para um NÓ PRIMITIVO
    // (ex: "/users/100/name"), armazenado sem trailing slash — extract_json
    // só reconstrói containers.
    if (prefix.endsWith("/")) return null;
    const node = this.stmtGetNode.get(prefix) as { type: number; text_value: string | null } | undefined;
    if (!node) return null;
    const { type, text_value } = node;
    switch (type) {
      case 3: // NUMBER
        return JSON.parse(text_value ?? "0") as T;
      case 4: // BOOLEAN
        return (text_value === "true") as unknown as T;
      case 5: // STRING (text_value é JSON escapado, ex: '"Alan"')
        return JSON.parse(text_value ?? '""') as T;
      default: // DATETIME/BIGINT/BINARY/REFERENCE/EMPTY
        return (text_value ?? null) as unknown as T;
    }
  }

  /** Insere entrada no cache de leitura com eviction FIFO (limite de itens). */
  private cacheSet(key: string, entry: { rev: string | null; json: string | null }): void {
    if (this.readCacheSize <= 0) return;
    if (this.readCache.size >= this.readCacheSize) {
      const first = this.readCache.keys().next().value;
      if (first !== undefined) this.readCache.delete(first);
    }
    this.readCache.set(key, entry);
  }

  /**
   * Invalida o cache de leitura para o path escrito e TODOS os ancestrais.
   * Ex: "/users/100/" → remove "/users/100/", "/users/" e "/".
   * (O JSON de um container pai muda quando qualquer descendente muda.)
   */
  private invalidateCacheKeys(key: string): void {
    const parts = key.split("/");
    let cur = "";
    for (const seg of parts) {
      if (seg === "") continue;
      cur += "/" + seg;
      this.readCache.delete(cur + "/");
    }
    this.readCache.delete("/"); // container raiz
  }

  /**
   * Faz merge (update parcial) de um JSON em um ou mais documentos.
   * Preserva chaves existentes não mencionadas. Para remover uma chave,
   * defina seu valor como `null`.
   *
   * @example
   * ```ts
   * store.update("/users/100", { age: 31 });
   * store.update("/people/*", { active: true }); // em massa
   * ```
   */
  public update<T = Record<string, unknown>>(prefix: string, data: T): string {
    const json = JSON.stringify(data);
    const row = (this.table !== "" ? this.stmtUpdate.get(this.table, prefix, json) : this.stmtUpdate.get(prefix, json)) as { revision: string } | undefined;
    // Update sempre pode mudar o documento → invalida o path e ancestrais
    this.invalidateCacheKeys(prefix.endsWith("/") ? prefix : prefix + "/");
    return row?.revision ?? "";
  }

  /**
   * Executa uma consulta com filtros, ordenação e paginação nos filhos
   * de um caminho. Use `/*` no final do path para coleção.
   *
   * @example
   * ```ts
   * const result = store.query("/people/*", {
   *   filters: [{ key: "age", op: ">", compare: 25 }],
   *   order: [{ key: "name", ascending: true }],
   *   take: 10,
   * });
   * ```
   */
  public query<T = Record<string, unknown>>(prefix: string, options: QueryOptions = {}): T[] {
    const json = JSON.stringify(options);
    const row = (this.table !== "" ? this.stmtQuery.get(this.table, prefix, json) : this.stmtQuery.get(prefix, json)) as { json_data: string | null } | undefined;
    const jsonData = row?.json_data;
    if (!jsonData) return [];
    return JSON.parse(jsonData) as T[];
  }

  // ===================================================================
  // Métodos legados (compatibilidade)
  // ===================================================================

  /** @deprecated Use `set()` */
  public saveDocument<T = Record<string, unknown>>(prefix: string, data: T | null): void {
    this.set(prefix, data);
  }

  /** @deprecated Use `get()` */
  public getDocument<T = Record<string, unknown>>(prefix: string): T | null {
    return this.get<T>(prefix);
  }

  /**
   * Busca hierárquica raw usando prefix range indexado (range scan).
   * `path >= ? AND path < upperBound` usa a PRIMARY KEY (B-tree) com
   * certeza — mais eficiente que `LIKE ? || '%'` (concatenação runtime).
   */
  public searchByPath(pathPrefix: string): Array<{ path: string; type: number; text_value: string | null }> {
    // Ajusta o prefixo: se não termina com '/', adiciona para capturar descendentes
    const prefix = pathPrefix.endsWith("/") ? pathPrefix : pathPrefix + "/";
    const rows = this.stmtSearch.all(prefix, upperBound(prefix)) as Array<{
      path: string;
      type: number;
      text_value: string | null;
    }>;
    return rows;
  }

  /**
   * Importa dados JSON ou CSV para o prefixo especificado.
   *
   * @param pathPrefix - Prefixo do caminho onde os dados serão importados.
   * @param data - Conteúdo a ser importado (Buffer ou Readable).
   * @param type - Tipo de dados ("json" ou "csv").
   *
   * @example
   * ```ts
   * await store.import("/users", Buffer.from(JSON.stringify([sampleUser])), "json");
   * ```
   */
  public async import(pathPrefix: string, data: Buffer | Readable, type: "json" | "csv" = "json"): Promise<void> {
    const rawContent = await bufferFromReadable(data);

    if (type === "json") {
      const parsed = JSON.parse(rawContent.toString("utf-8"));
      this.set(pathPrefix, parsed);
    } else {
      const csv = rawContent.toString("utf-8");
      this.table !== "" ? this.stmtImportCsv.get(this.table, pathPrefix, csv) : this.stmtImportCsv.get(pathPrefix, csv);
    }
  }

  /**
   * Exporta dados JSON ou CSV do prefixo especificado.
   *
   * @param pathPrefix - Prefixo do caminho de onde os dados serão exportados.
   * @param type - Tipo de dados ("json" ou "csv").
   * @returns Conteúdo exportado como Buffer.
   */
  public export(pathPrefix: string, type: "json" | "csv" = "json"): Buffer {
    if (type === "csv") {
      const row = (this.table !== "" ? this.stmtExportCsv.get(this.table, pathPrefix) : this.stmtExportCsv.get(pathPrefix)) as { csv_data: string | null } | undefined;
      return Buffer.from(row?.csv_data ?? CSV_HEADER, "utf-8");
    }
    const row = (this.table !== "" ? this.stmtExportJson.get(this.table, pathPrefix) : this.stmtExportJson.get(pathPrefix)) as { json_data: string | null } | undefined;
    return Buffer.from(row?.json_data ?? "null", "utf-8");
  }

  /**
   * Executa múltiplas operações em uma transação atômica.
   * Se qualquer operação lançar, TODAS as anteriores são revertidas (ROLLBACK).
   *
   * A extensão C respeita transações externas (via `sqlite3_get_autocommit`),
   * então `set`/`update` podem ser chamados dentro do callback.
   *
   * @example
   * ```ts
   * store.transaction((tx) => {
   *   tx.set("/a", { x: 1 });
   *   tx.set("/b", { y: 2 });
   * });
   * ```
   */
  public transaction<T>(fn: (tx: LibsqlHierarchicalStore) => T): T {
    const run = this.db.transaction(() => fn(this));
    return run();
  }
}

export default LibsqlHierarchicalStore;
