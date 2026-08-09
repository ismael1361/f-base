import { strict as assert } from "node:assert";
import { setupLibsql, type LibsqlTestContext } from "./libsql-test-helper";

// =============================================================================
// Test Suite: Múltiplas Tabelas ({t}_nodes / {t}_doc_hashes)
//
// Cobre:
//  - Isolamento entre tabelas (mesmo path → valores independentes)
//  - Criação automática das tabelas via register_table (idempotente)
//  - Coexistência com o par default (nodes/doc_hashes) legado
//  - Dedup por tabela (revision estável em set idêntico)
//  - Rejeição de nomes inválidos (anti SQL injection)
//  - query/update/export/import/searchByPath por tabela
// =============================================================================

let ctx: LibsqlTestContext;
ctx = setupLibsql();

function beforeTest(): void {
  ctx.reset();
}

function tableNames(): string[] {
  const rows = ctx.db.prepare("SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name").all() as { name: string }[];
  return rows.map((r) => r.name);
}

// ---------------------------------------------------------------------------
// Isolamento: mesmo path em tabelas diferentes
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Multi-tabela — isolamento entre users e posts");

  const users = ctx.store; // sem tableName → default
  const posts = new (ctx.store.constructor as typeof import("../libsql-store.js").LibsqlHierarchicalStore)(ctx.db, { tableName: "posts" });

  users.set("/x", { name: "Alice", age: 30 });
  posts.set("/x", { title: "Post A", tags: ["a"] });

  assert.deepEqual(users.get("/x"), { name: "Alice", age: 30 }, "default /x = Alice");
  assert.deepEqual(posts.get("/x"), { title: "Post A", tags: ["a"] }, "posts /x = Post A");

  // Mesmo path, storages independentes — um não afeta o outro
  posts.update("/x", { title: "Post B" });
  assert.deepEqual(users.get("/x"), { name: "Alice", age: 30 }, "default intacto após update em posts");
  assert.equal((posts.get("/x") as any).title, "Post B");

  // Tabelas físicas criadas
  const names = tableNames();
  assert.ok(names.includes("posts_nodes"), "posts_nodes deve existir");
  assert.ok(names.includes("posts_doc_hashes"), "posts_doc_hashes deve existir");

  console.log("   ✅ Isolamento passou");
}

// ---------------------------------------------------------------------------
// register_table idempotente + rejeição de nomes inválidos
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Multi-tabela — register_table idempotente e validação");

  ctx.db.exec("SELECT register_table('users')");
  const names1 = tableNames();
  assert.ok(names1.includes("users_nodes"), "users_nodes criada");
  assert.ok(names1.includes("users_doc_hashes"), "users_doc_hashes criada");

  // Idempotente: registrar de novo não quebra
  ctx.db.exec("SELECT register_table('users')");
  assert.deepEqual(tableNames(), names1, "nenhuma tabela duplicada");

  // Nomes inválidos devem lançar (anti SQL injection)
  for (const bad of ["x; DROP TABLE nodes", "9abc", "a b", "tabela.com", "x'y"]) {
    assert.throws(() => ctx.db.exec(`SELECT register_table('${bad}')`), /Invalid table name|error/i, `deve rejeitar: ${bad}`);
  }
  // A tabela nodes default segue intacta após as tentativas
  const alive = ctx.db.prepare("SELECT COUNT(*) c FROM main_nodes").get() as { c: number };
  assert.equal(alive.c, 0, "nodes segue existindo e vazia");

  console.log("   ✅ register_table/validação passou");
}

// ---------------------------------------------------------------------------
// Coexistência default + customizada no mesmo banco
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Multi-tabela — coexistência default + customizada");

  const legacy = ctx.store;
  const users = new (ctx.store.constructor as typeof import("../libsql-store.js").LibsqlHierarchicalStore)(ctx.db, { tableName: "users" });

  legacy.set("/people/alice", { name: "Alice" });
  users.set("/people/bob", { name: "Bob" });

  // Ambos leem os próprios dados
  assert.deepEqual(legacy.get("/people"), { alice: { name: "Alice" } }, "default só vê alice");
  assert.deepEqual(users.get("/people"), { bob: { name: "Bob" } }, "users só vê bob");

  // searchByPath isola por tabela
  const legacyPaths = legacy.searchByPath("/people/").map((n) => n.path);
  const usersPaths = users.searchByPath("/people/").map((n) => n.path);
  assert.ok(legacyPaths.includes("/people/alice/") && !legacyPaths.includes("/people/bob/"), "search default isolado");
  assert.ok(usersPaths.includes("/people/bob/") && !usersPaths.includes("/people/alice/"), "search users isolado");

  console.log("   ✅ Coexistência passou");
}

