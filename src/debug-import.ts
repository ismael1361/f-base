import Database from "better-sqlite3";
import path from "path";
import fs from "fs";

const DB_PATH = "db/debug.db";
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

// CSV test
const csvData = [
  "path,type,text_value",
  '"/imported/",1,"{}"',
  '"/imported/alice/",1,"{}"',
  '"/imported/alice/name",5,"""Alice"""',
  '"/imported/alice/age",3,"30"',
  '"/imported/bob/",1,"{}"',
  '"/imported/bob/name",5,"""Bob"""',
  '"/imported/bob/age",3,"25"',
].join("\n");

console.log("CSV data:", JSON.stringify(csvData));

db.prepare("SELECT import_csv(?, ?)").get("/imported", csvData);

const nodes = db.prepare("SELECT path, type, text_value FROM nodes ORDER BY path").all() as any[];
console.log("\nNodes after import:");
nodes.forEach((n: any) => console.log("  path=" + JSON.stringify(n.path), "type=" + n.type, "tv=" + JSON.stringify(n.text_value)));

const result = db.prepare("SELECT extract_json(?) as json_data").get("/imported/alice") as any;
console.log("\nextract_json /imported/alice:", JSON.stringify(result?.json_data));

const result2 = db.prepare("SELECT extract_json(?) as json_data").get("/imported") as any;
console.log("extract_json /imported:", JSON.stringify(result2?.json_data));

db.close();
