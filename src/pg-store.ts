import type { Client } from "pg";
import type { Readable } from "stream";

// =====================================================================
// TIPOS AUXILIARES (paridade com src/index.ts original)
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

/**
 * Store hierárquico JSON↔tabela sobre PostgreSQL, usando a extensão C
 * `hierarchical_engine` (que roda no processo do servidor via SPI).
 *
 * API idêntica ao `HierarchicalStore` do SQLite, porém assíncrona
 * (o acesso é via node-postgres sobre TCP loopback).
 */
export class PgHierarchicalStore {
  private client: Client;

  /**
   * @param client - Cliente node-postgres conectado (ver PgEngine).
   */
  constructor(client: Client) {
    this.client = client;
  }

  /**
   * Define (substitui) um documento JSON em um caminho.
   * O path DEVE começar com "/". `null` remove o documento.
   *
   * @example
   * ```ts
   * await store.set("/users/100", { name: "Alice", age: 30 });
   * await store.set("/users/100", null); // remove o documento
   * ```
   */
  public async set<T = Record<string, unknown>>(prefix: string, data: T | null): Promise<string> {
    const res = await this.client.query("SELECT set_json($1, $2) AS revision", [prefix, JSON.stringify(data)]);
    return (res.rows[0]?.revision as string | undefined) ?? "";
  }

  /**
   * Obtém um documento JSON de um caminho.
   * O path DEVE começar com "/".
   *
   * @example
   * ```ts
   * const user = await store.get("/users/100");
   * console.log(user.name); // "Alice"
   * ```
   */
  public async get<T = Record<string, unknown>>(prefix: string): Promise<T | null> {
    const res = await this.client.query("SELECT extract_json($1) AS json_data", [prefix]);
    const jsonData = res.rows[0]?.json_data as string | null | undefined;
    if (jsonData) return JSON.parse(jsonData) as T;

    // Fallback: o path pode apontar para um NÓ PRIMITIVO (ex: "/users/100/name"),
    // armazenado sem trailing slash — extract_json só reconstrói containers.
    if (prefix.endsWith("/")) return null;
    const node = await this.client.query<{ type: number; text_value: string | null }>(
      "SELECT type, text_value FROM nodes WHERE path = $1",
      [prefix],
    );
    if (node.rowCount === 0) return null;
    const { type, text_value } = node.rows[0];
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

  /**
   * Faz merge (update parcial) de um JSON em um ou mais documentos.
   * Preserva chaves existentes não mencionadas. Para remover uma chave,
   * defina seu valor como `null`.
   *
   * @example
   * ```ts
   * await store.update("/users/100", { age: 31 });
   * await store.update("/people/*", { active: true }); // em massa
   * ```
   */
  public async update<T = Record<string, unknown>>(prefix: string, data: T): Promise<string> {
    const res = await this.client.query("SELECT update_json($1, $2) AS revision", [prefix, JSON.stringify(data)]);
    return (res.rows[0]?.revision as string | undefined) ?? "";
  }

  /**
   * Executa uma consulta com filtros, ordenação e paginação nos filhos
   * de um caminho. Use `/*` no final do path para coleção.
   *
   * @example
   * ```ts
   * const result = await store.query("/people/*", {
   *   filters: [{ key: "age", op: ">", compare: 25 }],
   *   order: [{ key: "name", ascending: true }],
   *   take: 10,
   * });
   * ```
   */
  public async query<T = Record<string, unknown>>(prefix: string, options: QueryOptions = {}): Promise<T[]> {
    const res = await this.client.query("SELECT query_json($1, $2) AS json_data", [prefix, JSON.stringify(options)]);
    const jsonData = res.rows[0]?.json_data as string | null | undefined;
    if (!jsonData) return [];
    return JSON.parse(jsonData) as T[];
  }

  // ===================================================================
  // Métodos legados (compatibilidade)
  // ===================================================================

  /** @deprecated Use `set()` */
  public async saveDocument<T = Record<string, unknown>>(prefix: string, data: T | null): Promise<void> {
    await this.set(prefix, data);
  }

  /** @deprecated Use `get()` */
  public async getDocument<T = Record<string, unknown>>(prefix: string): Promise<T | null> {
    return this.get<T>(prefix);
  }

  /** Busca hierárquica raw usando prefix range (LIKE prefix — robusto p/ UTF-8) */
  public async searchByPath(
    pathPrefix: string,
  ): Promise<Array<{ path: string; type: number; text_value: string | null }>> {
    // Escapa wildcards do LIKE (%, _, \) para que o prefixo seja literal
    const escaped = pathPrefix.replace(/[\\%_]/g, (c) => `\\${c}`);
    const res = await this.client.query(
      `SELECT path, type, text_value
       FROM nodes
       WHERE path LIKE $1 || '%' ESCAPE '\\'
       ORDER BY path`,
      [escaped],
    );
    return res.rows as Array<{ path: string; type: number; text_value: string | null }>;
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
      await this.set(pathPrefix, parsed);
    } else {
      await this.client.query("SELECT import_csv($1, $2)", [pathPrefix, rawContent.toString("utf-8")]);
    }
  }

  /**
   * Exporta dados JSON ou CSV do prefixo especificado.
   *
   * @param pathPrefix - Prefixo do caminho de onde os dados serão exportados.
   * @param type - Tipo de dados ("json" ou "csv").
   * @returns Conteúdo exportado como Buffer.
   */
  public async export(pathPrefix: string, type: "json" | "csv" = "json"): Promise<Buffer> {
    if (type === "csv") {
      const res = await this.client.query("SELECT export_csv($1) AS csv_data", [pathPrefix]);
      return Buffer.from((res.rows[0]?.csv_data as string | undefined) ?? CSV_HEADER, "utf-8");
    }
    const res = await this.client.query("SELECT extract_json($1) AS json_data", [pathPrefix]);
    return Buffer.from((res.rows[0]?.json_data as string | undefined) ?? "null", "utf-8");
  }

  /**
   * Executa múltiplas operações em uma transação atômica.
   * Se qualquer operação lançar, TODAS as anteriores são revertidas (ROLLBACK).
   *
   * @example
   * ```ts
   * await store.transaction(async (tx) => {
   *   await tx.set("/a", { x: 1 });
   *   await tx.set("/b", { y: 2 });
   * });
   * ```
   */
  public async transaction<T>(fn: (tx: PgHierarchicalStore) => Promise<T>): Promise<T> {
    await this.client.query("BEGIN");
    try {
      const result = await fn(new PgHierarchicalStore(this.client));
      await this.client.query("COMMIT");
      return result;
    } catch (err) {
      await this.client.query("ROLLBACK").catch(() => undefined);
      throw err;
    }
  }
}

export default PgHierarchicalStore;
