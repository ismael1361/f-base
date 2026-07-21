import Database from "better-sqlite3";
import path from "path";
import fs from "fs";

const DB_PATH = "db/debug2.db";
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

// Teste 1: set_json com dados simples
const rev1 = db.prepare("SELECT set_json(?, ?) as r").get("/test-a", JSON.stringify({ name: "A", val: 1 })) as any;
console.log("set_json revision:", rev1?.r);
const nodes1 = db.prepare("SELECT path, type, text_value FROM nodes WHERE path >= '/test-a/' AND path < '/test-a0' ORDER BY path").all() as any[];
console.log("Nodes after set_json:", nodes1.length);
nodes1.forEach((n: any) => console.log("  ", JSON.stringify(n.path), "type=", n.type, "tv=", JSON.stringify(n.text_value)));

const j1 = db.prepare("SELECT extract_json('/test-a') as j").get() as any;
console.log("extract_json /test-a:", j1?.j);
console.log("");

// Teste 2: import_csv com CSV equivalente
const csvData = `path,type,text_value
/test-b/,1,{}
/test-b/name,5,""B""
/test-b/val,3,99`;

const rev2 = db.prepare("SELECT import_csv(?, ?) as r").get("/test-b", csvData) as any;
console.log("import_csv revision:", rev2?.r);
const nodes2 = db.prepare("SELECT path, type, text_value FROM nodes WHERE path >= '/test-b/' AND path < '/test-b0' ORDER BY path").all() as any[];
console.log("Nodes after import_csv:", nodes2.length);
nodes2.forEach((n: any) => console.log("  ", JSON.stringify(n.path), "type=", n.type, "tv=", JSON.stringify(n.text_value)));

const j2 = db.prepare("SELECT extract_json('/test-b') as j").get() as any;
console.log("extract_json /test-b:", j2?.j);

db.close();
