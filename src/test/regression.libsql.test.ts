import { strict as assert } from "node:assert";
import { setupLibsql, type LibsqlTestContext } from "./libsql-test-helper";

// =============================================================================
// Test Suite: Regressões de integridade e contrato de coleções
// -----------------------------------------------------------------------------
// Modelo OBJETO-ONLY (Firebase-style): arrays do JSON de entrada viram
// objetos com chaves UUID (push ID) — nunca arrays, nunca índices. Não
// existe TYPE_ARRAY; dados legados type=2 são lidos como objeto (compat).
// Bug A2 — update_json fast-path: delete_subtree SEM o '/' final varria
//          [path, path+1) e apagava chaves irmãs por prefixo de texto
//          (ex.: { user: null } apagava também "username").
// =============================================================================

let ctx: LibsqlTestContext;

function beforeTest(): void {
  ctx.reset();
}

ctx = setupLibsql();
ctx.registerTable("main");

// ---------------------------------------------------------------------------
// Regressão 01: Coleção de 15 elementos vira objeto com chaves UUID
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Regressão 01 — coleção de 15 elementos (chaves UUID)");

  const values = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14];
  ctx.store.set("/arr", { values });
  const doc = ctx.store.get<{ values: Record<string, number> }>("/arr");

  assert.ok(doc, "doc deve existir");
  const vals = doc.values;
  assert.ok(vals && !Array.isArray(vals), "coleção deve ser objeto (modelo objeto-only)");
  const vkeys = Object.keys(vals);
  assert.equal(vkeys.length, 15, "15 elementos");
  assert.ok(
    vkeys.every((k) => /^[0-9a-f]{32}$/.test(k)),
    "chaves UUID (32 hex)",
  );
  assert.deepEqual(
    Object.values(vals).sort((a, b) => a - b),
    values,
    "valores preservados",
  );

  // Storage: filhos com path de chave UUID (nunca índice numérico)
  const rows = ctx.db.prepare("SELECT path FROM main_nodes WHERE path LIKE '/arr/values/%' AND path NOT LIKE '%/'").all() as { path: string }[];
  assert.equal(rows.length, 15, "15 nós dedicados");
  assert.ok(
    rows.every((r) => /^\/arr\/values\/[0-9a-f]{32}$/.test(r.path)),
    "paths com chave UUID",
  );

  console.log("   ✅ Coleção 15 elementos (UUID) OK");
}

// ---------------------------------------------------------------------------
// Regressão 02: Coleção mista inline + dedicada (chaves UUID)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Regressão 02 — coleção mista inline+dedicada (chaves UUID)");

  ctx.store.set("/mix", { tags: ["aa", "string_bem_longa_maior_que_oito", "bb", "cc"] }, 8);
  const doc = ctx.store.get<{ tags: Record<string, string> }>("/mix");

  assert.ok(doc, "doc deve existir");
  const tags = doc.tags;
  assert.ok(tags && !Array.isArray(tags), "tags deve ser objeto");
  assert.equal(Object.keys(tags).length, 4, "4 elementos");
  assert.ok(
    Object.keys(tags).every((k) => /^[0-9a-f]{32}$/.test(k)),
    "chaves UUID",
  );
  assert.deepEqual(Object.values(tags).sort(), ["aa", "bb", "cc", "string_bem_longa_maior_que_oito"], "valores preservados (inline + dedicado)");

  console.log("   ✅ Coleção mista inline+dedicada OK");
}

