import { LibsqlEngine } from "./libsql-engine.js";
import { LibsqlHierarchicalStore } from "./libsql-store.js";

/**
 * =============================================================================
 * f-base — Store hierárquico JSON↔tabela sobre libSQL (Turso embedded)
 * =============================================================================
 *
 * Este módulo substitui o `embedded-postgres` (src/pg-engine.ts) pelo libSQL,
 * mantendo a MESMA API de alto nível (set/get/update/query/import/export).
 *
 * A extensão C `hierarchical_engine` (hierarchical_engine/src/he_extension.c,
 * com os módulos he_*.c em Clean Architecture) roda DENTRO do processo da
 * aplicação (como o SQLite original) — o flatten e o unflatten de JSON são
 * feitos em C (yyjson) sem round-trips de rede, sem processo separado e sem
 * travar o event loop (chamadas síncronas nativas).
 *
 * Diferença vs. embedded-postgres: as chamadas são SÍNCRONAS (driver libSQL é
 * better-sqlite3-compatible), portanto não há `await` — a latência cai para
 * microssegundos (zero IPC/TCP loopback).
 *
 * Execução: npm start
 * =============================================================================
 */

function main(): void {
  // 1. Inicialização do banco libSQL (arquivo local + extensão C)
  const engine = new LibsqlEngine();
  engine.start({
    databasePath: "./db/f-base.db",
    silent: true,
  });
  console.log("✅ libSQL iniciado (./db/f-base.db)");
  console.log("✅ Extensão C hierarchical_engine carregada e registrada");

  // 2. Store de alto nível
  const store = new LibsqlHierarchicalStore(engine.db);

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

  // ─── 4. QUERY com /* wildcard ──────────────────────────────────────
  console.log("\n─── 4. QUERY com /* wildcard ─────────────────────");

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

  // ─── 5. UPDATE com /* wildcard (aplica a TODOS os filhos) ──────────
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

  // ─── 8. TRANSACTION (atômica) ───────────────────────────────────────
  console.log("\n─── 8. Transaction atômica ───────────────────────");
  store.transaction((tx) => {
    tx.set("/tx/a", { x: 1 });
    tx.set("/tx/b", { y: 2 });
  });
  const txResult = store.get("/tx");
  console.log("✅ transaction() OK →", JSON.stringify(txResult));

  console.log("\n─── PoC concluída com sucesso! ───────────────────");

  // 3. Encerramento gracioso
  engine.stop();
  console.log("✅ libSQL encerrado.");

  store.set("/teste", [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]);
}

try {
  main();
} catch (err) {
  console.error("❌ Erro fatal:", err);
  process.exit(1);
}
