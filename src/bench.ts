/**
 * =============================================================================
 * Bench — f-base sobre libSQL
 * =============================================================================
 *
 * Mede ops/s e latência de set/get/update/delete com documentos de
 * tamanhos variados. Usa TEMPO-ALVO por medição (não iterações fixas)
 * para que docs grandes não explodam a duração do bench.
 *
 * Modo inline (--inline): demonstra o ganho da Fase 4 (max_inline_size),
 * que reduz o nº de linhas ~5× em documentos com muitos primitivos.
 *
 * Uso:
 *   npx tsx src/bench.ts               # benchmark completo (modo padrão)
 *   npx tsx src/bench.ts --inline      # compara padrão vs inline (Fase 4)
 *   npx tsx src/bench.ts 2000          # tempo-alvo customizado por medição (ms)
 * =============================================================================
 */
import { performance } from "node:perf_hooks";
import { LibsqlEngine } from "./libsql-engine.js";
import { LibsqlHierarchicalStore } from "./libsql-store.js";

const ARGS = process.argv.slice(2);
const INLINE = ARGS.includes("--inline");
const TARGET_MS = Number(ARGS.find((a) => !a.startsWith("--")) ?? 1000);

/**
 * Tamanhos de teste. Cada campo do makeDoc gera ~10 nós reais
 * (container + value + label + nested/x + nested/y + items + items/0..2).
 */
const SIZES = [50, 500, 5000];

/** Gera um documento com `size` campos (≈ size × 10 + 5 nós reais). */
function makeDoc(size: number): Record<string, unknown> {
  const doc: Record<string, unknown> = {
    id: 1,
    name: "Bench Doc",
    active: true,
    score: 9.99,
    tags: ["a", "b", "c", "d", "e"],
  };
  for (let i = 0; i < size; i++) {
    doc[`field_${i}`] = {
      value: i,
      label: `label-${i}`,
      nested: { x: i, y: i * 2 },
      items: [i, i + 1, i + 2],
    };
  }
  return doc;
}

/** Conta os nós reais (linhas da tabela) sob um prefixo. */
function countNodes(store: LibsqlHierarchicalStore, prefix: string): number {
  return store.searchByPath(prefix).length;
}

/**
 * Gera uma variante do doc com 1 campo diferente.
 * Derrota o dedup do set (doc_hashes) no import JSON — conteúdo idêntico
 * pularia a escrita (escrita idempotente) e o bench mediria dedup-skip.
 */
function mutateDoc(doc: Record<string, unknown>, v: number): Record<string, unknown> {
  const f0 = doc.field_0 as Record<string, unknown>;
  return { ...doc, field_0: { ...f0, value: (f0.value as number) + v + 1 } };
}

/** Mede ops/s por tempo-alvo (targetMs): quantas execuções em N ms. */
function measure(label: string, fn: () => void, targetMs: number): void {
  // Warmup curto
  const warmupEnd = performance.now() + 100;
  while (performance.now() < warmupEnd) fn();

  const latencies: number[] = [];
  const start = performance.now();
  let count = 0;
  while (performance.now() - start < targetMs) {
    const t0 = performance.now();
    fn();
    latencies.push(performance.now() - t0);
    count++;
  }
  const elapsed = performance.now() - start;
  const ops = (count / elapsed) * 1000;
  latencies.sort((a, b) => a - b);
  const p50 = latencies[Math.floor(latencies.length * 0.5)] ?? 0;
  const p99 = latencies[Math.floor(latencies.length * 0.99)] ?? 0;
  console.log(`  ${label.padEnd(30)} ${ops.toFixed(0).padStart(9)} ops/s   p50=${p50.toFixed(3).padStart(7)}ms   p99=${p99.toFixed(3).padStart(7)}ms`);
}

/** Mede ops/s por tempo-alvo para funções ASYNC (import retorna Promise). */
async function measureAsync(label: string, fn: () => Promise<void>, targetMs: number): Promise<void> {
  const warmupEnd = performance.now() + 100;
  while (performance.now() < warmupEnd) await fn();

  const latencies: number[] = [];
  const start = performance.now();
  let count = 0;
  while (performance.now() - start < targetMs) {
    const t0 = performance.now();
    await fn();
    latencies.push(performance.now() - t0);
    count++;
  }
  const elapsed = performance.now() - start;
  const ops = (count / elapsed) * 1000;
  latencies.sort((a, b) => a - b);
  const p50 = latencies[Math.floor(latencies.length * 0.5)] ?? 0;
  const p99 = latencies[Math.floor(latencies.length * 0.99)] ?? 0;
  console.log(`  ${label.padEnd(30)} ${ops.toFixed(0).padStart(9)} ops/s   p50=${p50.toFixed(3).padStart(7)}ms   p99=${p99.toFixed(3).padStart(7)}ms`);
}

