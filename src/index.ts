import Database from "better-sqlite3";
import path from "path";

// 1. Inicialização do Banco e Schema
const db = new Database("db/hierarchical.db", { verbose: console.log });

// Configurações de Performance (O "Sweet Spot" do SQLite)
db.pragma("journal_mode = WAL");
db.pragma("synchronous = NORMAL");
db.pragma("mmap_size = 268435456"); // 256MB
db.pragma("cache_size = -64000"); // 64MB

// Criação das Tabelas (Foco no WITHOUT ROWID para B-Tree otimizada)
db.exec(`
    CREATE TABLE IF NOT EXISTS nodes (
        path TEXT PRIMARY KEY,
        type INTEGER NOT NULL, 
        text_value TEXT,
        created INTEGER NOT NULL,
        modified INTEGER NOT NULL,
        revision_nr INTEGER NOT NULL,
        revision TEXT NOT NULL
    ) WITHOUT ROWID;
`);

// 2. Carregamento da Extensão C
// O caminho deve apontar para o arquivo compilado (.so, .dylib ou .dll)
const extPath = path.resolve(process.cwd(), "./hierarchical_engine/bin", `hierarchical_engine.${process.platform === "win32" ? "dll" : process.platform === "darwin" ? "dylib" : "so"}`);
console.log(`Tentando carregar extensão C de: ${extPath}`);
try {
  (db as any).loadExtension(extPath, "sqlite3_hierarchical_init");
  console.log("✅ Extensão C carregada com sucesso!");
} catch (err) {
  console.error("❌ Falha ao carregar extensão C:", err);
  process.exit(1);
}

// =====================================================================
// TIPOS AUXILIARES
// =====================================================================

/** Definição de um filtro para consulta */
interface QueryFilter {
  key: string;
  op: "<" | "<=" | "==" | "!=" | ">=" | ">" | "like" | "exists" | "!exists" | "in" | "between";
  compare: unknown;
}

/** Definição de um campo de ordenação */
interface QueryOrder {
  key: string;
  ascending?: boolean;
}

/** Opções completas para a query */
interface QueryOptions {
  filters?: QueryFilter[];
  order?: QueryOrder[];
  skip?: number;
  take?: number;
}

// =====================================================================
// 3. API de Alto Nível (TypeScript)
// =====================================================================

class HierarchicalStore {
  /**
   * Define (substitui) um documento JSON em um caminho.
   * O path DEVE começar com "/".
   *
   * @example
   * ```ts
   * store.set("/users/100", { name: "Alice", age: 30 });
   * store.set("/users/100", null); // remove o documento
   * ```
   */
  public set(docId: string, jsonData: Record<string, unknown> | unknown[] | null): string {
    // Garante que o path comece com /
    const normalizedId = docId.startsWith("/") ? docId.slice(1) : docId;
    const jsonStr = JSON.stringify(jsonData);
    const row = db.prepare("SELECT set_json(?, ?) as revision").get(normalizedId, jsonStr) as { revision: string } | undefined;
    return row?.revision ?? "";
  }

  /**
   * Obtém um documento JSON de um caminho.
   * O path DEVE começar com "/".
   *
   * @example
   * ```ts
   * const user = store.get("/users/100");
   * console.log(user.name); // "Alice"
   * ```
   */
  public get<T = Record<string, unknown>>(prefix: string): T | null {
    const result = db.prepare("SELECT extract_json(?) as json_data").get(prefix) as { json_data: string | null } | undefined;
    if (!result?.json_data) return null;
    return JSON.parse(result.json_data) as T;
  }

  /**
   * Faz merge (update parcial) de um JSON em um ou mais documentos.
   *
   * - Sem wildcard: aplica o merge ao documento específico.
   * - Com `/*` no final: aplica o merge a CADA filho direto do container.
   *
   * Preserva chaves existentes não mencionadas em jsonData.
   * Para remover uma chave, defina seu valor como `null` no jsonData.
   * O path DEVE começar com "/".
   *
   * @example
   * ```ts
   * // Update em um documento específico
   * store.update("/users/100", { age: 31, city: "NYC" });
   *
   * // Update em massa: adiciona { active: true } a TODOS os filhos de /people/
   * store.update("/people/*", { active: true });
   * ```
   */
  public update(docId: string, jsonData: Record<string, unknown>): string {
    const jsonStr = JSON.stringify(jsonData);
    const row = db.prepare("SELECT update_json(?, ?) as revision").get(docId, jsonStr) as { revision: string } | undefined;
    return row?.revision ?? "";
  }

  /**
   * Executa uma consulta com filtros, ordenação e paginação nos filhos
   * de um caminho.
   *
   * Cada filho direto do prefixo é tratado como um item da coleção.
   * Use `/*` no final do path para tornar explícita a semântica de coleção.
   * O path DEVE começar com "/".
   *
   * @example
   * ```ts
   * const result = store.query("/people/*", {
   *   filters: [{ key: "age", op: ">", compare: 25 }],
   *   order: [{ key: "name", ascending: true }],
   *   skip: 0,
   *   take: 10,
   * });
   * ```
   */
  public query<T = Record<string, unknown>>(prefix: string, options: QueryOptions = {}): T[] {
    const queryStr = JSON.stringify(options);
    const result = db.prepare("SELECT query_json(?, ?) as json_data").get(prefix, queryStr) as { json_data: string | null } | undefined;
    if (!result?.json_data) return [];
    return JSON.parse(result.json_data) as T[];
  }

