import { Readable } from "stream";
import { strict as assert } from "node:assert";
import { setupPg, type PgTestContext } from "./pg-test-helper.js";

// =============================================================================
// Test Suite: Import/Export (PostgreSQL)
// Paridade com src/import-export.test.ts (SQLite), agora sobre embedded-postgres
// =============================================================================

let ctx: PgTestContext;

async function ingest(docId: string, data: object | null): Promise<void> {
  await ctx.store.set(docId, data);
}

async function extract(prefix: string): Promise<object | null> {
  return ctx.store.get(prefix);
}

async function exportJson(prefix: string): Promise<string | null> {
  return ctx.store.export(prefix, "json").then((b) => b.toString("utf-8"));
}

async function exportCsv(prefix: string): Promise<string | null> {
  return ctx.store.export(prefix, "csv").then((b) => b.toString("utf-8"));
}

async function importCsv(prefix: string, csv: string): Promise<void> {
  await ctx.client.query("SELECT import_csv($1, $2)", [prefix, csv]);
}

async function storeImport(pathPrefix: string, data: Buffer | Readable, type: "json" | "csv" = "json"): Promise<void> {
  await ctx.store.import(pathPrefix, data, type);
}

async function storeExport(pathPrefix: string, type: "json" | "csv" = "json"): Promise<Buffer> {
  return ctx.store.export(pathPrefix, type);
}

async function beforeTest(): Promise<void> {
  await ctx.reset();
}

ctx = await setupPg();

// ---------------------------------------------------------------------------
// Teste 01: Export JSON - documento simples
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste 01 — Export JSON de documento simples");

  await ingest("users/100", { id: 100, name: "Alan Turing", age: 41, active: true });

  const parsed = JSON.parse((await exportJson("/users/100")) as string);
  assert.equal(parsed.id, 100);
  assert.equal(parsed.name, "Alan Turing");
  assert.equal(parsed.age, 41);
  assert.equal(parsed.active, true);

  console.log("   ✅ Export JSON funcionou");
}

// ---------------------------------------------------------------------------
// Teste 02: Export JSON - documento aninhado
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste 02 — Export JSON de documento aninhado");

  await ingest("users/200", {
    id: 200,
    name: "Ada Lovelace",
    address: { city: "Londres", zip: "12345" },
    tags: ["genius", "pioneer"],
  });

  const parsed = JSON.parse((await exportJson("/users/200")) as string);
  assert.equal(parsed.name, "Ada Lovelace");
  assert.deepEqual(parsed.address, { city: "Londres", zip: "12345" });
  assert.deepEqual(parsed.tags, ["genius", "pioneer"]);

  console.log("   ✅ Export JSON aninhado funcionou");
}

// ---------------------------------------------------------------------------
// Teste 03: Export JSON - prefixo com múltiplos filhos
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste 03 — Export JSON de prefixo com múltiplos filhos");

  await ingest("people/alice", { name: "Alice", age: 25 });
  await ingest("people/bob", { name: "Bob", age: 35 });
  await ingest("people/charlie", { name: "Charlie", age: 20 });

  const parsed = JSON.parse((await exportJson("/people")) as string);
  assert.ok(parsed.alice, "alice deve existir");
  assert.ok(parsed.bob, "bob deve existir");
  assert.ok(parsed.charlie, "charlie deve existir");
  assert.equal(parsed.alice.name, "Alice");
  assert.equal(parsed.bob.age, 35);

  console.log("   ✅ Export JSON multi-filhos funcionou");
}

// ---------------------------------------------------------------------------
// Teste 04: Export JSON - documento inexistente retorna "null"
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste 04 — Export JSON de documento inexistente");

  const jsonStr = await exportJson("/nonexistent/path");
  assert.equal(jsonStr, "null");

  console.log("   ✅ Export JSON inexistente retorna 'null'");
}

// ---------------------------------------------------------------------------
// Teste 05: Export CSV - documento simples
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste 05 — Export CSV de documento simples");

  await ingest("test/1", { name: "Test", value: 42 });

  const csvStr = (await exportCsv("/test/1")) as string;
  assert.ok(csvStr.startsWith("path,type,text_value\n"), "CSV deve ter header");
  assert.ok(csvStr.includes("/test/1/"), "Deve conter container /test/1/");
  assert.ok(csvStr.includes("/test/1/name"), "Deve conter nó name");
  assert.ok(csvStr.includes("/test/1/value"), "Deve conter nó value");

  console.log("   ✅ Export CSV funcionou");
}

