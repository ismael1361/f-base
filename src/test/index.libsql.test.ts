import { strict as assert } from "node:assert";
import { setupLibsql, type LibsqlTestContext } from "./libsql-test-helper";

// =============================================================================
// Test Suite: Hierarchical Engine (libSQL / Turso embedded)
// Paridade com src/index.test.ts (SQLite) e src/index.pg.test.ts (PostgreSQL),
// agora sobre libSQL com a extensão C rodando in-process (zero IPC).
// =============================================================================

let ctx: LibsqlTestContext;

// Helpers
function rawNodes(): any[] {
  return ctx.db.prepare("SELECT path, type, text_value FROM main_nodes ORDER BY path").all() as any[];
}

function ingest(docId: string, data: object | null): void {
  ctx.store.set(docId, data);
}

function extract(prefix: string): object | null {
  return ctx.store.get(prefix);
}

function beforeTest(): void {
  ctx.reset();
}

// Inicializa o banco de teste (memória + extensão C)
ctx = setupLibsql();
ctx.registerTable("main");

// ---------------------------------------------------------------------------
// Exemplo 01: SET documento aninhado
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Exemplo 01 — SET /users/100");

  ingest("users/100", {
    id: 100,
    name: "Alan Turing",
    address: { city: "Londres", zip: "12345" },
    tags: ["genius", "computer"],
  });

  const nodes = rawNodes();

  assert.ok(
    nodes.find((n: any) => n.path === "/"),
    "Root / deve existir",
  );
  assert.ok(
    nodes.find((n: any) => n.path === "/users/"),
    "/users/ deve existir",
  );
  assert.ok(
    nodes.find((n: any) => n.path === "/users/100/"),
    "/users/100/ deve existir",
  );

  const idNode = nodes.find((n: any) => n.path === "/users/100/id");
  assert.ok(idNode, "id node deve existir");
  assert.equal(idNode.type, 3, "id deve ser NUMBER (type=3)");
  assert.equal(idNode.text_value, "100", "id text_value = '100'");

  const nameNode = nodes.find((n: any) => n.path === "/users/100/name");
  assert.ok(nameNode, "name node deve existir");
  assert.equal(nameNode.type, 5, "name deve ser STRING (type=5)");
  assert.equal(nameNode.text_value, '"Alan Turing"', "name deve ter aspas");

  const addrNode = nodes.find((n: any) => n.path === "/users/100/address/");
  assert.ok(addrNode, "/users/100/address/ deve existir");
  assert.equal(addrNode.type, 1, "address deve ser OBJECT (type=1)");
  assert.equal(addrNode.text_value, "{}", "address text_value = '{}'");

  const tagsNode = nodes.find((n: any) => n.path === "/users/100/tags/");
  assert.ok(tagsNode, "/users/100/tags/ deve existir");
  assert.equal(tagsNode.type, 2, "tags deve ser ARRAY (type=2)");
  assert.equal(tagsNode.text_value, "{}", "tags text_value = '{}'");

  const tag0 = nodes.find((n: any) => n.path === "/users/100/tags/0");
  assert.ok(tag0, "tags/0 deve existir");
  assert.equal(tag0.type, 5, "tags/0 deve ser STRING");
  assert.equal(tag0.text_value, '"genius"', "tags/0 = genius");

  const tag1 = nodes.find((n: any) => n.path === "/users/100/tags/1");
  assert.equal(tag1.type, 5, "tags/1 deve ser STRING");
  assert.equal(tag1.text_value, '"computer"');

  console.log("   ✅ Exemplo 01 passou");
}

// ---------------------------------------------------------------------------
// Exemplo 04: SET null (deleção)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Exemplo 04 — SET /users/100/address → null");

  ingest("users/100", {
    id: 100,
    name: "Alan Turing",
    address: { city: "Londres", zip: "12345" },
    tags: ["genius", "computer"],
  });

  ingest("users/100/address", null);

  const nodes = rawNodes();
  const hasAddress = nodes.some((n: any) => n.path.startsWith("/users/100/address"));
  assert.ok(!hasAddress, "address e descendentes devem ter sido deletados");

  assert.ok(
    nodes.find((n: any) => n.path === "/users/100/"),
    "/users/100/ deve existir",
  );
  assert.ok(
    nodes.find((n: any) => n.path === "/users/100/id"),
    "id deve existir",
  );
  assert.ok(
    nodes.find((n: any) => n.path === "/users/100/name"),
    "name deve existir",
  );

  const addrAfter = extract("/users/100/address");
  assert.equal(addrAfter, null, "GET /users/100/address deve retornar null");

  console.log("   ✅ Exemplo 04 passou");
}

