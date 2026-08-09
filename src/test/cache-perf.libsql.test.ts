import { strict as assert } from "node:assert";
import { setupLibsql, type LibsqlTestContext } from "./libsql-test-helper";

// =============================================================================
// Test Suite: cache de leitura + dedup de escrita + batch INSERT
// (Fase de performance — set/get em documentos grandes)
//
// Cobre:
//  - Dedup de `set` (escrita idempotente): mesmo conteúdo → mesma revision
//  - Invalidação do dedup por update/delete
//  - Mini-cache de leitura do store (get O(1) após primeira leitura)
//  - Invalidação de ancestrais no cache (JSON do pai muda com o filho)
//  - Correção do batch INSERT multi-linha (contagem exata de nós)
//  - Correção do unflattening em documentos grandes (regressão O(n²))
// =============================================================================

let ctx: LibsqlTestContext;
ctx = setupLibsql();

function beforeTest(): void {
  ctx.reset();
}

/** Gera doc com `fields` campos (~10 nós reais por campo sem inline). */
function makeBigDoc(fields: number): Record<string, unknown> {
  const doc: Record<string, unknown> = { id: 1, name: "Big Doc" };
  for (let i = 0; i < fields; i++) {
    doc[`field_${i}`] = {
      value: i,
      label: `label-${i}`,
      nested: { x: i, y: i * 2 },
      items: [i, i + 1, i + 2],
    };
  }
  return doc;
}

/** Conta nós sob um prefixo (ex.: "/perf/doc/"). */
function countNodes(prefix: string): number {
  const row = ctx.db.prepare("SELECT COUNT(*) AS c FROM main_nodes WHERE path >= ? AND path < ?").get(prefix, prefix.slice(0, -1) + String.fromCharCode(prefix.charCodeAt(prefix.length - 1) + 1)) as {
    c: number;
  };
  return row.c;
}

// ---------------------------------------------------------------------------
// DEDUP: set idempotente (mesmo conteúdo → mesma revision, sem reescrever)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Dedup — set com mesmo conteúdo retorna a mesma revision");

  const doc = { name: "Alice", age: 30, tags: ["a", "b"] };
  const rev1 = ctx.store.set("/users/100", doc);
  const countAfterFirst = countNodes("/users/100/");

  const rev2 = ctx.store.set("/users/100", doc);
  const countAfterSecond = countNodes("/users/100/");

  assert.ok(rev1.length > 0, "revision 1 não pode ser vazia");
  assert.equal(rev2, rev1, "conteúdo idêntico deve devolver a MESMA revision");
  assert.equal(countAfterSecond, countAfterFirst, "set idêntico não pode reescrever nós");
  assert.deepEqual(ctx.store.get("/users/100"), doc, "conteúdo preservado");

  console.log("   ✅ Dedup passou (rev:", rev1.slice(0, 8) + "…)");
}

// ---------------------------------------------------------------------------
// DEDUP: conteúdo diferente → nova revision
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Dedup — conteúdo diferente gera nova revision");

  const rev1 = ctx.store.set("/d", { a: 1 });
  const rev2 = ctx.store.set("/d", { a: 2 });

  assert.notEqual(rev2, rev1, "conteúdo diferente deve gerar nova revision");
  assert.deepEqual(ctx.store.get("/d"), { a: 2 });

  console.log("   ✅ Dedup conteúdo diferente passou");
}

// ---------------------------------------------------------------------------
// DEDUP: maxInlineSize participa do hash (layout muda → nova revision)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Dedup — maxInlineSize diferente invalida o dedup");

  const doc = { value: 1, label: "x" };
  const revPadrao = ctx.store.set("/d", doc); // max_inline = 0
  const revInline = ctx.store.set("/d", doc, 128); // layout diferente
  const revInline2 = ctx.store.set("/d", doc, 128); // idêntico ao anterior

  assert.notEqual(revInline, revPadrao, "layout diferente (inline) deve gerar nova revision");
  assert.equal(revInline2, revInline, "mesmo layout idêntico → mesma revision");
  assert.deepEqual(ctx.store.get("/d"), doc);

  console.log("   ✅ Dedup maxInlineSize passou");
}

