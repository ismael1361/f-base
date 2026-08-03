import EmbeddedPostgres from "embedded-postgres";
import { Client } from "pg";
import path from "path";

/**
 * Opções de inicialização do cluster PostgreSQL embarcado.
 */
export interface PgEngineOptions {
  /** Diretório onde os dados do cluster persistem. */
  databaseDir?: string;
  /** Porta TCP (default: 5432). */
  port?: number;
  /** Usuário superuser do cluster. */
  user?: string;
  /** Senha do usuário. */
  password?: string;
  /** Se true, mantém os dados ao parar o cluster. */
  persistent?: boolean;
  /** Flags extras passadas ao processo postgres (tuning de performance). */
  postgresFlags?: string[];
  /** Desliga o log do postgres (default: false). */
  silent?: boolean;
}

/**
 * SQL de criação do schema (paridade com a tabela do SQLite, mas com
 * tipos do PostgreSQL e índice para prefix range).
 */
const SCHEMA_SQL = `
CREATE TABLE IF NOT EXISTS nodes (
    path TEXT PRIMARY KEY,
    type INTEGER NOT NULL,
    text_value TEXT,
    created BIGINT NOT NULL,
    modified BIGINT NOT NULL,
    revision_nr INTEGER NOT NULL,
    revision TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_nodes_path ON nodes (path text_pattern_ops);
`;

/**
 * Registro das funções C da extensão hierarchical_engine no catálogo.
 * O PostgreSQL resolve a DLL "hierarchical_engine" em dynamic_library_path
 * (default: $libdir → diretório lib/ dos binários embarcados).
 */
const FUNCTIONS_SQL = `
CREATE OR REPLACE FUNCTION set_json(text, text) RETURNS text
  AS 'hierarchical_engine', 'set_json' LANGUAGE C STRICT;
CREATE OR REPLACE FUNCTION update_json(text, text) RETURNS text
  AS 'hierarchical_engine', 'update_json' LANGUAGE C STRICT;
CREATE OR REPLACE FUNCTION extract_json(text) RETURNS text
  AS 'hierarchical_engine', 'extract_json' LANGUAGE C STRICT;
CREATE OR REPLACE FUNCTION query_json(text, text) RETURNS text
  AS 'hierarchical_engine', 'query_json' LANGUAGE C STRICT;
CREATE OR REPLACE FUNCTION export_csv(text) RETURNS text
  AS 'hierarchical_engine', 'export_csv' LANGUAGE C STRICT;
CREATE OR REPLACE FUNCTION import_csv(text, text) RETURNS text
  AS 'hierarchical_engine', 'import_csv' LANGUAGE C STRICT;
CREATE OR REPLACE FUNCTION ingest_json(text, text) RETURNS text
  AS 'hierarchical_engine', 'ingest_json' LANGUAGE C STRICT;
`;

/** Flags padrão de performance (mais throughput em dev). */
const DEFAULT_POSTGRES_FLAGS = ["-c", "synchronous_commit=off", "-c", "shared_buffers=256MB", "-c", "max_connections=50"];

/**
 * Gerencia o ciclo de vida do PostgreSQL embarcado (embedded-postgres):
 * inicialização do cluster, schema, registro das funções C e conexão.
 *
 * A extensão C roda DENTRO do processo do servidor (como no SQLite,
 * que roda no processo da aplicação) — elimina IPC/round-trips.
 *
 * @example
 * ```ts
 * const engine = new PgEngine();
 * await engine.start({ databaseDir: "./db/pg-data" });
 * const store = new PgHierarchicalStore(engine.client);
 * await store.set("/users/100", { name: "Alice" });
 * await engine.stop();
 * ```
 */
export class PgEngine {
  private pg: EmbeddedPostgres | null = null;
  private _client: Client | null = null;

  /**
   * Sobe o cluster, conecta e aplica schema + funções C.
   *
   * @param options - Configuração do cluster.
   * @example
   * ```ts
   * await engine.start({ port: 5432 });
   * ```
   */
  async start(options: PgEngineOptions = {}): Promise<PgEngine> {
    const databaseDir = options.databaseDir ?? path.resolve(process.cwd(), "db", "pg-data");

    this.pg = new EmbeddedPostgres({
      databaseDir,
      user: options.user ?? "postgres",
      password: options.password ?? "password",
      port: options.port ?? 5432,
      persistent: options.persistent ?? true,
      initdbFlags: ["--encoding=UTF8", "--locale=C"],
      postgresFlags: options.postgresFlags ?? DEFAULT_POSTGRES_FLAGS,
      onLog: options.silent ? () => undefined : (msg) => console.log(msg),
      onError: options.silent ? () => undefined : (msg) => console.error(msg),
    });

    await this.pg.initialise();
    await this.pg.start();

    this._client = this.pg.getPgClient();
    await this._client.connect();

    await this._client.query(SCHEMA_SQL);
    await this._client.query(FUNCTIONS_SQL);

    return this;
  }

  /** Cliente conectado (node-postgres). */
  get client(): Client {
    if (!this._client) {
      throw new Error("PgEngine não iniciado — chame start() primeiro.");
    }
    return this._client;
  }

  /**
   * Encerra o cliente e o cluster (graceful shutdown).
   *
   * @example
   * ```ts
   * await engine.stop();
   * ```
   */
  async stop(): Promise<void> {
    if (this._client) {
      await this._client.end().catch(() => undefined);
      this._client = null;
    }
    if (this.pg) {
      await this.pg.stop();
      this.pg = null;
    }
  }
}

export default PgEngine;
