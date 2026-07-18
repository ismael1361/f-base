import Database from "better-sqlite3";
import path from "path";
import { fileURLToPath } from "url";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

// 1. Inicialização do Banco e Schema
const db = new Database("hierarchical.db", { verbose: console.log });

// Configurações de Performance (O "Sweet Spot" do SQLite)
db.pragma("journal_mode = WAL");
db.pragma("synchronous = NORMAL");
db.pragma("mmap_size = 268435456"); // 256MB
db.pragma("cache_size = -64000"); // 64MB

// Criação das Tabelas (Foco no WITHOUT ROWID para B-Tree otimizada)
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

// 2. Carregamento da Extensão C
// O caminho deve apontar para o arquivo compilado (.so, .dylib ou .dll)
const extPath = path.resolve(process.cwd(), "./hierarchical_engine/bin", `libhierarchical_engine.${process.platform === "win32" ? "dll" : process.platform === "darwin" ? "dylib" : "so"}`);
try {
  db.loadExtension(extPath);
  console.log("✅ Extensão C carregada com sucesso!");
} catch (err) {
  console.error("❌ Falha ao carregar extensão C:", err);
  process.exit(1);
}

// 3. API de Alto Nível (TypeScript)
class HierarchicalStore {
  // A mágica acontece aqui: 1 chamada de função, zero IPC de dados
  public saveDocument(docId: string, jsonData: object): void {
    const jsonStr = JSON.stringify(jsonData);
    // Chama a função C diretamente via SQL
    db.prepare("SELECT ingest_json(?, ?)").get(docId, jsonStr);
  }

  public getDocument(prefix: string): object | null {
    // Chama a função C de reconstrução
    const result = db.prepare("SELECT extract_json(?) as json_data").get(prefix) as any;
    return result?.json_data ? JSON.parse(result.json_data) : null;
  }

  // Busca hierárquica usando o Prefix Range Trick (Sem REGEXP!)
  public searchByPath(pathPrefix: string): any[] {
    const upperBound = pathPrefix.slice(0, -1) + String.fromCharCode(pathPrefix.charCodeAt(pathPrefix.length - 1) + 1);

    return db
      .prepare(
        `
            SELECT path, type, text_value 
            FROM nodes 
            WHERE path >= ? AND path < ? 
            ORDER BY path
        `,
      )
      .all(pathPrefix, upperBound);
  }
}

// =====================================================================
// PROVA DE CONCEITO (PoC)
// =====================================================================
const store = new HierarchicalStore();

const sampleUser = {
  id: 100,
  name: "Alan Turing",
  address: {
    city: "Londres",
    zip: "12345",
  },
  tags: ["genius", "computer"],
};

console.log("\n--- Teste de Escrita (JSON -> Nodes) ---");
const startWrite = performance.now();
store.saveDocument("users/100", sampleUser);
console.log(`Escrita concluída em ${(performance.now() - startWrite).toFixed(2)}ms`);

console.log("\n--- Teste de Leitura (Nodes -> JSON) ---");
const startRead = performance.now();
const reconstructed = store.getDocument("/users/100");
console.log(`Leitura concluída em ${(performance.now() - startRead).toFixed(2)}ms`);
console.log("JSON Reconstruído:", JSON.stringify(reconstructed, null, 2));

console.log("\n--- Teste de Busca Hierárquica (Index Seek) ---");
const children = store.searchByPath("/users/100/address/");
console.log("Filhos do endereço:", children);

db.close();
