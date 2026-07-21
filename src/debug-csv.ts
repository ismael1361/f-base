import Database from "better-sqlite3";
import path from "path";
import fs from "fs";

const DB_PATH = "db/debug-csv.db";
try {
  fs.unlinkSync(DB_PATH);
} catch {}

const db = new Database(DB_PATH);
db.pragma("journal_mode = WAL");
db.exec(
  `CREATE TABLE IF NOT EXISTS nodes (path TEXT PRIMARY KEY, type INTEGER NOT NULL, text_value TEXT, created INTEGER NOT NULL, modified INTEGER NOT NULL, revision_nr INTEGER NOT NULL, revision TEXT NOT NULL) WITHOUT ROWID;`,
);

const extPath = path.resolve(process.cwd(), "./hierarchical_engine/bin/hierarchical_engine.dll");
(db as any).loadExtension(extPath);

// === Test: import CSV ===
const csvData = [
  "path,type,text_value",
  "/imported/,1,{}",
  "/imported/alice/,1,{}",
  '/imported/alice/name,5,""Alice""',
  "/imported/alice/age,3,30",
  "/imported/bob/,1,{}",
  '/imported/bob/name,5,""Bob""',
  "/imported/bob/age,3,25",
].join("\n");

console.log("=== CSV input (raw) ===");
console.log(csvData);
console.log("=== End CSV ===");

// Try direct import via sqlite
db.prepare("SELECT import_csv(?, ?)").get("/imported", csvData);

// Dump all nodes
const nodes = db.prepare("SELECT path, type, text_value FROM nodes ORDER BY path").all() as any[];
console.log("\n=== Nodes stored in DB ===");
for (const n of nodes) {
  console.log(`  path=${JSON.stringify(n.path)}  type=${n.type}  text_value=${JSON.stringify(n.text_value)}`);
}

// Try extract_json at various levels
for (const p of ["/imported", "/imported/alice", "/", "/imported/alice/name"]) {
  const r = db.prepare("SELECT extract_json(?) as j").get(p) as any;
  console.log(`\nextract_json(${JSON.stringify(p)}) = ${JSON.stringify(r?.j)}`);
}

db.close();
console.log("\nDone.");