// ---------------------------------------------------------------------------
// Teste 06: Export CSV - prefixo com múltiplos documentos
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste 06 — Export CSV de prefixo com múltiplos documentos");

  await ingest("people/alice", { name: "Alice", age: 25 });
  await ingest("people/bob", { name: "Bob", age: 35 });

  const csvStr = (await exportCsv("/people")) as string;
  assert.ok(csvStr.includes("/people/"), "Deve conter /people/");
  assert.ok(csvStr.includes("/people/alice/"), "Deve conter /people/alice/");
  assert.ok(csvStr.includes("/people/bob/"), "Deve conter /people/bob/");
  assert.ok(csvStr.includes("/people/alice/name"), "Deve conter nó alice/name");

  const lines = csvStr.trim().split("\n");
  assert.ok(lines.length >= 7, `Deve ter pelo menos 7 linhas, tem ${lines.length}`);

  console.log("   ✅ Export CSV multi-doc funcionou");
}

// ---------------------------------------------------------------------------
// Teste 07: Export CSV - valores com caracteres especiais
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste 07 — Export CSV com caracteres especiais");

  await ingest("special/1", {
    withComma: "hello, world",
    withQuotes: 'He said "hi"',
    withNewline: "line1\nline2",
    simple: "normal",
  });

  const csvStr = (await exportCsv("/special/1")) as string;
  assert.ok(csvStr.includes('"hello, world"'), "Vírgula deve estar entre aspas");

  console.log("   ✅ Export CSV com especiais funcionou");
}

// ---------------------------------------------------------------------------
// Teste 08: Export CSV - documento inexistente retorna apenas header
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste 08 — Export CSV de documento inexistente");

  const csvStr = await exportCsv("/nonexistent/path");
  assert.equal(csvStr, "path,type,text_value\n");

  console.log("   ✅ Export CSV inexistente retorna header vazio");
}

// ---------------------------------------------------------------------------
// Teste 09: Import CSV básico e round-trip
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste 09 — Import CSV básico + round-trip");

  const csvData = `path,type,text_value
"/imported/",1,"{}"
"/imported/alice/",1,"{}"
"/imported/alice/name",5,"""Alice"""
"/imported/alice/age",3,"30"
"/imported/bob/",1,"{}"
"/imported/bob/name",5,"""Bob"""
"/imported/bob/age",3,"25"`;

  await storeImport("/imported", Buffer.from(csvData), "csv");

  const alice = await extract("/imported/alice");
  assert.ok(alice, "alice deve existir");
  assert.equal((alice as any).name, "Alice");
  assert.equal((alice as any).age, 30);

  const bob = await extract("/imported/bob");
  assert.ok(bob, "bob deve existir");
  assert.equal((bob as any).name, "Bob");
  assert.equal((bob as any).age, 25);

  console.log("   ✅ Import CSV + round-trip funcionou");
}

// ---------------------------------------------------------------------------
// Teste 10: Import CSV com caracteres especiais
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste 10 — Import CSV com caracteres especiais");

  const csvData = `path,type,text_value
"/special/",1,"{}"
"/special/msg1",5,"""Hello, world"""
"/special/msg2",5,"""He said ""hi"""""
"/special/num",3,"42"`;

  await storeImport("/special", Buffer.from(csvData), "csv");

  const data = await extract("/special");
  assert.ok(data, "dados devem existir");
  assert.equal((data as any).msg1, "Hello, world");
  assert.equal((data as any).num, 42);

  console.log("   ✅ Import CSV com especiais funcionou");
}

// ---------------------------------------------------------------------------
// Teste 11: Import CSV via Readable stream
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste 11 — Import CSV via Readable stream");

  const csvData = `path,type,text_value
"/stream/",1,"{}"
"/stream/x",5,"""from stream"""
"/stream/y",3,"99"`;

  const readable = Readable.from([csvData]);
  await storeImport("/stream", readable, "csv");

  const data = await extract("/stream");
  assert.ok(data, "dados devem existir");
  assert.equal((data as any).x, "from stream");
  assert.equal((data as any).y, 99);

  console.log("   ✅ Import CSV via stream funcionou");
}

// ---------------------------------------------------------------------------
// Teste 12: Import JSON básico
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste 12 — Import JSON básico");

  const jsonData = JSON.stringify({ id: 500, name: "From JSON Import", active: true });
  await storeImport("/json-imported", Buffer.from(jsonData), "json");

  const data = await extract("/json-imported");
  assert.ok(data, "dados devem existir");
  assert.equal((data as any).id, 500);
  assert.equal((data as any).name, "From JSON Import");
  assert.equal((data as any).active, true);

  console.log("   ✅ Import JSON funcionou");
}

// ---------------------------------------------------------------------------
// Teste 13: Import JSON via Readable stream
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste 13 — Import JSON via Readable stream");

  const jsonData = JSON.stringify({ stream_test: true, value: "from stream" });
  const readable = Readable.from([jsonData]);
  await storeImport("/json-stream", readable, "json");

  const data = await extract("/json-stream");
  assert.ok(data, "dados devem existir");
  assert.equal((data as any).stream_test, true);
  assert.equal((data as any).value, "from stream");

  console.log("   ✅ Import JSON via stream funcionou");
}

