import { PgEngine } from "./pg-engine.js";
import { PgHierarchicalStore } from "./pg-store.js";

/**
 * =============================================================================
 * f-base — Store hierárquico JSON↔tabela sobre PostgreSQL embarcado
 * =============================================================================
 *
 * Este módulo substitui o SQLite (better-sqlite3) por `embedded-postgres`,
 * mantendo a MESMA API de alto nível (set/get/update/query/import/export).
 *
 * A extensão C `hierarchical_engine` (pg_extension/hierarchical_engine_pg.c)
 * roda DENTRO do processo do servidor PostgreSQL via SPI — o flatten e o
 * unflatten de JSON são feitos em C (yyjson) sem round-trips e sem travar
 * o event loop do Node.
 *
 * Execução: npm start
 * =============================================================================
 */

async function main(): Promise<void> {
  // 1. Inicialização do cluster PostgreSQL embarcado
  const engine = new PgEngine();
  await engine.start({
    databaseDir: "./db/pg-data",
    port: 5432,
    silent: true,
  });
  console.log("✅ PostgreSQL embarcado iniciado (porta 5432)");
  console.log("✅ Extensão C hierarchical_engine carregada e registrada");

  // 2. Store de alto nível
  const store = new PgHierarchicalStore(engine.client);

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
  const revSet = await store.set("/users/100", sampleUser);
  console.log(`✅ set("/users/100", …)`);
  console.log(`   Revision: ${revSet}`);

  // ─── 2. GET (leitura) ────────────────────────────────────────────────
  console.log("\n─── 2. GET (leitura) ─────────────────────────────");
  const user = await store.get("/users/100");
  console.log("✅ get('/users/100') →", JSON.stringify(user, null, 2));

  // ─── 3. UPDATE (merge parcial - documento específico) ────────────────
  console.log("\n─── 3. UPDATE (documento específico) ─────────────");
  const revUpd = await store.update("/users/100", {
    name: "Alan M. Turing",
    occupation: "Mathematician",
  });
  console.log(`✅ update("/users/100", { name, occupation })`);
  console.log(`   Revision: ${revUpd}`);

  const updatedUser = await store.get("/users/100");
  console.log("   Resultado:", JSON.stringify(updatedUser, null, 2));

  // ─── 4. QUERY com /* wildcard ──────────────────────────────────────
  console.log("\n─── 4. QUERY com /* wildcard ─────────────────────");

  await store.set("/people/alice", { name: "Alice", age: 25, city: "Londres" });
  await store.set("/people/bob", { name: "Bob", age: 35, city: "NYC" });
  await store.set("/people/charlie", { name: "Charlie", age: 20, city: "Londres" });
  await store.set("/people/diana", { name: "Diana", age: 28, city: "NYC" });
  await store.set("/people/ethan", { name: "Ethan", age: 42, city: "Londres" });
  console.log("✅ Inseridos 5 registros em /people/*");

  const result = await store.query("/people/*", {
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
  const revMass = await store.update("/people/*", { active: true, updated: true });
  console.log(`✅ update("/people/*", { active: true })`);
  console.log(`   Revision: ${revMass}`);

  console.log("   Resultado após update em massa:");
  const allPeople = await store.get("/people");
  if (allPeople) {
    for (const [key, val] of Object.entries(allPeople)) {
      console.log(`     ${key}: ${JSON.stringify(val)}`);
    }
  }

  // ─── 6. DELETE via SET(null) ─────────────────────────────────────────
  console.log("\n─── 6. DELETE via set(null) ──────────────────────");
  await store.set("/users/100", null);
  console.log('✅ set("/users/100", null)');

  const deleted = await store.get("/users/100");
  console.log("   get após delete →", deleted);

  // ─── 7. RAW SEARCH (Prefix Range Trick) ──────────────────────────────
  console.log("\n─── 7. Raw Search (Prefix Range Trick) ───────────");
  const children = await store.searchByPath("/people/");
  console.log("   Filhos de /people/:");
  for (const c of children) {
    console.log(`     path=${c.path.padEnd(30)} type=${c.type}`);
  }

  console.log("\n─── PoC concluída com sucesso! ───────────────────");

  // 3. Encerramento gracioso
  await engine.stop();
  console.log("✅ PostgreSQL encerrado.");
}

main().catch((err) => {
  console.error("❌ Erro fatal:", err);
  process.exit(1);
});
