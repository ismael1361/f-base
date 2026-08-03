import { PgEngine } from "./pg-engine.js";
import { PgHierarchicalStore } from "./pg-store.js";
import path from "path";
import fs from "fs";

/**
 * Helper de testes para a suíte PostgreSQL.
 * Sobe um único cluster embarcado para toda a suíte, com diretório
 * temporário, e limpa a tabela `nodes` entre os testes.
 */
export interface PgTestContext {
  engine: PgEngine;
  store: PgHierarchicalStore;
  client: PgEngine["client"];
  cleanup: () => Promise<void>;
  reset: () => Promise<void>;
}

const TEST_DIR = path.resolve(process.cwd(), "db", "pg-test-data");

/**
 * Sobe o cluster de teste (uma vez por suíte).
 *
 * @example
 * ```ts
 * const ctx = await setupPg();
 * await ctx.reset();
 * ```
 */
export async function setupPg(): Promise<PgTestContext> {
  // Remove dados antigos de execuções anteriores
  try {
    fs.rmSync(TEST_DIR, { recursive: true, force: true });
  } catch {
    /* ignore */
  }

  const engine = new PgEngine();
  await engine.start({
    databaseDir: TEST_DIR,
    port: Number(process.env.PG_TEST_PORT ?? 5433), // configurável p/ CI paralelo
    persistent: false,
    silent: true,
  });

  const store = new PgHierarchicalStore(engine.client);

  const reset = async (): Promise<void> => {
    await engine.client.query("DELETE FROM nodes");
  };

  const cleanup = async (): Promise<void> => {
    await engine.stop();
    // Remove dados de teste (pode falhar se o SO ainda segurar o diretório)
    try {
      fs.rmSync(TEST_DIR, { recursive: true, force: true });
    } catch {
      /* ignore */
    }
  };

  return { engine, store, client: engine.client, cleanup, reset };
}