// ---------------------------------------------------------------------------
// Dedup por tabela (revision estável em set idêntico)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Multi-tabela — dedup por tabela");

  const users = new (ctx.store.constructor as typeof import("../libsql-store.js").LibsqlHierarchicalStore)(ctx.db, { tableName: "users" });
  const posts = new (ctx.store.constructor as typeof import("../libsql-store.js").LibsqlHierarchicalStore)(ctx.db, { tableName: "posts" });

  const doc = { name: "X", tags: ["a", "b"] };
  const r1u = users.set("/d", doc);
  const r1p = posts.set("/d", doc);

  // Mesmo conteúdo em tabelas diferentes → revisions independentes (não colidem)
  assert.ok(r1u.length > 0 && r1p.length > 0);

  const r2u = users.set("/d", doc);
  assert.equal(r2u, r1u, "dedup ativo em users (mesma revision)");
  const r2p = posts.set("/d", doc);
  assert.equal(r2p, r1p, "dedup ativo em posts (mesma revision)");

  // Cada tabela tem seu doc_hashes próprio
  const usersHash = ctx.db.prepare("SELECT COUNT(*) c FROM users_doc_hashes").get() as { c: number };
  const postsHash = ctx.db.prepare("SELECT COUNT(*) c FROM posts_doc_hashes").get() as { c: number };
  assert.equal(usersHash.c, 1, "users_doc_hashes tem 1 entrada");
  assert.equal(postsHash.c, 1, "posts_doc_hashes tem 1 entrada");

  console.log("   ✅ Dedup por tabela passou");
}

// ---------------------------------------------------------------------------
// query/update/delete por tabela + cache de leitura isolado
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Multi-tabela — query, update, delete e cache por tabela");

  const users = new (ctx.store.constructor as typeof import("../libsql-store.js").LibsqlHierarchicalStore)(ctx.db, { tableName: "users" });
  const posts = new (ctx.store.constructor as typeof import("../libsql-store.js").LibsqlHierarchicalStore)(ctx.db, { tableName: "posts" });

  users.set("/people/alice", { name: "Alice", age: 25 });
  users.set("/people/bob", { name: "Bob", age: 35 });
  posts.set("/posts/1", { title: "P1" });
  posts.set("/posts/2", { title: "P2" });

  // Query com filtro por tabela
  const adults = users.query("/people/*", { filters: [{ key: "age", op: ">", compare: 30 }] });
  assert.equal(adults.length, 1, "users query: só Bob");
  assert.equal((adults[0] as any).name, "Bob");

  const allPosts = posts.query("/posts/*", { take: 10 });
  assert.equal(allPosts.length, 2, "posts query: 2 posts");

  // Cache de leitura: segundo get serve do cache (cada store com seu cache)
  const t0 = performance.now();
  users.get("/people");
  const warm1 = users.get("/people");
  const t1 = performance.now();
  assert.ok(warm1 && (warm1 as any).alice, "users /people válido");
  assert.ok(t1 - t0 < 50, "get em cache deve ser rápido");

  // Delete via set null por tabela
  posts.set("/posts/2", null);
  assert.equal(posts.get("/posts/2"), null, "posts /posts/2 removido");
  assert.ok(users.get("/people"), "users /people segue presente");

  console.log("   ✅ Query/update/delete por tabela passou");
}

// ---------------------------------------------------------------------------
// export/import por tabela
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Multi-tabela — export/import por tabela");

  const users = new (ctx.store.constructor as typeof import("../libsql-store.js").LibsqlHierarchicalStore)(ctx.db, { tableName: "users" });
  const posts = new (ctx.store.constructor as typeof import("../libsql-store.js").LibsqlHierarchicalStore)(ctx.db, { tableName: "posts" });

  users.set("/docs/1", { name: "A", value: 1 });
  posts.set("/docs/2", { name: "B", value: 2 });

  // Export JSON por tabela
  const jsonUsers = JSON.parse(users.export("/docs", "json").toString("utf-8"));
  const jsonPosts = JSON.parse(posts.export("/docs", "json").toString("utf-8"));
  assert.ok((jsonUsers as any)["1"], "export users traz docs/1");
  assert.equal((jsonUsers as any)["2"], undefined, "export users NÃO traz docs/2");
  assert.ok((jsonPosts as any)["2"], "export posts traz docs/2");

  // Export CSV por tabela
  const csvUsers = users.export("/docs", "csv").toString("utf-8");
  assert.ok(csvUsers.includes("/docs/1/"), "CSV users contém docs/1");
  assert.ok(!csvUsers.includes("/docs/2/"), "CSV users não contém docs/2");

  // Import CSV por tabela
  const csv = 'path,type,text_value\n/d/i/,1,{}\n/d/i/name,5,""Imported""\n';
  await users.import("/d", Buffer.from(csv, "utf-8"), "csv");
  const imported = users.get("/d") as any;
  assert.equal(imported.name, "Imported", "import CSV em users");
  assert.equal(posts.get("/d"), null, "posts não recebeu o import");

  console.log("   ✅ Export/import por tabela passou");
}

// ---------------------------------------------------------------------------
// Nome inválido no construtor do store é rejeitado
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Multi-tabela — tableName inválido no construtor");

  const Store = ctx.store.constructor as typeof import("../libsql-store.js").LibsqlHierarchicalStore;
  for (const bad of ["x; DROP TABLE nodes", "9abc", "a b", "tabela.com"]) {
    assert.throws(() => new Store(ctx.db, { tableName: bad }), /Invalid tableName/, `construtor deve rejeitar: ${bad}`);
  }
  // Nenhuma tabela foi criada pelas tentativas inválidas
  const names = tableNames();
  assert.ok(!names.includes("x; DROP TABLE nodes_nodes"), "nenhuma tabela inválida criada");

  console.log("   ✅ Validação no construtor passou");
}

// =============================================================================
// Resumo
// =============================================================================
console.log("\n========================================");
console.log("✅ Todos os testes multi-tabela passaram!");
console.log("========================================");

ctx.cleanup();
