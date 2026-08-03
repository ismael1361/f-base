import { strict as assert } from "node:assert";
import { setupPg, type PgTestContext } from "./pg-test-helper.js";

// =============================================================================
// Test Suite: Hierarchical Engine (PostgreSQL)
// Paridade com src/index.test.ts (SQLite), agora sobre embedded-postgres
// =============================================================================

let ctx: PgTestContext;

// Helpers
async function rawNodes(): Promise<any[]> {
  const res = await ctx.client.query("SELECT path, type, text_value FROM nodes ORDER BY path");
  return res.rows;
}

async function ingest(docId: string, data: object | null): Promise<void> {
  await ctx.store.set(docId, data);
}

async function extract(prefix: string): Promise<object | null> {
  return ctx.store.get(prefix);
}

async function beforeTest(): Promise<void> {
  await ctx.reset();
}

// Inicializa o cluster de teste
ctx = await setupPg();

// ---------------------------------------------------------------------------
// Exemplo 01: SET documento aninhado
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Exemplo 01 — SET /users/100");

  await ingest("users/100", {
    id: 100,
    name: "Alan Turing",
    address: { city: "Londres", zip: "12345" },
    tags: ["genius", "computer"],
  });

  const nodes = await rawNodes();

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

  const tag0 = nodes.find((n: any) => n.path === "/users/100/tags/0");
  assert.ok(tag0, "tags/0 deve existir");
  assert.equal(tag0.type, 5, "tags/0 deve ser STRING");
  assert.equal(tag0.text_value, '"genius"', "tags/0 = genius");

  console.log("   ✅ Exemplo 01 passou");
}

// ---------------------------------------------------------------------------
// Exemplo 04: SET null (deleção)
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Exemplo 04 — SET /users/100/address → null");

  await ingest("users/100", {
    id: 100,
    name: "Alan Turing",
    address: { city: "Londres", zip: "12345" },
    tags: ["genius", "computer"],
  });

  await ingest("users/100/address", null);

  const nodes = await rawNodes();
  const hasAddress = nodes.some((n: any) => n.path.startsWith("/users/100/address"));
  assert.ok(!hasAddress, "address e descendentes devem ter sido deletados");
  assert.ok(
    nodes.find((n: any) => n.path === "/users/100/"),
    "/users/100/ deve existir",
  );

  const addrAfter = await extract("/users/100/address");
  assert.equal(addrAfter, null, "GET /users/100/address deve retornar null");

  console.log("   ✅ Exemplo 04 passou");
}

// ---------------------------------------------------------------------------
// Exemplo 06: GET mantém estrutura (address deletado)
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Exemplo 06 — GET /users/100 sem address");

  await ingest("users/100", {
    id: 100,
    name: "Alan Turing",
    address: { city: "Londres", zip: "12345" },
    tags: ["genius", "computer"],
  });

  await ingest("users/100/address", null);

  const doc = await extract("/users/100");
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
  await beforeTest();
  console.log("\n🧪 Exemplo 07 — SET /users/100/tags → objeto nomeado");

  await ingest("users/100", {
    id: 100,
    name: "Alan Turing",
    tags: ["genius", "computer"],
  });

  await ingest("users/100/tags", {
    item_01: "mathematician",
    item_02: "logician",
    item_03: 400,
  });

  const nodes = await rawNodes();
  const tagsNode = nodes.find((n: any) => n.path === "/users/100/tags/");
  assert.ok(tagsNode, "tags/ deve existir");
  assert.equal(tagsNode.type, 1, "tags agora é type=1 (OBJECT)");

  const item1 = nodes.find((n: any) => n.path === "/users/100/tags/item_01");
  assert.ok(item1, "item_01 deve existir");
  assert.equal(item1.type, 5, "item_01 deve ser STRING");
  assert.equal(item1.text_value, '"mathematician"');

  const old0 = nodes.find((n: any) => n.path === "/users/100/tags/0");
  assert.equal(old0, undefined, "tags/0 deve ter sido deletado");

  const doc = await extract("/users/100");
  assert.ok((doc as any).tags, "tags deve existir");
  assert.equal((doc as any).tags.item_01, "mathematician");
  assert.equal((doc as any).tags.item_03, 400);

  console.log("   ✅ Exemplo 07 passou");
}