// ---------------------------------------------------------------------------
// Exemplo 06: GET mantém estrutura (address deletado)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Exemplo 06 — GET /users/100 sem address");

  ingest("users/100", {
    id: 100,
    name: "Alan Turing",
    address: { city: "Londres", zip: "12345" },
    tags: ["genius", "computer"],
  });

  ingest("users/100/address", null);

  const doc = extract("/users/100");
  assert.ok(doc, "documento deve existir");
  assert.equal((doc as any).id, 100, "id = 100");
  assert.equal((doc as any).name, "Alan Turing", "name = Alan Turing");
  assert.equal((doc as any).address, undefined, "address deve ter sido removido");

  assert.ok(Array.isArray((doc as any).tags), "tags deve ser array");
  assert.equal((doc as any).tags[0], "genius", "tags[0] = genius");
  assert.equal((doc as any).tags[1], "computer", "tags[1] = computer");

  console.log("   ✅ Exemplo 06 passou");
}

// ---------------------------------------------------------------------------
// Exemplo 07: SET substitui tags (array → objeto nomeado)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Exemplo 07 — SET /users/100/tags → objeto nomeado");

  ingest("users/100", {
    id: 100,
    name: "Alan Turing",
    tags: ["genius", "computer"],
  });

  ingest("users/100/tags", {
    item_01: "mathematician",
    item_02: "logician",
    item_03: 400,
  });

  const nodes = rawNodes();

  const tagsNode = nodes.find((n: any) => n.path === "/users/100/tags/");
  assert.ok(tagsNode, "tags/ deve existir");
  assert.equal(tagsNode.type, 1, "tags agora é type=1 (OBJECT)");
  assert.equal(tagsNode.text_value, "{}");

  const item1 = nodes.find((n: any) => n.path === "/users/100/tags/item_01");
  assert.ok(item1, "item_01 deve existir");
  assert.equal(item1.type, 5, "item_01 deve ser STRING");
  assert.equal(item1.text_value, '"mathematician"');

  const item3 = nodes.find((n: any) => n.path === "/users/100/tags/item_03");
  assert.ok(item3, "item_03 deve existir");
  assert.equal(item3.type, 3, "item_03 deve ser NUMBER");
  assert.equal(item3.text_value, "400");

  const old0 = nodes.find((n: any) => n.path === "/users/100/tags/0");
  assert.equal(old0, undefined, "tags/0 deve ter sido deletado");

  const doc = extract("/users/100");
  assert.ok((doc as any).tags, "tags deve existir");
  assert.equal((doc as any).tags.item_01, "mathematician");
  assert.equal((doc as any).tags.item_03, 400);

  console.log("   ✅ Exemplo 07 passou");
}

// ---------------------------------------------------------------------------
// Exemplo 09: Substituição parcial do documento
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Exemplo 09 — SET /users/100 (partial overwrite)");

  ingest("users/100", {
    id: 100,
    name: "Alan Turing",
    address: { city: "Londres", zip: "12345" },
    tags: ["genius", "computer"],
  });

  ingest("users/101", {
    id: 101,
    name: "Ada Lovelace",
    address: { city: "Londres", zip: "54321" },
    tags: ["pioneer", "programmer"],
  });

  ingest("users/100", {
    id: 100,
    name: "Alan Mathison Turing",
  });

  const doc100 = extract("/users/100");
  assert.equal((doc100 as any).id, 100);
  assert.equal((doc100 as any).name, "Alan Mathison Turing");
  assert.equal((doc100 as any).address, undefined);
  assert.equal((doc100 as any).tags, undefined);

  const doc101 = extract("/users/101");
  assert.equal((doc101 as any).id, 101);
  assert.equal((doc101 as any).name, "Ada Lovelace");
  assert.ok((doc101 as any).address, "address de user 101 não foi afetado");

  console.log("   ✅ Exemplo 09 passou");
}

