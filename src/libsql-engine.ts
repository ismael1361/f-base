import path from "path";
import fs from "fs";
import Database from "libsql";

/**
 * Opções de inicialização do motor libSQL (Turso embedded).
 */
export interface LibsqlEngineOptions {
  /** Caminho do arquivo de banco (default: ./db/f-base.db). */
  databasePath?: string;
  /** Se true, usa `:memory:` (útil para testes/bench). */
  memory?: boolean;
  /** Desliga logs de progresso (default: false). */
  silent?: boolean;
}

/**
 * Pragmas de performance (mesmos do setup SQLite original + extras):
 * - busy_timeout: escritores esperam em vez de SQLITE_BUSY imediato
 * - temp_store=MEMORY: sorts do query engine em RAM (não disco)
 * - wal_autocheckpoint=0: checkpoint manual pós-batch (evita picos)
 * - journal_size_limit: limita crescimento do WAL
 */
const PRAGMAS = [
  "PRAGMA journal_mode = WAL;",
  "PRAGMA synchronous = NORMAL;",
  "PRAGMA mmap_size = 268435456;",
  "PRAGMA cache_size = -64000;",
  "PRAGMA busy_timeout = 5000;",
  "PRAGMA temp_store = MEMORY;",
  "PRAGMA wal_autocheckpoint = 0;",
  "PRAGMA journal_size_limit = 67108864;",
];

/** Extensão C compilada (hierarchical_engine), uma por plataforma. */
const EXT_FILE = process.platform === "win32" ? "hierarchical_engine.dll" : process.platform === "darwin" ? "hierarchical_engine.dylib" : "hierarchical_engine.so";

const EXT_PATH = path.resolve(process.cwd(), "hierarchical_engine", "bin", EXT_FILE);

/**
 * Gerencia o ciclo de vida do banco libSQL (Turso embedded):
 * abertura do arquivo, pragmas, schema e carregamento da extensão C.
 *
 * A extensão C roda DENTRO do processo (como o SQLite original) — zero
 * IPC/round-trips, diferente do `embedded-postgres` (TCP loopback).
 *
 * ⚠️ Sobre o driver: usamos o pacote `libsql` (binding nativo do mesmo
 * motor libSQL) porque `@tursodatabase/database` ainda NÃO expõe
 * `loadExtension()` — o wrapper TS lança "not implemented" e o binding
 * nativo não exporta o método (validado empiricamente na v0.7.2). A API
 * é idêntica à do better-sqlite3; quando o `@tursodatabase/database`
 * suportar extensões, basta trocar o import abaixo.
 *
 * @example
 * ```ts
 * const engine = new LibsqlEngine();
 * engine.start({ databasePath: "./db/f-base.db" });
 * const store = new LibsqlHierarchicalStore(engine.db);
 * store.set("/users/100", { name: "Alice" });
 * engine.stop();
 * ```
 */
export class LibsqlEngine {
  private _db: Database.Database | null = null;
  private _path: string;

  constructor() {
    this._path = "";
  }

  /**
   * Abre o banco, aplica pragmas/schema e carrega a extensão C.
   *
   * @param options - Configuração do motor.
   * @returns `this` para encadeamento.
   * @example
   * ```ts
   * engine.start({ memory: true });
   * ```
   */
  start(options: LibsqlEngineOptions = {}): this {
    if (this._db) this.stop();

    const target = options.memory ? ":memory:" : (options.databasePath ?? path.resolve(process.cwd(), "db", "f-base.db"));

    this._path = target;

    // O libSQL não cria diretórios intermediários — garante o diretório pai
    if (target !== ":memory:") {
      const dir = path.dirname(target);
      if (dir && dir !== ".") fs.mkdirSync(dir, { recursive: true });
    }

    this._db = new Database(target);

    for (const pragma of PRAGMAS) this._db.exec(pragma);

    if (!options.silent) {
      console.log(`✅ libSQL aberto: ${target}`);
      console.log(`✅ Schema "nodes" pronto (WITHOUT ROWID)`);
    }

    try {
      this._db.loadExtension(EXT_PATH);
    } catch (err) {
      if (!options.silent) {
        console.error("❌ Falha ao carregar extensão C. Compile primeiro com build.sh");
        console.error(`   Caminho esperado: ${EXT_PATH}`);
      }
      throw err;
    }
    if (!options.silent) console.log("✅ Extensão C hierarchical_engine carregada");

    return this;
  }

  /** Instância `Database` do libSQL (better-sqlite3-compatible). */
  get db(): Database.Database {
    if (!this._db) {
      throw new Error("LibsqlEngine não iniciado — chame start() primeiro.");
    }
    return this._db;
  }

  /** Caminho do banco aberto (":memory:" se em memória). */
  get path(): string {
    return this._path;
  }

  /**
   * Executa um checkpoint do WAL (TRUNCATE) — ideal após batches grandes.
   * Como `wal_autocheckpoint=0`, o checkpoint é manual para evitar picos
   * de latência durante escrita sustentada.
   *
   * @example
   * ```ts
   * // após importar 10k documentos:
   * engine.checkpoint();
   * ```
   */
  checkpoint(): void {
    if (this._db) this._db.exec("PRAGMA wal_checkpoint(TRUNCATE)");
  }

  /**
   * Fecha o banco (graceful). Chamadas seguintes a `db` lançam erro.
   *
   * @example
   * ```ts
   * engine.stop();
   * ```
   */
  stop(): void {
    if (this._db) {
      try {
        this._db.close();
      } catch {
        /* ignore */
      }
      this._db = null;
    }
  }
}

export default LibsqlEngine;