// ---------------------------------------------------------------------------
// Regressão 03: update com null NÃO deleta chaves irmãs por prefixo
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Regressão 03 — update null preserva chaves irmãs (prefixo)");

  ctx.store.set("/doc", { user: { x: 1 }, username: "alice", name: "n", nameX: 7, nametag: "t" });

  // user → null: "username" compartilha o prefixo "user" e era apagada
  ctx.store.update("/doc", { user: null });
  const after1 = ctx.store.get<Record<string, unknown>>("/doc");
  assert.ok(after1, "doc deve existir");
  assert.equal(after1.username, "alice", "username deve ser preservada");
  assert.equal(after1.name, "n");
  assert.equal(after1.nameX, 7);
  assert.equal(after1.nametag, "t");
  assert.equal(after1.user, undefined, "user deve ter sido removida");

  // name → null: "nameX"/"nametag" compartilham o prefixo "name" e eram apagadas
  ctx.store.update("/doc", { name: null });
  const after2 = ctx.store.get<Record<string, unknown>>("/doc");
  assert.ok(after2, "doc deve existir");
  assert.equal(after2.name, undefined, "name deve ter sido removida");
  assert.equal(after2.nameX, 7, "nameX deve ser preservada");
  assert.equal(after2.nametag, "t", "nametag deve ser preservada");

  console.log("   ✅ update null preserva irmãs por prefixo OK");
}

// ---------------------------------------------------------------------------
// Regressão 04: update substituindo container por primitivo (sem órfãos)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Regressão 04 — update substitui container preservando irmãs");

  ctx.store.set("/c", { tags: { a: 1, b: 2 }, tagsOld: "keep" });
  // "tags" é container; substituir por primitivo deve apagar o subtree e
  // não tocar em "tagsOld" (irmã por prefixo).
  ctx.store.update("/c", { tags: "novo" });
  const doc = ctx.store.get<Record<string, unknown>>("/c");
  assert.ok(doc, "doc deve existir");
  assert.equal(doc.tags, "novo", "tags substituída por primitivo");
  assert.equal(doc.tagsOld, "keep", "tagsOld preservada");

  console.log("   ✅ Substituição de container preserva irmãs OK");
}

// ---------------------------------------------------------------------------
// Regressão 05: export/import CSV de coleção grande (chaves UUID preservadas)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Regressão 05 — export/import CSV de coleção grande");

  const values = Array.from({ length: 15 }, (_, i) => `v${i}`);
  ctx.store.set("/rt", { items: values });

  const csv = ctx.store.export("/rt", "csv").toString("utf-8");
  ctx.store.import("/rt", Buffer.from(csv), "csv");

  const doc = ctx.store.get<{ items: Record<string, string> }>("/rt");
  assert.ok(doc, "doc deve existir após import");
  assert.ok(doc.items && !Array.isArray(doc.items), "items deve ser objeto");
  assert.equal(Object.keys(doc.items).length, 15, "15 elementos");
  assert.ok(
    Object.keys(doc.items).every((k) => /^[0-9a-f]{32}$/.test(k)),
    "chaves UUID",
  );
  assert.deepEqual(
    (Object.values(doc.items) as string[]).sort((a, b) => parseInt(a.slice(1), 10) - parseInt(b.slice(1), 10)),
    values,
    "valores preservados no round-trip CSV",
  );

  console.log("   ✅ CSV round-trip de coleção grande OK");
}

// ---------------------------------------------------------------------------
// Regressão 06: query_json em container de coleção (objeto com chaves UUID)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Regressão 06 — query_json em container de coleção");

  ctx.store.set("/doc", { v: [10, 2, 3, 1, 20] });
  const res = ctx.store.query<number>("/doc/v/*");

  assert.ok(Array.isArray(res), "resultado deve ser array");
  assert.equal(res.length, 5, "5 elementos");
  assert.deepEqual(
    [...res].sort((a, b) => a - b),
    [1, 2, 3, 10, 20],
    "todos os valores presentes (ordem do mapa não é semântica)",
  );

  console.log("   ✅ query em container de coleção OK");
}

