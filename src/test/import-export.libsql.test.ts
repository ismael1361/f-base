import { strict as assert } from "node:assert";
import { Readable } from "stream";
import { setupLibsql, type LibsqlTestContext } from "./libsql-test-helper";

// =============================================================================
// Test Suite: Import/Export (JSON e CSV) — libSQL / Turso embedded
// Paridade com src/import-export.test.ts (SQLite), agora via LibsqlHierarchicalStore
// =============================================================================

let ctx: LibsqlTestContext;

// Helpers
function ingest(docId: string, data: object | null): void {
  ctx.store.set(docId, data);
}

function extract(prefix: string): object | null {
  return ctx.store.get(prefix);
}

async function storeImport(pathPrefix: string, data: Buffer | Readable, type: "json" | "csv" = "json"): Promise<void> {
  await ctx.store.import(pathPrefix, data, type);
}

function storeExport(pathPrefix: string, type: "json" | "csv" = "json"): Buffer {
  return ctx.store.export(pathPrefix, type);
}

function beforeTest(): void {
  ctx.reset();
}

// Inicializa o banco de teste (memória + extensão C)
ctx = setupLibsql();

// ---------------------------------------------------------------------------
// Teste 01: Export JSON - documento simples
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Teste 01 — Export JSON de documento simples");

  ingest("users/100", {
    id: 100,
    name: "Alan Turing",
    age: 41,
    active: true,
  });

  const jsonBuf = storeExport("/users/100", "json");
  const jsonStr = jsonBuf.toString("utf-8");
  const parsed = JSON.parse(jsonStr);

  assert.equal(parsed.id, 100);
  assert.equal(parsed.name, "Alan Turing");
  assert.equal(parsed.age, 41);
  assert.equal(parsed.active, true);

  console.log("   ✅ Export JSON funcionou:", jsonStr.slice(0, 60) + "...");
}

// ---------------------------------------------------------------------------
// Teste 02: Export JSON - documento aninhado
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Teste 02 — Export JSON de documento aninhado");

  ingest("users/200", {
    id: 200,
    name: "Ada Lovelace",
    address: { city: "Londres", zip: "12345" },
    tags: ["genius", "pioneer"],
  });

  const jsonBuf = storeExport("/users/200", "json");
  const parsed = JSON.parse(jsonBuf.toString("utf-8"));

  assert.equal(parsed.name, "Ada Lovelace");
  assert.deepEqual(parsed.address, { city: "Londres", zip: "12345" });
  // tags: array literal → objeto com chaves UUID (modelo objeto-only)
  const tags = parsed.tags as Record<string, string>;
  assert.ok(tags && !Array.isArray(tags), "tags deve ser objeto (não array)");
  const tagKeys = Object.keys(tags);
  assert.equal(tagKeys.length, 2, "tags com 2 elementos");
  assert.ok(
    tagKeys.every((k) => /^[0-9a-f]{32}$/.test(k)),
    "chaves UUID (32 hex)",
  );
  assert.deepEqual(Object.values(tags).sort(), ["genius", "pioneer"], "valores preservados");

  console.log("   ✅ Export JSON aninhado funcionou");
}

// ---------------------------------------------------------------------------
// Teste 03: Export JSON - prefixo com múltiplos filhos
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Teste 03 — Export JSON de prefixo com múltiplos filhos");

  ingest("people/alice", { name: "Alice", age: 25 });
  ingest("people/bob", { name: "Bob", age: 35 });
  ingest("people/charlie", { name: "Charlie", age: 20 });

  const jsonBuf = storeExport("/people", "json");
  const parsed = JSON.parse(jsonBuf.toString("utf-8"));

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
  beforeTest();
  console.log("\n🧪 Teste 04 — Export JSON de documento inexistente");

  const jsonStr = storeExport("/nonexistent/path", "json").toString("utf-8");
  assert.equal(jsonStr, "null");

  console.log("   ✅ Export JSON inexistente retorna 'null'");
}

// ---------------------------------------------------------------------------
// Teste 05: Export CSV - documento simples
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Teste 05 — Export CSV de documento simples");

  ingest("test/1", { name: "Test", value: 42 });

  const csvStr = storeExport("/test/1", "csv").toString("utf-8");

  assert.ok(csvStr.startsWith("path,type,text_value\n"), "CSV deve ter header");
  assert.ok(csvStr.includes("/test/1/"), "Deve conter container /test/1/");
  assert.ok(csvStr.includes("/test/1/name"), "Deve conter nó name");
  assert.ok(csvStr.includes("/test/1/value"), "Deve conter nó value");
  assert.ok(csvStr.includes("Test") || csvStr.includes('"Test"'), "Deve conter valor Test");
  assert.ok(csvStr.includes("42"), "Deve conter valor 42");

  console.log("   ✅ Export CSV funcionou:");
  console.log(
    csvStr
      .split("\n")
      .map((l) => "      " + l)
      .join("\n"),
  );
}