// ---------------------------------------------------------------------------
// Exemplo 09: Substituição parcial do documento
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Exemplo 09 — SET /users/100 (partial overwrite)");

  await ingest("users/100", {
    id: 100,
    name: "Alan Turing",
    address: { city: "Londres", zip: "12345" },
    tags: ["genius", "computer"],
  });
  await ingest("users/101", {
    id: 101,
    name: "Ada Lovelace",
    address: { city: "Londres", zip: "54321" },
    tags: ["pioneer", "programmer"],
  });

  await ingest("users/100", { id: 100, name: "Alan Mathison Turing" });

  const doc100 = await extract("/users/100");
  assert.equal((doc100 as any).id, 100);
  assert.equal((doc100 as any).name, "Alan Mathison Turing");
  assert.equal((doc100 as any).address, undefined);
  assert.equal((doc100 as any).tags, undefined);

  const doc101 = await extract("/users/101");
  assert.equal((doc101 as any).id, 101);
  assert.equal((doc101 as any).name, "Ada Lovelace");
  assert.ok((doc101 as any).address, "address de user 101 não foi afetado");

  console.log("   ✅ Exemplo 09 passou");
}

// ---------------------------------------------------------------------------
// Exemplo 10: Deleção de documento não afeta outros
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Exemplo 10 — SET /users/101 → null");

  await ingest("users/100", { id: 100, name: "Alan Mathison Turing" });
  await ingest("users/101", { id: 101, name: "Ada Lovelace" });

  await ingest("users/101", null);

  const doc101 = await extract("/users/101");
  assert.equal(doc101, null, "user 101 deletado → null");

  const doc100 = await extract("/users/100");
  assert.ok(doc100, "user 100 ainda existe");
  assert.equal((doc100 as any).name, "Alan Mathison Turing");

  console.log("   ✅ Exemplo 10 passou");
}

// ---------------------------------------------------------------------------
// Teste: Boolean roundtrip
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste — Boolean roundtrip");

  await ingest("test/bool", { active: true, verified: false });

  const doc = await extract("/test/bool");
  assert.equal((doc as any).active, true);
  assert.equal((doc as any).verified, false);

  const activeNode = (await rawNodes()).find((n: any) => n.path === "/test/bool/active");
  assert.equal(activeNode.type, 4, "boolean type = 4");
  assert.equal(activeNode.text_value, "true");

  console.log("   ✅ Boolean passou");
}

// ---------------------------------------------------------------------------
// Teste: Números (inteiros e reais)
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste — Number roundtrip");

  await ingest("test/numbers", { integer: 42, negative: -10, floating: 3.14 });

  const doc = await extract("/test/numbers");
  assert.equal((doc as any).integer, 42);
  assert.equal((doc as any).negative, -10);
  assert.equal((doc as any).floating, 3.14);

  console.log("   ✅ Numbers passou");
}

// ---------------------------------------------------------------------------
// Teste: JSON vazio e array vazio
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste — Objetos e arrays vazios");

  await ingest("test/empty", { emptyObj: {}, emptyArr: [] });

  const doc = await extract("/test/empty");
  assert.deepEqual((doc as any).emptyObj, {}, "objeto vazio");
  assert.deepEqual((doc as any).emptyArr, [], "array vazio");

  console.log("   ✅ Empty structures passou");
}

// ---------------------------------------------------------------------------
// Teste: Documento não existente
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste — Documento não existente retorna null");

  const result = await extract("/nonexistent/doc");
  assert.equal(result, null);

  console.log("   ✅ Nonexistent passou");
}