// ---------------------------------------------------------------------------
// DEDUP: update invalida (set com conteúdo antigo → nova revision)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Dedup — update invalida o hash (set antigo reescreve)");

  const original = { name: "Alice", age: 30 };
  const rev1 = ctx.store.set("/users/100", original);
  const revUpdate = ctx.store.update("/users/100", { age: 31 }); // muda o doc
  assert.notEqual(revUpdate, rev1, "update deve gerar nova revision");

  // Após update, o hash foi invalidado → set com o conteúdo ANTIGO reescreve
  const revBack = ctx.store.set("/users/100", original);
  assert.notEqual(revBack, revUpdate, "set com conteúdo antigo deve reescrever");
  assert.deepEqual(ctx.store.get("/users/100"), original, "doc voltou ao conteúdo original");

  // Agora o hash registrado é o do original → próximo set idêntico é dedup
  const revDedup = ctx.store.set("/users/100", original);
  assert.equal(revDedup, revBack, "conteúdo idêntico após re-write → dedup");

  console.log("   ✅ Dedup invalidação por update passou");
}

// ---------------------------------------------------------------------------
// DEDUP: delete (set null) limpa o hash
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Dedup — delete (set null) limpa o hash");

  const doc = { a: 1 };
  ctx.store.set("/d", doc);
  ctx.store.set("/d", null); // remove

  assert.equal(ctx.store.get("/d"), null, "doc removido");

  const rev = ctx.store.set("/d", doc); // recria
  assert.ok(rev.length > 0);
  assert.deepEqual(ctx.store.get("/d"), doc);

  // Após o delete, a recriação é um write real (revision_nr volta a 1)
  const root = ctx.db.prepare("SELECT revision_nr AS nr FROM main_nodes WHERE path = '/d/'").get() as { nr: number } | undefined;
  assert.equal(root?.nr, 1, "doc recriado do zero (revision_nr=1)");

  console.log("   ✅ Dedup delete passou");
}

// ---------------------------------------------------------------------------
// DEDUP: import seguido de set com mesmo conteúdo (hash registrado pelo import)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Dedup — import registra hash (set idêntico é pulado)");

  const doc = { name: "Imported", list: [1, 2, 3] };
  await ctx.store.import("/imp", Buffer.from(JSON.stringify(doc)), "json");

  const revImport = ctx.db.prepare("SELECT revision FROM main_nodes WHERE path = '/imp/'").get() as { revision: string } | undefined;
  assert.ok(revImport, "import deve ter escrito o doc");

  const revSet = ctx.store.set("/imp", doc);
  assert.equal(revSet, revImport.revision, "set com mesmo conteúdo após import → dedup");

  console.log("   ✅ Dedup import passou");
}

// ---------------------------------------------------------------------------
// CACHE DE LEITURA: get repetido é servido pelo cache (e invalida ao mudar)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Cache de leitura — get repetido + invalidação por escrita");

  ctx.store.set("/people/alice", { name: "Alice", age: 25 });
  ctx.store.set("/people/bob", { name: "Bob", age: 35 });

  const a1 = ctx.store.get("/people/alice");
  const a2 = ctx.store.get("/people/alice");
  assert.deepEqual(a1, { name: "Alice", age: 25 });
  assert.deepEqual(a2, { name: "Alice", age: 25 });

  // Escrita muda o doc → cache invalidado → get retorna o novo valor
  ctx.store.set("/people/alice", { name: "Alice", age: 26 });
  assert.deepEqual(ctx.store.get("/people/alice"), { name: "Alice", age: 26 });

  // Update patch → invalida → get reflete
  ctx.store.update("/people/alice", { age: 27 });
  assert.deepEqual(ctx.store.get("/people/alice"), { name: "Alice", age: 27 });

  console.log("   ✅ Cache de leitura passou");
}