// ---------------------------------------------------------------------------
// Exemplo 10: Deleção de documento não afeta outros
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Exemplo 10 — SET /users/101 → null");

  ingest("users/100", { id: 100, name: "Alan Mathison Turing" });
  ingest("users/101", { id: 101, name: "Ada Lovelace" });

  ingest("users/101", null);

  const doc101 = extract("/users/101");
  assert.equal(doc101, null, "user 101 deletado → null");

  const doc100 = extract("/users/100");
  assert.ok(doc100, "user 100 ainda existe");
  assert.equal((doc100 as any).name, "Alan Mathison Turing");

  console.log("   ✅ Exemplo 10 passou");
}

// ---------------------------------------------------------------------------
// Teste: Boolean roundtrip
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Teste — Boolean roundtrip");

  ingest("test/bool", { active: true, verified: false });

  const doc = extract("/test/bool");
  assert.equal((doc as any).active, true);
  assert.equal((doc as any).verified, false);

  const activeNode = rawNodes().find((n: any) => n.path === "/test/bool/active");
  assert.equal(activeNode.type, 4, "boolean type = 4");
  assert.equal(activeNode.text_value, "true");

  console.log("   ✅ Boolean passou");
}

// ---------------------------------------------------------------------------
// Teste: Números (inteiros e reais)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Teste — Number roundtrip");

  ingest("test/numbers", { integer: 42, negative: -10, floating: 3.14 });

  const doc = extract("/test/numbers");
  assert.equal((doc as any).integer, 42);
  assert.equal((doc as any).negative, -10);
  assert.equal((doc as any).floating, 3.14);

  console.log("   ✅ Numbers passou");
}

// ---------------------------------------------------------------------------
// Teste: JSON vazio e array vazio
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Teste — Objetos e arrays vazios");

  ingest("test/empty", { emptyObj: {}, emptyArr: [] });

  const doc = extract("/test/empty");
  assert.deepEqual((doc as any).emptyObj, {}, "objeto vazio");
  assert.deepEqual((doc as any).emptyArr, [], "array vazio");

  console.log("   ✅ Empty structures passou");
}

// ---------------------------------------------------------------------------
// Teste: Documento não existente
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Teste — Documento não existente retorna null");

  const result = extract("/nonexistent/doc");
  assert.equal(result, null);

  console.log("   ✅ Nonexistent passou");
}

// ---------------------------------------------------------------------------
// Teste: QUERY com filtros e ordenação
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Teste — Query com filtros, order e paginação");

  ctx.store.set("/people/alice", { name: "Alice", age: 25, city: "Londres" });
  ctx.store.set("/people/bob", { name: "Bob", age: 35, city: "NYC" });
  ctx.store.set("/people/charlie", { name: "Charlie", age: 20, city: "Londres" });
  ctx.store.set("/people/diana", { name: "Diana", age: 28, city: "NYC" });
  ctx.store.set("/people/ethan", { name: "Ethan", age: 42, city: "Londres" });

  const result = ctx.store.query("/people/*", {
    filters: [
      { key: "age", op: ">", compare: 22 },
      { key: "city", op: "==", compare: "Londres" },
    ],
    order: [{ key: "age", ascending: true }],
    skip: 0,
    take: 10,
  });

  assert.equal(result.length, 2, "deve retornar 2 (Alice e Ethan)");
  assert.equal((result[0] as any).name, "Alice", "primeiro por age asc");
  assert.equal((result[1] as any).name, "Ethan", "segundo por age asc");

  console.log("   ✅ Query passou:", result.map((r: any) => r.name).join(", "));
}

// ---------------------------------------------------------------------------
// Teste: UPDATE com wildcard (em massa)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Teste — Update em massa com /* wildcard");

  ctx.store.set("/people/alice", { name: "Alice", age: 25 });
  ctx.store.set("/people/bob", { name: "Bob", age: 35 });

  ctx.store.update("/people/*", { active: true, updated: true });

  const all = ctx.store.get("/people");
  assert.ok(all, "pessoas devem existir");
  for (const [key, val] of Object.entries(all)) {
    assert.equal((val as any).active, true, `${key} active=true`);
    assert.equal((val as any).updated, true, `${key} updated=true`);
  }

  console.log("   ✅ Update em massa passou");
}