async function runSize(store: LibsqlHierarchicalStore, size: number, doc: Record<string, unknown>, label: string): Promise<void> {
  const key = `/bench/${label}-${size}`;
  console.log(`\n── ${label}: ~${size} campos (≈${size * 10 + 5} nós) ──`);

  const inline = label === "inline" ? 128 : undefined;
  const before = performance.now();
  store.set(key, doc, inline);
  const setMs = performance.now() - before;
  const real = countNodes(store, key + "/");
  console.log(`  (nós reais: ${real} — set inicial: ${setMs.toFixed(1)}ms)`);

  // ── Leitura/serialização (sync) ──
  measure(`get`, () => store.get(key), TARGET_MS);
  measure(`export json`, () => store.export(key, "json"), TARGET_MS);
  measure(`export csv`, () => store.export(key, "csv"), TARGET_MS);

  // ── Escritas (sync) ──
  // set (rewrite): doc MUTADO a cada iteração (contador MONOTÔNICO derrota o
  // dedup SEMPRE — mede o custo REAL de reescrever o doc inteiro: delete +
  // re-flatten dos ~50k nós). Um contador cíclico (% n) repetiria a variante
  // e o dedup-skip contaminaria a média.
  let setCounter = 0;
  measure(`set (rewrite)`, () => store.set(key, mutateDoc(doc, setCounter++), inline), TARGET_MS);
  // set (dedup): MESMO doc → escrita idempotente (hash igual, sem write) —
  // mede o custo do dedup-skip (stringify + SHA-256, sem yyjson_read no C).
  measure(`set (dedup)`, () => store.set(key, doc, inline), TARGET_MS);
  store.set(key, doc, inline); // garante existência
  measure(`update patch`, () => store.update(key, { score: 8.5, active: false }), TARGET_MS);
  measure(`query`, () => store.query(`${key}/*`, { filters: [{ key: "value", op: ">", compare: 1 }], take: 10 }), TARGET_MS);

  // ── Import (async) ──
  // JSON: payloads pré-gerados FORA do loop (a serialização é custo do
  // produtor, não do motor) e variados por iteração (derrota o dedup).
  const VARIANTS = 8;
  const jsonVariants = Array.from({ length: VARIANTS }, (_, v) => Buffer.from(JSON.stringify(mutateDoc(doc, v)), "utf-8"));
  // CSV: 1 export fora do loop → round-trip realista (import_csv no C
  // NÃO consulta doc_hashes — sempre reescreve, então o mesmo CSV é honesto).
  const csvText = store.export(key, "csv").toString("utf-8");

  let tick = 0;
  await measureAsync(
    `import json`,
    async () => {
      await store.import(key, jsonVariants[tick++ % VARIANTS]!, "json");
    },
    TARGET_MS,
  );
  await measureAsync(
    `import csv`,
    async () => {
      await store.import(key, Buffer.from(csvText, "utf-8"), "csv");
    },
    TARGET_MS,
  );

  measure(`delete`, () => store.set(key, null), TARGET_MS);
}

async function main(): Promise<void> {
  const mode = INLINE ? "padrão vs inline (Fase 4)" : "padrão";
  console.log(`\n📊 Bench libSQL — tempo-alvo ${TARGET_MS}ms/medição — modo: ${mode}\n`);

  const engine = new LibsqlEngine();
  engine.start({ memory: true, silent: true });
  // cacheObjects: o bench mede o cenário de leitura pesada sem mutação do
  // resultado — o get cache-hit devolve a referência (zero JSON.parse).
  const store = new LibsqlHierarchicalStore(engine.db, { cacheObjects: true });

  const docs = new Map<number, Record<string, unknown>>();
  for (const size of SIZES) docs.set(size, makeDoc(size));

  if (INLINE) {
    for (const size of SIZES) await runSize(store, size, docs.get(size)!, "padrão");
    console.log("\n── comparando com INLINE (max_inline_size=128) ──");
    for (const size of SIZES) await runSize(store, size, docs.get(size)!, "inline");
  } else {
    for (const size of SIZES) await runSize(store, size, docs.get(size)!, "padrão");
  }

  engine.stop();
  console.log("\n✅ Bench concluído");
}

void main();