  // ===================================================================
  // Métodos legados (compatibilidade)
  // ===================================================================

  /** @deprecated Use `set()` */
  public saveDocument(docId: string, jsonData: Record<string, unknown> | null): void {
    this.set(docId, jsonData);
  }

  /** @deprecated Use `get()` */
  public getDocument<T = Record<string, unknown>>(prefix: string): T | null {
    return this.get<T>(prefix);
  }

  /** Busca hierárquica raw usando Prefix Range Trick (sem reconstrução JSON) */
  public searchByPath(pathPrefix: string): Array<{ path: string; type: number; text_value: string | null }> {
    const upperBound = pathPrefix.slice(0, -1) + String.fromCharCode(pathPrefix.charCodeAt(pathPrefix.length - 1) + 1);

    return db
      .prepare(
        `SELECT path, type, text_value
         FROM nodes
         WHERE path >= ? AND path < ?
         ORDER BY path`,
      )
      .all(pathPrefix, upperBound) as Array<{ path: string; type: number; text_value: string | null }>;
  }
}

// =====================================================================
// PROVA DE CONCEITO (PoC)
// =====================================================================
const store = new HierarchicalStore();

// ─── 1. SET (escrita inicial) ────────────────────────────────────────
const sampleUser = {
  id: 100,
  name: "Alan Turing",
  address: {
    city: "Londres",
    zip: "12345",
  },
  tags: ["genius", "computer"],
};

console.log("\n─── 1. SET (escrita) ─────────────────────────────");
const revSet = store.set("/users/100", sampleUser);
console.log(`✅ set("/users/100", …)`);
console.log(`   Revision: ${revSet}`);

// ─── 2. GET (leitura) ────────────────────────────────────────────────
console.log("\n─── 2. GET (leitura) ─────────────────────────────");
const user = store.get("/users/100");
console.log("✅ get('/users/100') →", JSON.stringify(user, null, 2));

// ─── 3. UPDATE (merge parcial - documento específico) ────────────────
console.log("\n─── 3. UPDATE (documento específico) ─────────────");
const revUpd = store.update("/users/100", {
  name: "Alan M. Turing",
  occupation: "Mathematician",
});
console.log(`✅ update("/users/100", { name, occupation })`);
console.log(`   Revision: ${revUpd}`);

const updatedUser = store.get("/users/100");
console.log("   Resultado:", JSON.stringify(updatedUser, null, 2));

// ─── 4. QUERY com / * wildcard ──────────────────────────────────────
console.log("\n─── 4. QUERY com /* wildcard ─────────────────────");

// Cria uma coleção de pessoas
store.set("/people/alice", { name: "Alice", age: 25, city: "Londres" });
store.set("/people/bob", { name: "Bob", age: 35, city: "NYC" });
store.set("/people/charlie", { name: "Charlie", age: 20, city: "Londres" });
store.set("/people/diana", { name: "Diana", age: 28, city: "NYC" });
store.set("/people/ethan", { name: "Ethan", age: 42, city: "Londres" });
console.log("✅ Inseridos 5 registros em /people/*");

const result = store.query("/people/*", {
  filters: [
    { key: "age", op: ">", compare: 22 },
    { key: "city", op: "==", compare: "Londres" },
  ],
  order: [{ key: "age", ascending: true }],
  skip: 0,
  take: 10,
});

console.log("   query(/people/*, { age > 22, city == 'Londres' }, order=age):");
for (const r of result) {
  console.log(`     - ${JSON.stringify(r)}`);
}

// ─── 5. UPDATE com / * wildcard (aplica a TODOS os filhos) ──────────
console.log("\n─── 5. UPDATE em massa com /* wildcard ───────────");
const revMass = store.update("/people/*", { active: true, updated: true });
console.log(`✅ update("/people/*", { active: true })`);
console.log(`   Revision: ${revMass}`);

console.log("   Resultado após update em massa:");
const allPeople = store.get("/people");
if (allPeople) {
  for (const [key, val] of Object.entries(allPeople)) {
    console.log(`     ${key}: ${JSON.stringify(val)}`);
  }
}

// ─── 6. DELETE via SET(null) ─────────────────────────────────────────
console.log("\n─── 6. DELETE via set(null) ──────────────────────");
store.set("/users/100", null);
console.log('✅ set("/users/100", null)');

const deleted = store.get("/users/100");
console.log("   get após delete →", deleted);

// ─── 7. RAW SEARCH (Prefix Range Trick) ──────────────────────────────
console.log("\n─── 7. Raw Search (Prefix Range Trick) ───────────");
const children = store.searchByPath("/people/");
console.log("   Filhos de /people/:");
for (const c of children) {
  console.log(`     path=${c.path.padEnd(30)} type=${c.type}`);
}

console.log("\n─── PoC concluída com sucesso! ───────────────────");

db.close();
