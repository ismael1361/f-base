import { strict as assert } from "node:assert";
import { setupLibsql, type LibsqlTestContext } from "./libsql-test-helper";
import { LibsqlHierarchicalStore } from "../libsql-store";

// =============================================================================
// Test Suite: Projeção include/exclude (get, query, searchByPath, import)
// =============================================================================
// Semântica:
//   - Caminhos pontilhados ("address.city") — exclude SEMPRE vence include.
//   - Ancestrais de caminhos pontilhados viram containers de passagem.
//   - searchByPath/import CSV filtram por padrões glob de path
//     ("*" = 1 segmento, "**" = qualquer profundidade).
// =============================================================================

let ctx: LibsqlTestContext;

function beforeTest(): void {
  ctx.reset();
}

ctx = setupLibsql();

// ---------------------------------------------------------------------------
// 01: query com include (campos top-level + pontilhados)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Projeção 01 — query include");

  ctx.store.set("/people/alice", { name: "Alice", age: 25, address: { city: "Londres", zip: "12345" }, password: "x" });
  ctx.store.set("/people/bob", { name: "Bob", age: 35, address: { city: "NYC", zip: "99999" }, password: "y" });

  const r = ctx.store.query("/people/*", { include: ["name", "address.city"] });

  assert.equal(r.length, 2, "ambos os docs devem retornar");
  assert.deepEqual(r[0], { name: "Alice", address: { city: "Londres" } }, "include top-level + pontilhado");
  assert.deepEqual(r[1], { name: "Bob", address: { city: "NYC" } }, "include aplicado a cada elemento");

  console.log("   ✅ query include (top-level + pontilhado) OK");
}

// ---------------------------------------------------------------------------
// 02: query com exclude (e exclude vence include)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Projeção 02 — query exclude");

  ctx.store.set("/people/alice", { name: "Alice", age: 25, password: "x" });
  ctx.store.set("/people/bob", { name: "Bob", age: 35, password: "y" });

  const semSenha = ctx.store.query("/people/*", { exclude: ["password"] });
  assert.deepEqual(
    semSenha,
    [
      { name: "Alice", age: 25 },
      { name: "Bob", age: 35 },
    ],
    "exclude remove a chave",
  );

  // exclude vence include
  const comExcludeVencedor = ctx.store.query("/people/*", { include: ["name", "age", "password"], exclude: ["password"] });
  assert.deepEqual(
    comExcludeVencedor,
    [
      { name: "Alice", age: 25 },
      { name: "Bob", age: 35 },
    ],
    "exclude vence include",
  );

  // exclude pontilhado mantém o ancestral como container de passagem
  ctx.store.set("/people/carol", { name: "Carol", address: { city: "SP", zip: "00000" } });
  const semZip = ctx.store.query("/people/*", { exclude: ["address.zip"] });
  const carol = semZip.find((p: any) => p.name === "Carol") as any;
  assert.deepEqual(carol.address, { city: "SP" }, "exclude pontilhado poda só o ramo");

  console.log("   ✅ query exclude + precedence OK");
}

// ---------------------------------------------------------------------------
// 03: query include com arrays de objetos
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Projeção 03 — query include em array");

  ctx.store.set("/teams/1", {
    name: "A",
    members: [
      { id: 1, role: "lead" },
      { id: 2, role: "dev" },
    ],
  });

  const r = ctx.store.query("/teams/*", { include: ["name", "members.id"] });
  assert.equal((r[0] as any).name, "A");
  // members: array literal → objeto com chaves UUID; projeção members.id
  // aplicada a cada elemento (objetos) preserva apenas os ids.
  const members = (r[0] as any).members as Record<string, { id: number }>;
  assert.ok(members && !Array.isArray(members), "members deve ser objeto");
  assert.equal(Object.keys(members).length, 2);
  assert.ok(
    Object.keys(members).every((k) => /^[0-9a-f]{32}$/.test(k)),
    "chaves UUID",
  );
  assert.deepEqual(
    Object.values(members)
      .map((m) => m.id)
      .sort(),
    [1, 2],
    "projeção members.id preserva os ids",
  );

  console.log("   ✅ query include em array OK");
}

// ---------------------------------------------------------------------------
// 04: get com exclude — coerência de cache (hit e miss)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Projeção 04 — get exclude com cache coerente");

  const doc = { name: "Alan", age: 41, password: "secret", address: { city: "Londres", zip: "12345" } };
  ctx.store.set("/users/100", doc);

  // 1ª chamada: cache MISS → extrai completo + projeta
  const miss = ctx.store.get("/users/100", { exclude: ["password"] });
  assert.deepEqual(miss, { name: "Alan", age: 41, address: { city: "Londres", zip: "12345" } }, "get exclude (miss)");

  // 2ª chamada: cache HIT → projeta a partir do JSON completo em cache
  const hit = ctx.store.get("/users/100", { exclude: ["password"] });
  assert.deepEqual(hit, miss, "get exclude (hit) == miss");

  // get SEM options depois de um get projetado → documento COMPLETO (cache não corrompido)
  const completo = ctx.store.get("/users/100") as Record<string, unknown>;
  assert.equal(completo.password, "secret", "cache continua com o documento completo");

  // exclude pontilhado (miss→hit)
  const semZip = ctx.store.get("/users/100", { exclude: ["address.zip"] }) as any;
  assert.deepEqual(semZip.address, { city: "Londres" }, "get exclude pontilhado");
  const semZip2 = ctx.store.get("/users/100", { exclude: ["address.zip"] }) as any;
  assert.deepEqual(semZip2, semZip, "get exclude pontilhado idempotente");

  console.log("   ✅ get exclude + coerência de cache OK");
}

