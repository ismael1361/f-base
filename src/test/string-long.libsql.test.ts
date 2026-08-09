import { strict as assert } from "node:assert";
import { setupLibsql, type LibsqlTestContext } from "./libsql-test-helper";
import { LibsqlHierarchicalStore } from "../libsql-store";

// =============================================================================
// Test Suite: Strings longas (> 1KB) — round-trip SEM truncagem
// =============================================================================
// Regressão: os buffers fixos (text_buf[1024] na escrita, unesc[2048] na
// leitura) TRUNCAVAM strings > ~1KB silenciosamente — o JSON armazenado era
// sintaticamente válido porém cortado no meio do valor, sem nenhum erro.
// Este suite garante round-trip byte a byte para set/update (fast path de
// folhas E merge estrutural)/export/import CSV/import JSON, com escapes e
// caracteres multi-byte.
// =============================================================================

let ctx: LibsqlTestContext;

function beforeTest(): void {
  ctx.reset();
}

ctx = setupLibsql();

/** Gera uma string com `n` chars misturando multi-byte e escapes JSON. */
function makeLong(prefix: string, n: number): string {
  const unit = 'é"\\\n\tx';
  return prefix + unit.repeat(Math.ceil(n / unit.length)).slice(0, n);
}

// ---------------------------------------------------------------------------
// 01: set + get round-trip (boundary de 1KB e valores longos)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Strings longas 01 — set/get round-trip");

  for (const n of [900, 1010, 1020, 1030, 5000, 50000]) {
    const s = makeLong("v", n);
    ctx.store.set(`/doc/${n}`, { bio: s });
    const out = ctx.store.get(`/doc/${n}`) as { bio: string };
    assert.equal(out.bio, s, `round-trip set/get com ${n} chars`);
  }

  console.log("   ✅ set/get íntegro para 900..50000 chars");
}

// ---------------------------------------------------------------------------
// 02: update fast path (folhas) com string longa
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Strings longas 02 — update fast path (folhas)");

  ctx.store.set("/doc", { a: 1, b: "short" });
  const s = makeLong("u", 3000);
  ctx.store.update("/doc", { b: s });
  const out = ctx.store.get("/doc") as { a: number; b: string };
  assert.equal(out.b, s, "update de folha preserva string longa");
  assert.equal(out.a, 1, "chave não mencionada preservada");

  console.log("   ✅ update leaf com 3000 chars OK");
}

// ---------------------------------------------------------------------------
// 03: update estrutural (merge completo) com string longa aninhada
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Strings longas 03 — update estrutural (merge)");

  ctx.store.set("/doc", { nested: { keep: 1 } });
  const s = makeLong("m", 5000);
  ctx.store.update("/doc", { nested: { long: s } });
  const out = ctx.store.get("/doc") as { nested: { keep: number; long: string } };
  assert.equal(out.nested.long, s, "merge estrutural preserva string longa");
  assert.equal(out.nested.keep, 1, "chave preservada no merge");

  console.log("   ✅ update merge com 5000 chars OK");
}

// ---------------------------------------------------------------------------
// 04: export/import CSV round-trip com string longa (com quebras de linha)
// ---------------------------------------------------------------------------
// ⚠️ import() é ASYNC (aceita Readable) — SEM await o get roda antes do
// import concluir e vê o documento antigo/ausente (bug clássico de timing).
{
  beforeTest();
  console.log("\n🧪 Strings longas 04 — export/import CSV round-trip");

  const s = makeLong("c", 2048);
  ctx.store.set("/orig/csv", { data: s });
  const csv = ctx.store.export("/orig/csv", "csv");
  await ctx.store.import("/copy/csv", csv, "csv");
  const out = ctx.store.get("/copy/csv") as { data: string };
  assert.equal(out.data, s, "round-trip CSV preserva string longa");

  console.log("   ✅ export/import CSV com 2048 chars OK");
}

// ---------------------------------------------------------------------------
// 05: import JSON com string longa (via set)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Strings longas 05 — import JSON com string longa");

  const s = makeLong("j", 8000);
  await ctx.store.import("/imp/json", Buffer.from(JSON.stringify({ blob: s })), "json");
  const out = ctx.store.get("/imp/json") as { blob: string };
  assert.equal(out.blob, s, "import JSON preserva string longa");

  console.log("   ✅ import JSON com 8000 chars OK");
}

// ---------------------------------------------------------------------------
// 06: inline mode — string acima do maxInlineSize vira nó dedicado
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Strings longas 06 — inline mode (string > maxInlineSize)");

  const s = makeLong("i", 4096);
  ctx.store.set("/inline/doc", { short: "ok", long: s }, 128);
  const out = ctx.store.get("/inline/doc") as { short: string; long: string };
  assert.equal(out.short, "ok", "string inline curta preservada");
  assert.equal(out.long, s, "string dedicada (acima do inline) íntegra");

  console.log("   ✅ inline com string dedicada 4096 chars OK");
}

// ---------------------------------------------------------------------------
// 07: cache de leitura NÃO fica stale após import CSV
// ---------------------------------------------------------------------------
// Regressão: import() CSV escreve direto (sem passar por set) e NÃO
// invalidava o cache de leitura do destino — um get logo após o import
// devolvia o JSON antigo em cache (revision antiga).
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Strings longas 07 — cache invalidação pós-import CSV");

  // Popula o cache de leitura com o doc ANTES do import
  ctx.store.set("/data/doc", { v: "antes" });
  assert.equal((ctx.store.get("/data/doc") as { v: string }).v, "antes", "leitura inicial");

  // Import CSV por cima (revision muda) — get deve ver o NOVO doc
  ctx.store.set("/orig", { data: "depois" });
  const csv = ctx.store.export("/orig", "csv");
  await ctx.store.import("/data/doc", csv, "csv");
  const out = ctx.store.get("/data/doc") as { data: string };
  assert.equal(out.data, "depois", "get pós-import CSV vê o doc novo (sem cache stale)");

  console.log("   ✅ cache invalidação pós-import CSV OK");
}

// ---------------------------------------------------------------------------
// 08: escapes na fronteira exata do buffer antigo (truncava no meio do escape)
// ---------------------------------------------------------------------------
{
  beforeTest();
  console.log("\n🧪 Strings longas 08 — escapes na fronteira do buffer antigo");

  for (const n of [1016, 1017, 1018, 1019, 1020, 1021, 1022]) {
    // Sequência de aspas: no buffer antigo, cada '"' vira \" e o corte
    // acontecia no meio da sequência de escape
    const s = '"'.repeat(n);
    ctx.store.set(`/esc/${n}`, { v: s });
    const out = ctx.store.get(`/esc/${n}`) as { v: string };
    assert.equal(out.v, s, `round-trip com ${n} aspas`);
  }

  console.log("   ✅ escapes na fronteira 1016..1022 OK");
}

console.log("✅ Suíte de strings longas concluída");