// ---------------------------------------------------------------------------
// CACHE DE LEITURA: invalidação de ancestrais (get de container pai)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Cache de leitura — ancestrais invalidados");

  ctx.store.set("/users/100", { name: "A", age: 30 });
  ctx.store.set("/users/200", { name: "B", age: 40 });

  const people1 = ctx.store.get("/users") as Record<string, unknown>;
  assert.ok(people1["100"] && people1["200"], "get /users traz os dois filhos");

  // Mudar um filho deve refletir no JSON do pai (cache do pai invalidado)
  ctx.store.set("/users/100", { name: "A", age: 31 });
  const people2 = ctx.store.get("/users") as Record<string, unknown>;
  assert.equal((people2["100"] as any).age, 31, "get /users reflete mudança no filho");

  // Deletar um filho também
  ctx.store.set("/users/200", null);
  const people3 = ctx.store.get("/users") as Record<string, unknown>;
  assert.equal(people3["200"], undefined, "filho removido some do pai");

  console.log("   ✅ Invalidação de ancestrais passou");
}

// ---------------------------------------------------------------------------
// CACHE DE LEITURA: mutação do objeto retornado não corrompe o cache
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Cache de leitura — mutação do retorno não corrompe");

  ctx.store.set("/doc", { list: [1, 2, 3], nested: { k: "v" } });

  const first = ctx.store.get("/doc") as any;
  first.list.push(999); // mutação do caller
  first.nested.k = "CORROMPIDO";

  const second = ctx.store.get("/doc");
  assert.deepEqual(second, { list: [1, 2, 3], nested: { k: "v" } }, "cada get devolve objeto novo");

  console.log("   ✅ Mutação-safe passou");
}

// ---------------------------------------------------------------------------
// CACHE DE LEITURA: dedup preserva o cache (set idêntico não invalida)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Cache de leitura — set idêntico (dedup) preserva o cache");

  const doc = { name: "X", deep: { a: [1, 2, 3], b: "str" } };
  ctx.store.set("/d", doc);
  ctx.store.get("/d"); // popula o cache

  ctx.store.set("/d", doc); // dedup → mesma revision → cache intacto
  assert.deepEqual(ctx.store.get("/d"), doc, "get após dedup-skip retorna o conteúdo");

  console.log("   ✅ Dedup preserva cache passou");
}

// ---------------------------------------------------------------------------
// BATCH: contagem exata de nós (default e inline) + corretude
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Batch INSERT — contagem exata de nós (default e inline)");

  const N = 200;
  // Sem inline: root + id + name + por campo → field_N/, value, label,
  // nested/, x, y, items/, 0, 1, 2 = 10 nós → total = 3 + 10*N
  ctx.store.set("/perf/doc", makeBigDoc(N));
  const countDefault = countNodes("/perf/doc/");
  assert.equal(countDefault, 3 + 10 * N, `default: esperado ${3 + 10 * N}, obtido ${countDefault}`);

  // Com inline=128: root (id/name inline) + por campo → field_N/, nested/, items/ = 3 nós
  ctx.store.set("/perf/inline", makeBigDoc(N), 128);
  const countInline = countNodes("/perf/inline/");
  assert.equal(countInline, 1 + 3 * N, `inline: esperado ${1 + 3 * N}, obtido ${countInline}`);

  // Corretude do conteúdo em ambos os layouts
  const d = ctx.store.get("/perf/doc") as any;
  const i = ctx.store.get("/perf/inline") as any;
  assert.equal(d.field_199.value, 199);
  assert.equal(d.field_199.nested.y, 398);
  assert.deepEqual(d.field_199.items, [199, 200, 201]);
  assert.equal(i.field_199.value, 199, "inline preserva value");
  assert.equal(i.field_199.nested.x, 199, "inline preserva nested.x");
  assert.deepEqual(i.field_199.items, [199, 200, 201], "inline preserva items");

  console.log("   ✅ Batch contagem passou (default:", countDefault, "| inline:", countInline + ")");
}