// ---------------------------------------------------------------------------
// 05: get com include (miss + hit)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Projeção 05 — get include");

  ctx.store.set("/users/200", { id: 200, name: "Ada", address: { city: "Londres", zip: "54321" }, tags: ["a", "b"] });

  const slim = ctx.store.get("/users/200", { include: ["id", "address.city"] });
  assert.deepEqual(slim, { id: 200, address: { city: "Londres" } }, "get include (miss)");

  const slim2 = ctx.store.get("/users/200", { include: ["id", "address.city"] });
  assert.deepEqual(slim2, slim, "get include (hit)");

  console.log("   ✅ get include (miss+hit) OK");
}

// ---------------------------------------------------------------------------
// 06: get de path primitivo com options — projeção não se aplica (no-op)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Projeção 06 — get primitivo com options");

  ctx.store.set("/users/300", { name: "Grace" });
  const name = ctx.store.get("/users/300/name", { include: ["name"] });
  assert.equal(name, "Grace", "path primitivo retorna o valor mesmo com options");

  console.log("   ✅ get primitivo com options OK");
}

// ---------------------------------------------------------------------------
// 07: searchByPath com include/exclude globs
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Projeção 07 — searchByPath globs");

  ctx.store.set("/users/100", { name: "A", password: "x" });
  ctx.store.set("/users/200", { name: "B", password: "y" });

  const todos = ctx.store.searchByPath("/users/");
  assert.ok(todos.length > 5, "range scan devolve containers + primitivos");

  // include: só paths com 1 segmento sob /users (containers dos docs + root)
  const soContainers = ctx.store.searchByPath("/users/", { include: ["/users/*"] });
  assert.ok(
    soContainers.every((n) => !n.path.endsWith("/name") && !n.path.endsWith("/password")),
    "include por glob de path",
  );

  // exclude: remove os nós de senha em qualquer profundidade
  const semSenha = ctx.store.searchByPath("/users/", { exclude: ["/users/**"] });
  assert.ok(
    semSenha.every((n) => !n.path.includes("password")),
    "exclude ** remove descendentes profundos",
  );

  // exclude específico de 1 nível
  const semNome = ctx.store.searchByPath("/users/", { exclude: ["/users/*/name"] });
  assert.ok(
    semNome.every((n) => !n.path.endsWith("/name")),
    "exclude por segmento único",
  );

  console.log("   ✅ searchByPath globs OK");
}

// ---------------------------------------------------------------------------
// 08: import JSON com exclude
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Projeção 08 — import JSON com exclude");

  const doc = { id: 1, name: "X", password: "abc", meta: { internal: true, public: false } };
  await ctx.store.import("/imp-json", Buffer.from(JSON.stringify(doc)), "json", { exclude: ["password", "meta.internal"] });

  const stored = ctx.store.get("/imp-json") as any;
  assert.equal(stored.password, undefined, "password não foi gravado");
  assert.deepEqual(stored.meta, { public: false }, "exclude pontilhado no import");
  assert.equal(stored.name, "X");

  console.log("   ✅ import JSON com exclude OK");
}

// ---------------------------------------------------------------------------
// 09: import CSV com exclude de path
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Projeção 09 — import CSV com exclude de path");

  const csvData = `path,type,text_value
"/imp-csv/",1,"{}"
"/imp-csv/alice/",1,"{}"
"/imp-csv/alice/name",5,"""Alice"""
"/imp-csv/alice/password",5,"""x"""
"/imp-csv/bob/",1,"{}"
"/imp-csv/bob/name",5,"""Bob"""`;

  // exclude de qualquer nó chamado password (1 nível abaixo de /imp-csv)
  await ctx.store.import("/imp-csv", Buffer.from(csvData), "csv", { exclude: ["/imp-csv/*/password"] });

  const alice = ctx.store.get("/imp-csv/alice") as any;
  assert.equal(alice.name, "Alice");
  assert.equal(alice.password, undefined, "nó de senha foi pulado na importação CSV");

  const bob = ctx.store.get("/imp-csv/bob") as any;
  assert.equal(bob.name, "Bob");

  console.log("   ✅ import CSV com exclude de path OK");
}

// ---------------------------------------------------------------------------
// 10: multi-tabela — projeção funciona com tableName
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Projeção 10 — multi-tabela com projeção");

  const users = new LibsqlHierarchicalStore(ctx.db, { tableName: "users" });
  users.set("/u/1", { name: "Ann", age: 30, secret: "s" });

  const r = users.query("/u/*", { include: ["name"] });
  assert.deepEqual(r, [{ name: "Ann" }], "query include em tabela customizada");

  const g = users.get("/u/1", { exclude: ["secret"] });
  assert.deepEqual(g, { name: "Ann", age: 30 }, "get exclude em tabela customizada");

  console.log("   ✅ multi-tabela com projeção OK");
}

console.log("\n✅ Suíte de projeção concluída");