// ---------------------------------------------------------------------------
// Teste 06: Export CSV - prefixo com múltiplos documentos
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Teste 06 — Export CSV de prefixo com múltiplos documentos");

  ingest("people/alice", { name: "Alice", age: 25 });
  ingest("people/bob", { name: "Bob", age: 35 });

  const csvStr = storeExport("/people", "csv").toString("utf-8");

  assert.ok(csvStr.includes("/people/"), "Deve conter /people/");
  assert.ok(csvStr.includes("/people/alice/"), "Deve conter /people/alice/");
  assert.ok(csvStr.includes("/people/bob/"), "Deve conter /people/bob/");
  assert.ok(csvStr.includes("/people/alice/name"), "Deve conter nó alice/name");
  assert.ok(csvStr.includes("/people/bob/name"), "Deve conter nó bob/name");

  const lines = csvStr.trim().split("\n");
  assert.ok(lines.length >= 7, `Deve ter pelo menos 7 linhas (header + 6+ nós), tem ${lines.length}`);

  console.log("   ✅ Export CSV multi-doc funcionou:", lines.length, "linhas");
}

// ---------------------------------------------------------------------------
// Teste 07: Export CSV - valores com caracteres especiais
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Teste 07 — Export CSV com caracteres especiais");

  ingest("special/1", {
    withComma: "hello, world",
    withQuotes: 'He said "hi"',
    withNewline: "line1\nline2",
    simple: "normal",
  });

  const csvStr = storeExport("/special/1", "csv").toString("utf-8");

  assert.ok(csvStr.includes('"hello, world"'), "Vírgula deve estar entre aspas");

  console.log("   ✅ Export CSV com especiais funcionou:");
  console.log(
    csvStr
      .split("\n")
      .map((l) => "      " + l)
      .join("\n"),
  );
}

// ---------------------------------------------------------------------------
// Teste 08: Export CSV - documento inexistente retorna apenas header
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Teste 08 — Export CSV de documento inexistente");

  const csvStr = storeExport("/nonexistent/path", "csv").toString("utf-8");
  assert.equal(csvStr, "path,type,text_value\n");

  console.log("   ✅ Export CSV inexistente retorna header vazio");
}

// ---------------------------------------------------------------------------
// Teste 09: Import CSV básico e round-trip
// ---------------------------------------------------------------------------
{
  beforeTest();
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

  const alice = extract("/imported/alice");
  assert.ok(alice, "alice deve existir");
  assert.equal((alice as any).name, "Alice");
  assert.equal((alice as any).age, 30);

  const bob = extract("/imported/bob");
  assert.ok(bob, "bob deve existir");
  assert.equal((bob as any).name, "Bob");
  assert.equal((bob as any).age, 25);

  console.log("   ✅ Import CSV + round-trip funcionou");
}

// ---------------------------------------------------------------------------
// Teste 10: Import CSV com caracteres especiais
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Teste 10 — Import CSV com caracteres especiais");

  const csvData = `path,type,text_value
"/special/",1,"{}"
"/special/msg1",5,"""Hello, world"""
"/special/msg2",5,"""He said ""hi"""""
"/special/num",3,"42"`;

  await storeImport("/special", Buffer.from(csvData), "csv");

  const data = extract("/special");
  assert.ok(data, "dados devem existir");
  assert.equal((data as any).msg1, "Hello, world");
  assert.equal((data as any).num, 42);

  console.log("   ✅ Import CSV com especiais funcionou");
}

// ---------------------------------------------------------------------------
// Teste 11: Import CSV via Readable stream
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Teste 11 — Import CSV via Readable stream");

  const csvData = `path,type,text_value
"/stream/",1,"{}"
"/stream/x",5,"""from stream"""
"/stream/y",3,"99"`;

  const readable = Readable.from([csvData]);
  await storeImport("/stream", readable, "csv");

  const data = extract("/stream");
  assert.ok(data, "dados devem existir");
  assert.equal((data as any).x, "from stream");
  assert.equal((data as any).y, 99);

  console.log("   ✅ Import CSV via stream funcionou");
}

