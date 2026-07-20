import Database from "better-sqlite3";
import path from "path";
import fs from "fs";
import { strict as assert } from "node:assert";

// =============================================================================
// Test Suite: Hierarchical Engine
// Valida todos os exemplos do attachment (Untitled-1)
// =============================================================================

const DB_PATH = "db/test-hierarchical.db";
const EXT_PATH = path.resolve(process.cwd(), "./hierarchical_engine/bin", `hierarchical_engine.${process.platform === "win32" ? "dll" : process.platform === "darwin" ? "dylib" : "so"}`);

// Setup: banco limpo + extensão + schema
function setupDatabase(): Database.Database {
  // Remove banco anterior se existir
  try {
    fs.unlinkSync(DB_PATH);
  } catch {
    /* ignore */
  }

  const db = new Database(DB_PATH);
  db.pragma("journal_mode = WAL");
  db.pragma("synchronous = NORMAL");
  db.pragma("mmap_size = 268435456");
  db.pragma("cache_size = -64000");

  db.exec(`
    CREATE TABLE IF NOT EXISTS nodes (
        path TEXT PRIMARY KEY,
        type INTEGER NOT NULL,
        text_value TEXT,
        created INTEGER NOT NULL,
        modified INTEGER NOT NULL,
        revision_nr INTEGER NOT NULL,
        revision TEXT NOT NULL
    ) WITHOUT ROWID;
  `);

  // Carrega extensão C
  try {
    (db as any).loadExtension(EXT_PATH, "sqlite3_hierarchical_init");
  } catch (err) {
    console.error("❌ Falha ao carregar extensão C. Compile primeiro com build.sh");
    console.error(`   Caminho esperado: ${EXT_PATH}`);
    throw err;
  }

  return db;
}

// Helpers de teste
function rawNodes(db: Database.Database): any[] {
  return db.prepare("SELECT path, type, text_value FROM nodes ORDER BY path").all();
}

function ingest(db: Database.Database, docId: string, data: object | null): void {
  const jsonStr = JSON.stringify(data);
  db.prepare("SELECT ingest_json(?, ?)").get(docId, jsonStr);
}

function extract(db: Database.Database, prefix: string): object | null {
  const result = db.prepare("SELECT extract_json(?) as json_data").get(prefix) as any;
  return result?.json_data ? JSON.parse(result.json_data) : null;
}

// =============================================================================
// Testes
// =============================================================================

let db: Database.Database;

function afterTest() {
  try {
    db.close();
  } catch {
    /* ignore */
  }
}

// Hook: setup antes de cada teste
function beforeTest(): Database.Database {
  if (db) afterTest();
  db = setupDatabase();
  return db;
}

// ---------------------------------------------------------------------------
// Exemplo 01: SET documento aninhado
// ---------------------------------------------------------------------------
{
  db = beforeTest();

  console.log("\n🧪 Exemplo 01 — SET /users/100");

  ingest(db, "users/100", {
    id: 100,
    name: "Alan Turing",
    address: { city: "Londres", zip: "12345" },
    tags: ["genius", "computer"],
  });

  // Verifica nós armazenados
  const nodes = rawNodes(db);

  // Root / deve existir
  assert.ok(
    nodes.find((n: any) => n.path === "/"),
    "Root / deve existir",
  );

  // Container /users/ deve existir
  assert.ok(
    nodes.find((n: any) => n.path === "/users/"),
    "/users/ deve existir",
  );

  // Container /users/100/ deve existir
  assert.ok(
    nodes.find((n: any) => n.path === "/users/100/"),
    "/users/100/ deve existir",
  );

  // Primitivos
  const idNode = nodes.find((n: any) => n.path === "/users/100/id");
  assert.ok(idNode, "id node deve existir");
  assert.equal(idNode.type, 3, "id deve ser NUMBER (type=3)");
  assert.equal(idNode.text_value, "100", "id text_value = '100'");

  const nameNode = nodes.find((n: any) => n.path === "/users/100/name");
  assert.ok(nameNode, "name node deve existir");
  assert.equal(nameNode.type, 5, "name deve ser STRING (type=5)");
  assert.equal(nameNode.text_value, '"Alan Turing"', "name deve ter aspas");

  // Container address/ deve ter trailing slash
  const addrNode = nodes.find((n: any) => n.path === "/users/100/address/");
  assert.ok(addrNode, "/users/100/address/ deve existir");
  assert.equal(addrNode.type, 1, "address deve ser OBJECT (type=1)");
  assert.equal(addrNode.text_value, "{}", "address text_value = '{}'");

  // Tags container (type 2 = ARRAY)
  const tagsNode = nodes.find((n: any) => n.path === "/users/100/tags/");
  assert.ok(tagsNode, "/users/100/tags/ deve existir");
  assert.equal(tagsNode.type, 2, "tags deve ser ARRAY (type=2)");
  assert.equal(tagsNode.text_value, "{}", "tags text_value = '{}'");

  // Tags items
  const tag0 = nodes.find((n: any) => n.path === "/users/100/tags/0");
  assert.ok(tag0, "tags/0 deve existir");
  assert.equal(tag0.type, 5, "tags/0 deve ser STRING");
  assert.equal(tag0.text_value, '"genius"', "tags/0 = genius");

  const tag1 = nodes.find((n: any) => n.path === "/users/100/tags/1");
  assert.equal(tag1.type, 5, "tags/1 deve ser STRING");
  assert.equal(tag1.text_value, '"computer"');

  console.log("   ✅ Exemplo 01 passou");
  afterTest();
}