// ---------------------------------------------------------------------------
// Teste: String longa (>1KB) — round-trip sem truncamento
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste — String longa (10KB) round-trip");

  const longText = "a".repeat(10_000);
  await ingest("long/1", { content: longText, nested: { deep: longText } });

  const doc = await extract("/long/1");
  assert.ok(doc, "documento deve existir");
  assert.equal((doc as any).content, longText, "string 10KB deve ser íntegra");
  assert.equal((doc as any).nested.deep, longText, "string aninhada deve ser íntegra");
  assert.equal((doc as any).content.length, 10_000, "comprimento preservado");

  console.log("   ✅ Long string round-trip passou");
}

// ---------------------------------------------------------------------------
// Teste: take: 0 explícito retorna vazio (semântica correta de paginação)
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste — take:0 retorna array vazio");

  await ingest("q/1", { name: "One", age: 1 });
  await ingest("q/2", { name: "Two", age: 2 });

  const empty = await ctx.store.query("/q/*", { take: 0 });
  assert.deepEqual(empty, [], "take:0 deve retornar []");

  // Sem take → retorna tudo (default)
  const all = await ctx.store.query("/q/*", {});
  assert.equal(all.length, 2, "sem take deve retornar todos");

  console.log("   ✅ take:0 passou");
}

// ---------------------------------------------------------------------------
// Teste: get() em nó primitivo (ex: "/users/100/name")
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste — get() em nó primitivo");

  await ingest("users/100", { id: 100, name: "Alan Turing", active: true, age: 41 });

  assert.equal(await ctx.store.get("/users/100/name"), "Alan Turing", "get string primitiva");
  assert.equal(await ctx.store.get("/users/100/age"), 41, "get number primitivo");
  assert.equal(await ctx.store.get("/users/100/active"), true, "get boolean primitivo");

  console.log("   ✅ get() primitivo passou");
}

// ---------------------------------------------------------------------------
// Teste: searchByPath com segmentos UTF-8 (emoji/CJK)
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste — searchByPath com emoji/CJK");

  await ingest("people/😀", { name: "Smile" });
  await ingest("people/日本語", { name: "Nihongo" });
  await ingest("people/alice", { name: "Alice" });

  const children = await ctx.store.searchByPath("/people/");
  const paths = children.map((c: any) => c.path);
  // Containers são armazenados com trailing slash ("/people/😀/")
  assert.ok(paths.includes("/people/😀/"), "emoji deve aparecer");
  assert.ok(paths.includes("/people/日本語/"), "CJK deve aparecer");
  assert.ok(paths.includes("/people/alice/"), "ascii deve aparecer");

  console.log("   ✅ searchByPath UTF-8 passou");
}

// ---------------------------------------------------------------------------
// Teste: transaction() — rollback em caso de erro
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste — transaction() com rollback");

  await ctx.store.transaction(async (tx) => {
    await tx.set("/tx/a", { x: 1 });
    await tx.set("/tx/b", { x: 2 });
  });

  assert.ok(await ctx.store.get("/tx/a"), "commit deve persistir /tx/a");
  assert.ok(await ctx.store.get("/tx/b"), "commit deve persistir /tx/b");

  // Erro no meio → rollback total
  let threw = false;
  try {
    await ctx.store.transaction(async (tx) => {
      await tx.set("/tx/c", { x: 3 });
      throw new Error("forçar rollback");
    });
  } catch {
    threw = true;
  }
  assert.ok(threw, "transaction deve relançar o erro");
  assert.equal(await ctx.store.get("/tx/c"), null, "rollback deve desfazer /tx/c");
  assert.ok(await ctx.store.get("/tx/a"), "dados anteriores preservados");

  console.log("   ✅ transaction() passou");
}

// =============================================================================
// Encerramento
// =============================================================================
try {
  await ctx.cleanup();
  console.log("\n========================================");
  console.log("✅ Todos os testes PostgreSQL passaram!");
  console.log("========================================");
} catch (err) {
  console.error("❌ Falha nos testes:", err);
  process.exitCode = 1;
}
