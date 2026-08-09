import { LibsqlEngine } from "../libsql-engine";
import { LibsqlHierarchicalStore } from "../libsql-store";
import type Database from "libsql";

/**
 * Helper de testes para a suíte libSQL.
 * Cria um banco em memória com a extensão C carregada e a tabela `nodes`
 * pronta, limpa os dados entre os testes e expõe o store de alto nível.
 */
export interface LibsqlTestContext {
  engine: LibsqlEngine;
  store: LibsqlHierarchicalStore;
  db: Database.Database;
  cleanup: () => void;
  registerTable: (name: string) => void;
  reset: () => void;
}

/**
 * Configura o ambiente de teste (banco em memória + extensão C).
 *
 * @example
 * ```ts
 * const ctx = setupLibsql();
 * ctx.reset();
 * ```
 */
export function setupLibsql(): LibsqlTestContext {
  const engine = new LibsqlEngine();
  engine.start({ memory: true, silent: true });

  const store = new LibsqlHierarchicalStore(engine.db);

  const registerTable = (name: string): void => {
    engine.db.exec(`SELECT register_table('${name}')`);
  };

  const reset = (): void => {
    // Limpa qualquer par customizado ({t}_nodes/{t}_doc_hashes) criado via
    // register_table (nomes validados como identificadores — sem risco de
    // injeção na interpolação).
    const tables = engine.db.prepare("SELECT name FROM sqlite_master WHERE type = 'table' AND (name LIKE '%_nodes' OR name LIKE '%_doc_hashes')").all() as { name: string }[];
    for (const { name } of tables) {
      engine.db.exec(`DELETE FROM "${name}"`);
    }
  };

  const cleanup = (): void => {
    engine.stop();
  };

  return { engine, store, db: engine.db, cleanup, registerTable, reset };
}
