import { strict as assert } from "node:assert";
import { setupLibsql, type LibsqlTestContext } from "./libsql-test-helper";
import { LibsqlHierarchicalStore } from "../libsql-store";

// =============================================================================
// Test Suite: Cache de objetos (cacheObjects) + dedup de set sem parse
// =============================================================================
// cacheObjects=true: o get cache-hit devolve a MESMA referência do objeto
// (zero JSON.parse). ⚠️ Contrato: o caller NÃO deve mutar o resultado.
// cacheObjects=false (default): cada get devolve objeto novo (isolado).
// Com projeção / nós primitivos, o comportamento NÃO muda (objeto novo).
// =============================================================================

let ctx: LibsqlTestContext;

function beforeTest(): void {
  ctx.reset();
}

ctx = setupLibsql();

// ---------------------------------------------------------------------------
// 01: default (false) — get devolve objeto NOVO (mutação não vaza)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 CacheObjects 01 — default: objeto isolado por get");

  ctx.store.set("/doc", { a: 1, b: { c: 2 } });

  const first = ctx.store.get("/doc") as { a: number; b: { c: number } };
  first.a = 999; // mutação do caller
  const second = ctx.store.get("/doc") as { a: number };
  assert.equal(second.a, 1, "com default, a mutação NÃO vaza para o próximo get");

  console.log("   ✅ default isola o objeto OK");
}

// ---------------------------------------------------------------------------
// 02: cacheObjects=true — hit devolve a MESMA referência (contrato documentado)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 CacheObjects 02 — true: referência compartilhada no hit");

  const store = new LibsqlHierarchicalStore(ctx.db, { cacheObjects: true });
  store.set("/doc", { a: 1, b: { c: 2 } });

  const first = store.get("/doc") as { a: number };
  assert.equal(first.a, 1, "miss parseia e guarda");
  const second = store.get("/doc") as { a: number };
  assert.equal(second, first, "cache-hit devolve a MESMA referência (sem parse)");

  // ⚠️ Contrato: mutar o resultado corrompe o cache (documentado na opção)
  first.a = 999;
  const third = store.get("/doc") as { a: number };
  assert.equal(third.a, 999, "mutação do caller reflete (contrato de risco)");

  console.log("   ✅ referência compartilhada OK");
}

// ---------------------------------------------------------------------------
// 03: cacheObjects=true — invalidação pós-escrita continua correta
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 CacheObjects 03 — invalidação pós-set");

  const store = new LibsqlHierarchicalStore(ctx.db, { cacheObjects: true });
  store.set("/doc", { a: 1 });
  assert.equal((store.get("/doc") as { a: number }).a, 1);

  store.set("/doc", { a: 2 }); // reescrita → revision nova
  const after = store.get("/doc") as { a: number };
  assert.equal(after.a, 2, "get pós-set vê o valor novo (cache invalidado)");

  // update também invalida
  store.update("/doc", { b: 3 });
  const afterUpd = store.get("/doc") as { a: number; b: number };
  assert.equal(afterUpd.b, 3, "get pós-update vê o valor novo");

  console.log("   ✅ invalidação pós-escrita OK");
}

// ---------------------------------------------------------------------------
// 04: cacheObjects=true — projeção continua devolvendo objeto novo
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 CacheObjects 04 — projeção não é afetada");

  const store = new LibsqlHierarchicalStore(ctx.db, { cacheObjects: true });
  store.set("/doc", { a: 1, b: 2 });

  const projected = store.get("/doc", { include: ["a"] }) as { a: number };
  projected.a = 999; // mutar o projetado
  const projected2 = store.get("/doc", { include: ["a"] }) as { a: number };
  assert.equal(projected2.a, 1, "projeção devolve objeto novo (não vaza)");
  const full = store.get("/doc") as { a: number; b: number };
  assert.equal(full.a, 1, "projeção não corrompe o cache completo");

  console.log("   ✅ projeção isolada OK");
}