// ---------------------------------------------------------------------------
// Teste: TRANSACTION atômica (set dentro do callback)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Teste — Transaction atômica");

  ctx.store.transaction((tx) => {
    tx.set("/a", { x: 1 });
    tx.set("/b", { y: 2 });
  });

  assert.deepEqual(ctx.store.get("/a"), { x: 1 });
  assert.deepEqual(ctx.store.get("/b"), { y: 2 });

  // Rollback: se uma operação lança, nada é persistido
  let threw = false;
  try {
    ctx.store.transaction((tx) => {
      tx.set("/c", { z: 3 });
      throw new Error("forçar rollback");
    });
  } catch {
    threw = true;
  }
  assert.ok(threw, "transação deve lançar");
  assert.equal(ctx.store.get("/c"), null, "/c não deve existir após rollback");

  console.log("   ✅ Transaction passou");
}

// ---------------------------------------------------------------------------
// Teste: searchByPath (prefix range)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Teste — searchByPath");

  ctx.store.set("/people/alice", { name: "Alice" });
  ctx.store.set("/people/bob", { name: "Bob" });
  ctx.store.set("/other/x", { name: "X" });

  const children = ctx.store.searchByPath("/people/");
  const paths = children.map((c) => c.path);
  // Containers (OBJECT) armazenam trailing slash: "/people/alice/"
  assert.ok(paths.includes("/people/alice/"), "alice presente");
  assert.ok(paths.includes("/people/bob/"), "bob presente");
  assert.ok(!paths.includes("/other/x/"), "/other/x fora do prefixo");

  console.log("   ✅ searchByPath passou:", paths.join(", "));
}

// ---------------------------------------------------------------------------
// Teste: UPDATE de folhas (fast path) preserva irmãos e tipos
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Teste — Update de folhas (fast path)");

  ctx.store.set("/users/100", {
    id: 100,
    name: "Alan Turing",
    age: 41,
    active: true,
    tags: ["genius", "computer"],
  });

  // Patch de primitivos — deve preservar tags (container) intacto
  ctx.store.update("/users/100", { age: 42, name: "Alan M. Turing" });

  const doc = ctx.store.get("/users/100");
  assert.equal((doc as any).id, 100, "id preservado");
  assert.equal((doc as any).name, "Alan M. Turing", "name atualizado");
  assert.equal((doc as any).age, 42, "age atualizado");
  assert.equal((doc as any).active, true, "active preservado");
  assert.deepEqual((doc as any).tags, ["genius", "computer"], "tags container preservado");

  // Deletar uma chave com null (fast path delete)
  ctx.store.update("/users/100", { age: null });
  const doc2 = ctx.store.get("/users/100");
  assert.equal((doc2 as any).age, undefined, "age removido via null");
  assert.equal((doc2 as any).name, "Alan M. Turing", "name segue presente");

  console.log("   ✅ Update de folhas passou");
}

// ---------------------------------------------------------------------------
// Teste: UPDATE de folha sobre nó container existente (substituição)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Teste — Update de folha substituindo container");

  ctx.store.set("/doc", { profile: { nested: { deep: true } }, name: "X" });

  // Substitui profile (container) por string — fast path deve apagar subtree
  ctx.store.update("/doc", { profile: "simples" });

  const doc = ctx.store.get("/doc");
  assert.equal((doc as any).profile, "simples", "profile virou string");
  assert.equal((doc as any).name, "X", "name preservado");

  // Verifica que não há nós órfãos do container antigo
  const orphan = ctx.db.prepare("SELECT path FROM main_nodes WHERE path LIKE '/doc/profile/%'").all();
  assert.equal(orphan.length, 0, "nenhum nó órfão de /doc/profile/");

  console.log("   ✅ Update de folha substituindo container passou");
}

// =============================================================================
// Resumo
// =============================================================================
console.log("\n========================================");
console.log("✅ Todos os testes libSQL passaram!");
console.log("========================================");

ctx.cleanup();