// ---------------------------------------------------------------------------
// Regressão 07: leitura de dados LEGADOS (antigo type=2 / índices) — compat
// de migração: um banco gravado por versão antiga continua legível (como
// OBJETO, nunca corrompido para string).
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Regressão 07 — leitura de dados legados (type=2 / índices)");

  const ins = ctx.db.prepare("INSERT INTO main_nodes (path, type, text_value, created, modified, revision_nr, revision) VALUES (?, ?, ?, 1, 1, 1, 'r')");

  // Documento legado: container type=2 (antigo TYPE_ARRAY) + folhas numeradas
  ins.run("/legacy/", 1, "{}");
  ins.run("/legacy/v/", 2, "{}");
  for (const i of [0, 1, 10, 11, 2, 3, 4, 5, 6, 7, 8, 9, 12]) {
    ins.run(`/legacy/v/${i}`, 3, String(i * 10));
  }
  const legacy = ctx.store.get<{ v: Record<string, number> }>("/legacy");
  assert.ok(legacy?.v && !Array.isArray(legacy.v), "container legado type=2 lido como objeto");
  assert.deepEqual(
    legacy.v,
    {
      "0": 0,
      "1": 10,
      "2": 20,
      "3": 30,
      "4": 40,
      "5": 50,
      "6": 60,
      "7": 70,
      "8": 80,
      "9": 90,
      "10": 100,
      "11": 110,
      "12": 120,
    },
    "chaves numéricas legadas preservadas como objeto",
  );

  // Documento com chaves mistas (pad + cru) — compat de leitura
  ins.run("/mixed/", 1, "{}");
  ins.run("/mixed/arr/", 2, "{}");
  ins.run("/mixed/arr/0000000000", 5, '"ZERO"');
  ins.run("/mixed/arr/1", 5, '"UM"');
  ins.run("/mixed/arr/0000000002", 5, '"DOIS"');
  const mixed = ctx.store.get<{ arr: Record<string, string> }>("/mixed");
  assert.deepEqual(mixed?.arr, { "0000000000": "ZERO", "1": "UM", "0000000002": "DOIS" }, "chaves preservadas (pad + cru)");

  console.log("   ✅ Leitura de dados legados OK");
}

// ---------------------------------------------------------------------------
// Regressão 08: CONTRATO de coleções (modelo Firebase objeto-only) — arrays
// do JSON de entrada viram objetos com chaves UUID (push ID); NÃO existe
// TYPE_ARRAY (container é OBJECT); chaves nomeadas são criação explícita.
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Regressão 08 — contrato de coleções (objeto-only, UUID)");

  ctx.store.set("/doc", { tags: ["genius", "computer"] });
  const nodes = ctx.db.prepare("SELECT path, type FROM main_nodes WHERE path LIKE '/doc/tags/%' ORDER BY path").all() as { path: string; type: number }[];

  // Storage: container OBJECT + filhos com chave UUID (nunca índice)
  const leaves = nodes.filter((n) => !n.path.endsWith("/"));
  assert.equal(leaves.length, 2, "2 filhos");
  assert.ok(
    leaves.every((n) => /^\/doc\/tags\/[0-9a-f]{32}$/.test(n.path)),
    "filhos com chave UUID (nunca índice)",
  );
  const container = nodes.find((n) => n.path === "/doc/tags/");
  assert.equal(container?.type, 1, "tags é container OBJECT (não existe TYPE_ARRAY)");

  // Round-trip: array in → OBJETO com chaves UUID (nunca array)
  const doc = ctx.store.get<{ tags: Record<string, string> }>("/doc");
  assert.ok(doc?.tags && !Array.isArray(doc.tags), "round-trip devolve objeto");
  assert.equal(Object.keys(doc.tags).length, 2);
  assert.ok(
    Object.keys(doc.tags).every((k) => /^[0-9a-f]{32}$/.test(k)),
    "chaves UUID",
  );
  assert.deepEqual(Object.values(doc.tags).sort(), ["computer", "genius"], "valores preservados");

  // Objeto explícito preserva chaves nomeadas
  ctx.store.set("/named", { items: { item_01: "a", item_03: 400 } });
  const namedNodes = ctx.db.prepare("SELECT path FROM main_nodes WHERE path LIKE '/named/items/%'").all() as { path: string }[];
  assert.ok(
    namedNodes.some((n) => n.path === "/named/items/item_01"),
    "objeto explícito preserva chave nomeada",
  );
  assert.ok(!namedNodes.some((n) => n.path === "/named/items/0"), "objeto explícito NÃO ganha índice numérico");
  const named = ctx.store.get<{ items: Record<string, unknown> }>("/named");
  assert.deepEqual(named?.items, { item_01: "a", item_03: 400 }, "objeto nomeado lido de volta como objeto");

  console.log("   ✅ Contrato de coleções (UUID) OK");
}