// ---------------------------------------------------------------------------
// Teste 12: Import JSON básico
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Teste 12 — Import JSON básico");

  const jsonData = JSON.stringify({ id: 500, name: "From JSON Import", active: true });

  await storeImport("/json-imported", Buffer.from(jsonData), "json");

  const data = extract("/json-imported");
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
  beforeTest();
  console.log("\n🧪 Teste 13 — Import JSON via Readable stream");

  const jsonData = JSON.stringify({ stream_test: true, value: "from stream" });
  const readable = Readable.from([jsonData]);

  await storeImport("/json-stream", readable, "json");

  const data = extract("/json-stream");
  assert.ok(data, "dados devem existir");
  assert.equal((data as any).stream_test, true);
  assert.equal((data as any).value, "from stream");

  console.log("   ✅ Import JSON via stream funcionou");
}

// ---------------------------------------------------------------------------
// Teste 14: Round-trip JSON → Export JSON → Import JSON
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Teste 14 — Round-trip JSON → Export → Import");

  ingest("original/doc", {
    id: 999,
    name: "Original",
    nested: { key: "value" },
    arr: [1, 2, 3],
  });

  const exported = storeExport("/original/doc", "json");
  await storeImport("/copy/doc", exported, "json");

  const original = extract("/original/doc");
  const copy = extract("/copy/doc");

  assert.deepEqual(original, copy, "dados devem ser idênticos após round-trip");

  console.log("   ✅ Round-trip JSON funcionou");
}

// ---------------------------------------------------------------------------
// Teste 15: Round-trip CSV → Export CSV → Import CSV
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Teste 15 — Round-trip CSV → Export → Import");

  ingest("original/csv", { name: "CSV Test", value: 123, active: false });

  const exportedCsv = storeExport("/original/csv", "csv");
  await storeImport("/copy/csv", exportedCsv, "csv");

  const original = extract("/original/csv");
  const copy = extract("/copy/csv");

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
  beforeTest();
  console.log("\n🧪 Teste 16 — Import CSV vazio (só header)");

  const csvData = "path,type,text_value\n";

  await storeImport("/empty", Buffer.from(csvData), "csv");

  const data = extract("/empty");
  assert.equal(data, null);

  console.log("   ✅ Import CSV vazio não criou dados");
}

// ---------------------------------------------------------------------------
// Teste 17: Import JSON inválido deve lançar erro
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Teste 17 — Import JSON inválido deve lançar erro");

  let threw = false;
  try {
    await storeImport("/invalid", Buffer.from("not valid json"), "json");
  } catch {
    threw = true;
  }
  assert.ok(threw, "Deve lançar erro para JSON inválido");

  console.log("   ✅ Import JSON inválido lançou erro");
}

// ---------------------------------------------------------------------------
// Teste 18: Import/Export default type é "json"
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Teste 18 — Default type é 'json'");

  const jsonData = JSON.stringify({ test: "default" });
  await storeImport("/default-test", Buffer.from(jsonData)); // sem type

  const exported = storeExport("/default-test"); // sem type
  const parsed = JSON.parse(exported.toString("utf-8"));

  assert.equal(parsed.test, "default");

  console.log("   ✅ Default type 'json' funcionou");
}

// ---------------------------------------------------------------------------
// Teste 19: Export CSV com array
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Teste 19 — Export CSV com array");

  ingest("with-array/1", { name: "With Array", tags: ["a", "b", "c"] });

  const csvStr = storeExport("/with-array/1", "csv").toString("utf-8");

  assert.ok(csvStr.includes("/with-array/1/tags/"), "Deve conter container tags/");
  // Filhos com chave UUID (32 hex) — nunca índice numérico (modelo objeto-only)
  const uuidLeaf = /\/with-array\/1\/tags\/[0-9a-f]{32}/.test(csvStr);
  assert.ok(uuidLeaf, "Deve conter nós tags/<uuid> (chave UUID)");

  console.log("   ✅ Export CSV com array funcionou");
}

// ---------------------------------------------------------------------------
// Teste 20: Import CSV com boolean
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Teste 20 — Import CSV com boolean");

  const csvData = `path,type,text_value
"/bool-test/",1,"{}"
"/bool-test/active",4,"true"
"/bool-test/inactive",4,"false"
"/bool-test/count",3,"10"`;

  await storeImport("/bool-test", Buffer.from(csvData), "csv");

  const data = extract("/bool-test");
  assert.ok(data, "dados devem existir");
  assert.equal((data as any).active, true);
  assert.equal((data as any).inactive, false);
  assert.equal((data as any).count, 10);

  console.log("   ✅ Import CSV com boolean funcionou");
}

// =============================================================================
// Resumo
// =============================================================================
console.log("\n========================================");
console.log("✅ Todos os testes import/export libSQL passaram!");
console.log("========================================");

ctx.cleanup();