// ---------------------------------------------------------------------------
// Teste 14: Round-trip JSON → Export JSON → Import JSON
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste 14 — Round-trip JSON → Export → Import");

  await ingest("original/doc", {
    id: 999,
    name: "Original",
    nested: { key: "value" },
    arr: [1, 2, 3],
  });

  const exported = await storeExport("/original/doc", "json");
  await storeImport("/copy/doc", exported, "json");

  const original = await extract("/original/doc");
  const copy = await extract("/copy/doc");
  assert.deepEqual(original, copy, "dados devem ser idênticos após round-trip");

  console.log("   ✅ Round-trip JSON funcionou");
}

// ---------------------------------------------------------------------------
// Teste 15: Round-trip CSV → Export CSV → Import CSV
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste 15 — Round-trip CSV → Export → Import");

  await ingest("original/csv", { name: "CSV Test", value: 123, active: false });

  const exportedCsv = await storeExport("/original/csv", "csv");
  await storeImport("/copy/csv", exportedCsv, "csv");

  const original = await extract("/original/csv");
  const copy = await extract("/copy/csv");
  assert.ok(original, "original deve existir");
  assert.ok(copy, "copy deve existir");
  assert.equal((original as any).name, (copy as any).name);
  assert.equal((original as any).value, (copy as any).value);

  console.log("   ✅ Round-trip CSV funcionou");
}

// ---------------------------------------------------------------------------
// Teste 16: Import CSV vazio (só header)
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste 16 — Import CSV vazio (só header)");

  await storeImport("/empty", Buffer.from("path,type,text_value\n"), "csv");

  const data = await extract("/empty");
  assert.equal(data, null, "não deve criar dados");

  console.log("   ✅ Import CSV vazio não criou dados");
}

// ---------------------------------------------------------------------------
// Teste 17: Import JSON inválido deve lançar erro
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste 17 — Import JSON inválido deve lançar erro");

  let threw = false;
  try {
    await storeImport("/invalid", Buffer.from("not valid json"), "json");
  } catch (e) {
    threw = true;
  }
  assert.ok(threw, "Deve lançar erro para JSON inválido");

  console.log("   ✅ Import JSON inválido lançou erro");
}

// ---------------------------------------------------------------------------
// Teste 18: Import/Export default type é "json"
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste 18 — Default type é 'json'");

  const jsonData = JSON.stringify({ test: "default" });
  await storeImport("/default-test", Buffer.from(jsonData));

  const exported = await storeExport("/default-test");
  const parsed = JSON.parse(exported.toString("utf-8"));
  assert.equal(parsed.test, "default");

  console.log("   ✅ Default type 'json' funcionou");
}

// ---------------------------------------------------------------------------
// Teste 19: Export CSV com array
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste 19 — Export CSV com array");

  await ingest("with-array/1", { name: "With Array", tags: ["a", "b", "c"] });

  const csvStr = (await exportCsv("/with-array/1")) as string;
  assert.ok(csvStr.includes("/with-array/1/tags/"), "Deve conter container tags/");
  assert.ok(csvStr.includes("/with-array/1/tags/0"), "Deve conter nó tags/0");

  console.log("   ✅ Export CSV com array funcionou");
}

// ---------------------------------------------------------------------------
// Teste 20: Round-trip CSV com string longa (>1KB) — sem truncamento
// ---------------------------------------------------------------------------
{
  await beforeTest();
  console.log("\n🧪 Teste 20 — Round-trip CSV com string longa");

  const longText = "b".repeat(8_000);
  await ingest("long-csv/1", { content: longText });

  // Exporta → Importa em outro path → compara
  const exportedCsv = await storeExport("/long-csv/1", "csv");
  await storeImport("/long-csv-copy/1", exportedCsv, "csv");

  const original = await extract("/long-csv/1");
  const copy = await extract("/long-csv-copy/1");
  assert.equal((original as any).content, longText, "original íntegro");
  assert.equal((copy as any).content, longText, "cópia via CSV íntegra (8KB)");
  assert.equal((copy as any).content.length, 8_000, "comprimento preservado");

  console.log("   ✅ Round-trip CSV long string funcionou");
}

// =============================================================================
// Encerramento
// =============================================================================
try {
  await ctx.cleanup();
  console.log("\n========================================");
  console.log("✅ Todos os testes Import/Export PostgreSQL passaram!");
  console.log("========================================");
} catch (err) {
  console.error("❌ Falha nos testes:", err);
  process.exitCode = 1;
}