// ---------------------------------------------------------------------------
// Exemplo 04: SET null (deleção)
// ---------------------------------------------------------------------------
{
  db = beforeTest();

  console.log("\n🧪 Exemplo 04 — SET /users/100/address → null");

  // Primeiro insere documento completo
  ingest(db, "users/100", {
    id: 100,
    name: "Alan Turing",
    address: { city: "Londres", zip: "12345" },
    tags: ["genius", "computer"],
  });

  // Depois deleta address com null
  ingest(db, "users/100/address", null);

  const nodes = rawNodes(db);
  const hasAddress = nodes.some((n: any) => n.path.startsWith("/users/100/address"));
  assert.ok(!hasAddress, "address e descendentes devem ter sido deletados");

  // O restante do documento deve estar intacto
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

  const addrAfter = extract(db, "/users/100/address");
  assert.equal(addrAfter, null, "GET /users/100/address deve retornar null");

  console.log("   ✅ Exemplo 04 + 05 passaram");
  afterTest();
}

// ---------------------------------------------------------------------------
// Exemplo 06: GET mantém estrutura (address deletado)
// ---------------------------------------------------------------------------
{
  db = beforeTest();

  console.log("\n🧪 Exemplo 06 — GET /users/100 sem address");

  ingest(db, "users/100", {
    id: 100,
    name: "Alan Turing",
    address: { city: "Londres", zip: "12345" },
    tags: ["genius", "computer"],
  });

  // Deleta address
  ingest(db, "users/100/address", null);

  const doc = extract(db, "/users/100");
  assert.ok(doc, "documento deve existir");
  assert.equal((doc as any).id, 100, "id = 100");
  assert.equal((doc as any).name, "Alan Turing", "name = Alan Turing");
  assert.equal((doc as any).address, undefined, "address deve ter sido removido");

  // tags deve ser array (pois type=2 → yyjson_mut_arr)
  assert.ok(Array.isArray((doc as any).tags), "tags deve ser array");
  assert.equal((doc as any).tags[0], "genius", "tags[0] = genius");
  assert.equal((doc as any).tags[1], "computer", "tags[1] = computer");

  console.log("   ✅ Exemplo 06 passou");
  afterTest();
}

// ---------------------------------------------------------------------------
// Exemplo 07: SET substitui tags (array → objeto nomeado)
// ---------------------------------------------------------------------------
{
  db = beforeTest();

  console.log("\n🧪 Exemplo 07 — SET /users/100/tags → objeto nomeado");

  ingest(db, "users/100", {
    id: 100,
    name: "Alan Turing",
    tags: ["genius", "computer"],
  });

  // Substitui tags por objeto nomeado
  ingest(db, "users/100/tags", {
    item_01: "mathematician",
    item_02: "logician",
    item_03: 400,
  });

  const nodes = rawNodes(db);

  // Tags container deve existir
  const tagsNode = nodes.find((n: any) => n.path === "/users/100/tags/");
  assert.ok(tagsNode, "tags/ deve existir");
  assert.equal(tagsNode.type, 1, "tags agora é type=1 (OBJECT)");
  assert.equal(tagsNode.text_value, "{}");

  // Tags items
  const item1 = nodes.find((n: any) => n.path === "/users/100/tags/item_01");
  assert.ok(item1, "item_01 deve existir");
  assert.equal(item1.type, 5, "item_01 deve ser STRING");
  assert.equal(item1.text_value, '"mathematician"');

  const item3 = nodes.find((n: any) => n.path === "/users/100/tags/item_03");
  assert.ok(item3, "item_03 deve existir");
  assert.equal(item3.type, 3, "item_03 deve ser NUMBER");
  assert.equal(item3.text_value, "400");

  // Itens antigos (0, 1) devem ter sido removidos
  const old0 = nodes.find((n: any) => n.path === "/users/100/tags/0");
  assert.equal(old0, undefined, "tags/0 deve ter sido deletado");

  const doc = extract(db, "/users/100");
  assert.ok((doc as any).tags, "tags deve existir");
  assert.equal((doc as any).tags.item_01, "mathematician");
  assert.equal((doc as any).tags.item_03, 400);

  console.log("   ✅ Exemplo 07 passou");
  afterTest();
}

