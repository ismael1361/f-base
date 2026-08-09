import { strict as assert } from "node:assert";
import { setupLibsql, type LibsqlTestContext } from "./libsql-test-helper";
import { LibsqlHierarchicalStore } from "../libsql-store";

// =============================================================================
// Test Suite: Query single-tail com streaming + early-exit ("/pai/*")
// =============================================================================
// O motor agora processa queries de coleção ("/pai/*") SEM materializar o
// documento inteiro: varre as linhas uma vez (ORDER BY path), reconstrói os
// filhos diretos incrementalmente e aplica filtro + skip/take NA HORA.
// Com `order` (ou projeção), cai no pipeline antigo (fallback).
// Estes testes garantem PARIDADE de resultados entre os dois caminhos.
// =============================================================================

let ctx: LibsqlTestContext;

function beforeTest(): void {
  ctx.reset();
}

ctx = setupLibsql();

// ---------------------------------------------------------------------------
// 01: streaming — ordem lexicográfica dos filhos (paridade com ORDER BY path)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Streaming query 01 — ordem lexicográfica");

  ctx.store.set("/people/alice", { name: "Alice", age: 25 });
  ctx.store.set("/people/bob", { name: "Bob", age: 35 });
  ctx.store.set("/people/carol", { name: "Carol", age: 40 });

  const r = ctx.store.query("/people/*", { take: 10 }) as Array<{ name: string }>;
  assert.deepEqual(
    r.map((p) => p.name),
    ["Alice", "Bob", "Carol"],
    "ordem lexicográfica por path (alice < bob < carol)",
  );

  console.log("   ✅ ordem lexicográfica OK");
}

// ---------------------------------------------------------------------------
// 02: streaming — filtro numérico e de string
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Streaming query 02 — filtros");

  ctx.store.set("/people/alice", { name: "Alice", age: 25 });
  ctx.store.set("/people/bob", { name: "Bob", age: 35 });

  const adults = ctx.store.query("/people/*", { filters: [{ key: "age", op: ">", compare: 30 }] }) as Array<{ name: string }>;
  assert.equal(adults.length, 1, "age>30 → só Bob");
  assert.equal(adults[0].name, "Bob");

  const byName = ctx.store.query("/people/*", { filters: [{ key: "name", op: "==", compare: "Alice" }] }) as Array<{ name: string }>;
  assert.equal(byName.length, 1, "name==Alice");
  assert.equal(byName[0].name, "Alice");

  console.log("   ✅ filtros numérico e string OK");
}

// ---------------------------------------------------------------------------
// 03: streaming — skip/take com early-exit (não processa o doc todo)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Streaming query 03 — skip/take");

  // Doc grande: 200 campos (~2005 nós) — take pequeno deve parar cedo
  const doc: Record<string, unknown> = {};
  for (let i = 0; i < 200; i++) doc[`f_${i}`] = { value: i, label: `l-${i}` };
  ctx.store.set("/big", doc);

  const r = ctx.store.query("/big/*", { take: 5 }) as Array<{ value: number }>;
  assert.equal(r.length, 5, "take=5 devolve 5");
  assert.equal(r[0].value, 0, "começa no f_0");

  // skip em ordem lexicográfica de path: f_0, f_1, f_10, f_100, f_101, ...
  // skip=2 pula f_0 e f_1 → começa em f_10
  const paged = ctx.store.query("/big/*", { skip: 2, take: 3 }) as Array<{ value: number }>;
  assert.equal(paged.length, 3, "skip=2,take=3 devolve 3");
  assert.deepEqual(
    paged.map((p) => p.value),
    [10, 100, 101],
    "skip lexicográfico pula f_0,f_1 → f_10,f_100,f_101",
  );

  console.log("   ✅ skip/take OK");
}

// ---------------------------------------------------------------------------
// 04: streaming — doc inexistente e container vazio (paridade com o store TS)
// ---------------------------------------------------------------------------
// O query() do store converte SQL NULL em [] (idêntico ao pipeline antigo).
{
  beforeTest();
  console.log("\n🧪 Streaming query 04 — inexistente e vazio");

  const missing = ctx.store.query("/ghost/*", { take: 10 });
  assert.deepEqual(missing, [], "doc inexistente → [] (store converte SQL NULL)");

  ctx.store.set("/empty", {});
  const empty = ctx.store.query("/empty/*", { take: 10 });
  assert.deepEqual(empty, [], "container vazio → []");

  console.log("   ✅ inexistente e vazio → [] OK");
}

// ---------------------------------------------------------------------------
// 05: streaming — filhos com subárvore dedicada (aninhados) preservados
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Streaming query 05 — filhos aninhados");

  ctx.store.set("/people/carol", { name: "Carol", address: { city: "SP", zip: "00000" } });
  const r = ctx.store.query("/people/*", { take: 10 }) as Array<{ name: string; address: { city: string } }>;
  assert.equal(r.length, 1);
  assert.deepEqual(r[0].address, { city: "SP", zip: "00000" }, "subárvore dedicada preservada");

  console.log("   ✅ aninhados preservados OK");
}

// ---------------------------------------------------------------------------
// 06: paridade — streaming (sem order) vs pipeline antigo (forçado via include)
// ---------------------------------------------------------------------------
// O streaming (sem order) usa ordem lexicográfica de path. Para comparar com
// o pipeline antigo, forçamos o fallback passando uma projeção include
// (que desliga o streaming) — o resultado DEVE ser idêntico.
{
  beforeTest();
  console.log("\n🧪 Streaming query 06 — paridade streaming vs fallback");

  const doc: Record<string, unknown> = {};
  for (let i = 0; i < 50; i++) doc[`k_${i}`] = { n: i, name: `n-${i}` };
  ctx.store.set("/parity", doc);

  // Fallback (projeção desliga o streaming)
  const fallback = ctx.store.query("/parity/*", { take: 4, include: ["n", "name"] }) as Array<{ n: number }>;
  // Streaming (sem projeção)
  const streaming = ctx.store.query("/parity/*", { take: 4 }) as Array<{ n: number }>;

  assert.deepEqual(
    streaming.map((x) => x.n),
    fallback.map((x) => x.n),
    "streaming e fallback produzem a mesma ordem lexicográfica (k_0,k_1,k_10,k_100)",
  );

  console.log("   ✅ paridade streaming/fallback OK");
}

// ---------------------------------------------------------------------------
// 07: paridade — filtro com projeção cai no fallback e mantém comportamento
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Streaming query 07 — projeção cai no fallback");

  ctx.store.set("/people/alice", { name: "Alice", age: 25, password: "x" });
  ctx.store.set("/people/bob", { name: "Bob", age: 35, password: "y" });

  const r = ctx.store.query("/people/*", { filters: [{ key: "age", op: ">", compare: 30 }], exclude: ["password"] }) as Array<{ name: string; age: number }>;
  assert.equal(r.length, 1);
  assert.deepEqual(r[0], { name: "Bob", age: 35 }, "exclude aplicado (fallback com projeção)");

  console.log("   ✅ projeção + filtro (fallback) OK");
}

console.log("✅ Suíte de streaming query concluída");