// ---------------------------------------------------------------------------
// BATCH + fast path: update de folha após set grande (flush antes do insert)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Batch — update de folha após set grande (ordem do flush)");

  ctx.store.set("/perf/doc", makeBigDoc(100));
  ctx.store.update("/perf/doc", { score: 9.5 }); // fast path keep_created=1

  const doc = ctx.store.get("/perf/doc") as any;
  assert.equal(doc.score, 9.5, "chave nova criada pelo fast path");
  assert.equal(doc.field_99.value, 99, "nós do batch preservados");
  assert.equal(doc.name, "Big Doc");

  // Remover chave via null (delete no fast path)
  ctx.store.update("/perf/doc", { score: null });
  const doc2 = ctx.store.get("/perf/doc") as any;
  assert.equal(doc2.score, undefined, "score removido");
  assert.equal(doc2.field_99.value, 99);

  console.log("   ✅ Batch + fast path passou");
}

// ---------------------------------------------------------------------------
// INLINE: update_text após batch (containers com inline children)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Inline — containers com inline children (update_text pós-batch)");

  ctx.store.set("/perf/inline", makeBigDoc(50), 128);
  // O root tem inline (id/name) → update_text é chamado com linhas do batch pendentes
  const doc = ctx.store.get("/perf/inline") as any;
  assert.equal(doc.id, 1, "id inline preservado");
  assert.equal(doc.name, "Big Doc", "name inline preservado");
  assert.equal(doc.field_49.label, "label-49", "label inline preservado");

  // Update patch num doc com inline children (fast path + inline coexistindo)
  ctx.store.update("/perf/inline", { active: true });
  const doc2 = ctx.store.get("/perf/inline") as any;
  assert.equal(doc2.active, true);
  assert.equal(doc2.field_49.label, "label-49");

  console.log("   ✅ Inline pós-batch passou");
}

// ---------------------------------------------------------------------------
// DOC GRANDE: get correto + regressão O(n²) (sanity de tempo, sem flaky)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Doc grande — get 3000 campos (≈30k nós) com corretude");

  ctx.store.set("/perf/big", makeBigDoc(3000));

  const t0 = performance.now();
  const big = ctx.store.get("/perf/big") as any;
  const elapsed = performance.now() - t0;

  assert.equal(big.field_2999.value, 2999, "último campo correto");
  assert.equal(big.field_0.nested.y, 0, "primeiro campo correto");
  assert.deepEqual(big.field_1500.items, [1500, 1501, 1502], "items correto");

  // Guarda frouxa contra regressão O(n²) (o algoritmo antigo explodia aqui).
  // Em máquina de dev, o get (frio) fica em dezenas de ms — 8000ms só pega
  // blow-ups patológicos.
  assert.ok(elapsed < 8000, `get de 30k nós muito lento (${elapsed.toFixed(0)}ms)`);

  // Cache de leitura: segundo get é O(1)
  const t1 = performance.now();
  ctx.store.get("/perf/big");
  const cachedMs = performance.now() - t1;
  assert.ok(cachedMs < 200, `get em cache deveria ser ~0ms (${cachedMs.toFixed(1)}ms)`);

  console.log(`   ✅ Doc grande passou (frio: ${elapsed.toFixed(0)}ms | cache: ${cachedMs.toFixed(1)}ms)`);
}

// ---------------------------------------------------------------------------
// DEDUP + cache dentro de transaction()
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Transaction — dedup e cache dentro de transação");

  ctx.store.transaction((tx) => {
    tx.set("/a", { x: 1 });
    const r1 = tx.set("/a", { x: 1 }); // dedup dentro da tx
    assert.ok(r1.length > 0);
    assert.deepEqual(tx.get("/a"), { x: 1 });
  });
  assert.deepEqual(ctx.store.get("/a"), { x: 1 });

  // Rollback: set dentro de tx que lança não persiste
  let threw = false;
  try {
    ctx.store.transaction((tx) => {
      tx.set("/b", { y: 2 });
      throw new Error("rollback");
    });
  } catch {
    threw = true;
  }
  assert.ok(threw, "transação deve lançar");
  assert.equal(ctx.store.get("/b"), null, "nada persistido após rollback");

  console.log("   ✅ Transaction + dedup passou");
}

// =============================================================================
// Resumo
// =============================================================================
console.log("\n========================================");
console.log("✅ Todos os testes de cache/dedup/batch passaram!");
console.log("========================================");

ctx.cleanup();