// ---------------------------------------------------------------------------
// Exemplo 09: Substituição parcial do documento
// ---------------------------------------------------------------------------
{
  db = beforeTest();

  console.log("\n🧪 Exemplo 09 — SET /users/100 (partial overwrite)");

  // Insere documento completo
  ingest(db, "users/100", {
    id: 100,
    name: "Alan Turing",
    address: { city: "Londres", zip: "12345" },
    tags: ["genius", "computer"],
  });

  // Insere segundo documento
  ingest(db, "users/101", {
    id: 101,
    name: "Ada Lovelace",
    address: { city: "Londres", zip: "54321" },
    tags: ["pioneer", "programmer"],
  });

  // Substitui apenas user 100 (sobrescrita completa)
  ingest(db, "users/100", {
    id: 100,
    name: "Alan Mathison Turing",
  });

  const doc100 = extract(db, "/users/100");
  assert.equal((doc100 as any).id, 100);
  assert.equal((doc100 as any).name, "Alan Mathison Turing");
  // address e tags foram removidos (overwrite completo substitui o subtree)
  assert.equal((doc100 as any).address, undefined);
  assert.equal((doc100 as any).tags, undefined);

  // User 101 não foi afetado
  const doc101 = extract(db, "/users/101");
  assert.equal((doc101 as any).id, 101);
  assert.equal((doc101 as any).name, "Ada Lovelace");
  assert.ok((doc101 as any).address, "address de user 101 não foi afetado");

  console.log("   ✅ Exemplo 09 passou");
  afterTest();
}

// ---------------------------------------------------------------------------
// Exemplo 10: Deleção de documento não afeta outros
// ---------------------------------------------------------------------------
{
  db = beforeTest();

  console.log("\n🧪 Exemplo 10 — SET /users/101 → null");

  ingest(db, "users/100", {
    id: 100,
    name: "Alan Mathison Turing",
  });

  ingest(db, "users/101", {
    id: 101,
    name: "Ada Lovelace",
  });

  // Deleta user 101
  ingest(db, "users/101", null);

  const doc101 = extract(db, "/users/101");
  assert.equal(doc101, null, "user 101 deletado → null");

  const doc100 = extract(db, "/users/100");
  assert.ok(doc100, "user 100 ainda existe");
  assert.equal((doc100 as any).name, "Alan Mathison Turing");

  console.log("   ✅ Exemplo 10 passou");
  afterTest();
}

// ---------------------------------------------------------------------------
// Teste: Boolean
// ---------------------------------------------------------------------------
{
  db = beforeTest();

  console.log("\n🧪 Teste — Boolean roundtrip");

  ingest(db, "test/bool", {
    active: true,
    verified: false,
  });

  const doc = extract(db, "/test/bool");
  assert.equal((doc as any).active, true);
  assert.equal((doc as any).verified, false);

  // Verifica storage
  const activeNode = rawNodes(db).find((n: any) => n.path === "/test/bool/active");
  assert.equal(activeNode.type, 4, "boolean type = 4");
  assert.equal(activeNode.text_value, "true");

  console.log("   ✅ Boolean passou");
  afterTest();
}

// ---------------------------------------------------------------------------
// Teste: Números (inteiros e reais)
// ---------------------------------------------------------------------------
{
  db = beforeTest();

  console.log("\n🧪 Teste — Number roundtrip");

  ingest(db, "test/numbers", {
    integer: 42,
    negative: -10,
    floating: 3.14,
  });

  const doc = extract(db, "/test/numbers");
  assert.equal((doc as any).integer, 42);
  assert.equal((doc as any).negative, -10);
  assert.equal((doc as any).floating, 3.14);

  console.log("   ✅ Numbers passou");
  afterTest();
}

// ---------------------------------------------------------------------------
// Teste: JSON vazio e array vazio
// ---------------------------------------------------------------------------
{
  db = beforeTest();

  console.log("\n🧪 Teste — Objetos e arrays vazios");

  ingest(db, "test/empty", {
    emptyObj: {},
    emptyArr: [],
  });

  const doc = extract(db, "/test/empty");
  assert.deepEqual((doc as any).emptyObj, {}, "objeto vazio");
  assert.deepEqual((doc as any).emptyArr, [], "array vazio");

  console.log("   ✅ Empty structures passou");
  afterTest();
}

// ---------------------------------------------------------------------------
// Teste: Documento não existente
// ---------------------------------------------------------------------------
{
  db = beforeTest();

  console.log("\n🧪 Teste — Documento não existente retorna null");

  const result = extract(db, "/nonexistent/doc");
  assert.equal(result, null);

  console.log("   ✅ Nonexistent passou");
  afterTest();
}

// =============================================================================
// Resumo
// =============================================================================
console.log("\n========================================");
console.log("✅ Todos os testes passaram!");
console.log("========================================");