// ---------------------------------------------------------------------------
// 05: cacheObjects=true — nó primitivo não é afetado (fallback sem cache)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 CacheObjects 05 — primitivo continua isolado");

  const store = new LibsqlHierarchicalStore(ctx.db, { cacheObjects: true });
  store.set("/doc", { name: "Alice" });

  // Primitivos são devolvidos por valor (string/number imutáveis) — não
  // há referência compartilhada nem risco de mutação do cache.
  const name1 = store.get("/doc/name") as string;
  const name2 = store.get("/doc/name") as string;
  assert.equal(name1, "Alice");
  assert.equal(name2, "Alice");

  console.log("   ✅ primitivo por valor OK");
}

// ---------------------------------------------------------------------------
// 06: dedup do set — mesma string não reescreve (revision preservada)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 CacheObjects 06 — dedup de set (mesma string)");

  ctx.store.set("/doc", { a: 1, b: "x".repeat(2000) });
  const rev1 = ctx.store.set("/doc", { a: 1, b: "x".repeat(2000) });
  const rev2 = ctx.store.set("/doc", { a: 1, b: "x".repeat(2000) });
  assert.equal(rev1, rev2, "conteúdo idêntico → mesma revision (dedup, sem write)");

  // Doc mutado → revision nova (rewrite)
  const rev3 = ctx.store.set("/doc", { a: 2, b: "x".repeat(2000) });
  assert.notEqual(rev3, rev2, "conteúdo diferente → rewrite (revision nova)");

  // Round-trip preservado (dedup antes do parse não quebrou nada)
  const out = ctx.store.get("/doc") as { a: number; b: string };
  assert.equal(out.a, 2);
  assert.equal(out.b, "x".repeat(2000));

  console.log("   ✅ dedup + rewrite + round-trip OK");
}

// ---------------------------------------------------------------------------
// 07: JSON inválido continua dando erro (dedup antes do parse não mascara)
// ---------------------------------------------------------------------------
// O store TS sempre serializa (JSON.stringify) antes de chamar o C — um texto
// inválido só atinge o motor via SQL direto (set_json). O dedup ANTES do
// parse não pode mascarar a validação de conteúdo novo.
{
  beforeTest();
  console.log("\n🧪 CacheObjects 07 — JSON inválido → erro via SQL direto");

  // Conteúdo inválido NUNCA foi escrito → não há hash armazenado → o dedup
  // não bate → o parse roda e reporta erro (mesmo com o dedup antes).
  // O controller usa sqlite3_result_error → o driver lança exceção.
  // ('main' = par default do store: main_nodes/main_doc_hashes)
  const stmt = ctx.db.prepare("SELECT set_json('main', ?, ?) AS r");
  assert.throws(() => stmt.get("/doc", "{invalid"), /Invalid JSON/, "set_json com JSON inválido lança erro");
  assert.equal(ctx.store.get("/doc"), null, "nada foi gravado");

  // Um JSON válido escrito primeiro → dedup-skip funciona; um texto mutado
  // (hash diferente) cai no parse e valida.
  ctx.store.set("/doc", { a: 1 });
  const revSame = ctx.store.set("/doc", { a: 1 });
  const revSame2 = ctx.store.set("/doc", { a: 1 });
  assert.equal(revSame, revSame2, "dedup com conteúdo idêntico (mesma revision)");

  console.log("   ✅ validação preservada (dedup não mascara) OK");
}

// ---------------------------------------------------------------------------
// 08: null continua deletando o documento
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 CacheObjects 08 — null deleta");

  ctx.store.set("/doc", { a: 1 });
  ctx.store.set("/doc", null);
  assert.equal(ctx.store.get("/doc"), null, "set null remove o documento");

  console.log("   ✅ null deleta OK");
}

console.log("✅ Suíte de cache de objetos concluída");
