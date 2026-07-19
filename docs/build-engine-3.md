# 1. Build Engine (C vs Node.js)

Para o *parsing* e *serialização* JSON, bibliotecas nativas em C/C++ oferecem desempenho muito superior. Por exemplo, **simdjson** consegue parsear JSON enormes a velocidades de gigabytes/s, mas é *read-only*, enquanto **yyjson** é otimizado para construir/atualizar JSONs dinamicamente. Se essa lógica rodar **dentro do SQLite como extensão C**, pode receber a string JSON inteira e gerar entradas na tabela sem troca de contexto (IPC). Já no **Node.js**, usar `JSON.parse/stringify` é rápido no V8, mas cada chamada ao SQLite implica overhead de ponte JS⇄C. Benchmarks mostram que o driver `better-sqlite3` (API síncrona) supera alternatives em Node, mas ainda fica atrás de código nativo. Aliás, para JSON de tamanho moderado, “é melhor usar a implementação normal do V8 do que um addon C++, pois a ponte é custosa”. 

**Prós/Contras**:
- **C (Extensão SQLite)**: *Extrema performance* (parsing em memória usando SIMD, sem IPC), mas maior complexidade de desenvolvimento (gerenciamento manual de memória e bindings). Permite implementar algo como `SELECT upsert_json(?1);` usando simdjson/yyjson internamente.
- **Node.js (V8)**: *Facilidade de uso e maturidade* (ecossistema rico, TypeScript, N-API etc.), mas menor throughput puro. Requer transações em lote, prepared statements e possivelmente *worker threads* ou addons nativos para não travar o event loop. Mesmo assim, o número de consultas SQL (ou binds em batch) será o gargalo principal.

**Recomendação:** Para **latência mínima e máximo throughput**, a solução C (SQLite Loadable Extension) é imbatível. Porém, se a prioridade for manutenibilidade e velocidade de iteração, uma versão em Node.js com `better-sqlite3` e *batching* cuidadoso também pode atingir alta performance. Em resumo, recomendamos **C/C++ (extensão SQLite)** para o _build engine_ por eliminar overhead de contexto, usando simdjson para parse e yyjson para composição de JSON.

# 2. Arquitetura do Conversor JSON↔Nodes

O fluxo básico de conversão é o seguinte:

- **Flatten (JSON → Nodes):**  
  1. Parsear o JSON completo (em C: simdjson; em Node: `JSON.parse`).  
  2. Fazer *recursion* pelo objeto/array. Para cada campo ou elemento, montar uma *chave de caminho* (`path`), por exemplo `"/usuarios/100/endereco/cidade"`.  
  3. Para cada valor (primitivo ou objeto/array), determinar o `type` (objeto, array, string, binário, etc.) e preencher os campos da tabela `nodes`: `text_value`, `binary_value` ou `json_value` (este para objetos JSON curtos). Inserir em lote (ex.: várias linhas por *INSERT VALUES*, ou usar *prepared statement* executada repetidamente).  
  4. Envolver tudo numa transação atômica (`BEGIN...COMMIT`) usando o mesmo `revision` (UUID ou timestamp) para todos os nós do documento. Isso garante um histórico consistente.  

- **Unflatten (Nodes → JSON):**  
  1. Executar uma consulta ao SQLite obtendo as linhas relevantes, ordenadas por `path`. Por exemplo, para reconstruir `/usuarios/100`, buscar `SELECT * FROM nodes WHERE path LIKE '/usuarios/100/%' ORDER BY path`.  
  2. Iterar sobre os resultados, quebrando cada `path` em segmentos. Construir recursivamente o objeto JSON: ao descer no caminho, criar objetos/arrays e atribuir o `text_value` ou `binary_value` apropriado como valor de nó. Para `type=object/array`, pode usar `json_value` pré-armazenado quando disponível; caso contrário, agrupar filhos pelas chaves intermediárias.  
  3. Retornar o objeto JSON completo. (Opcionalmente, poderiam ser usadas funções JSON1 do SQLite, mas normalmente faz-se em código host.)

Este desenho garante que *uma única chamada* ao build engine gere todos os *INSERTs* necessários, e que a reconstrução percorra o caminho hierárquico usando consultas indexadas e montagem em memória.  

# 3. Otimizações de SQL, Paths e Regex

- **Índices de caminho:** a coluna `nodes.path` é **PRIMARY KEY** (WITHOUT ROWID), ou seja, já tem índice B-Tree. Para consultas hierárquicas, é ideal restringir pelo *prefixo* do path. Por exemplo, em vez de `WHERE path GLOB '/usuarios/*/pedidos'` (o GLOB padrão), pode-se usar:  
  ```sql
  WHERE path >= '/usuarios/' AND path < '/usuarios0'
  ```  
  Isso define um intervalo lexicográfico cobrindo todos que começam com `/usuarios/`. O SQLite otimiza esse padrão usando o índice (fazendo busca entre limites). Alternativamente, `WHERE path LIKE '/usuarios/%'` também usa índice (já que `%` só no fim).  

- **GLOB vs REGEXP:** no SQLite o operador `REGEXP` sempre exige função externa e **faz full table scan** (sem uso de índice). Para evitar isso, sempre que possível filtre primeiramente pelo prefixo: por exemplo,  
  ```sql
  SELECT * FROM nodes 
    WHERE path LIKE '/usuarios/%' 
      AND path REGEXP '^/usuarios/[0-9]+/pedidos'
  ```  
  O `LIKE '/usuarios/%'` reduz o conjunto via índice, e só então aplica o regex no conjunto menor. Em geral, `GLOB 'literal*'` ou `LIKE 'literal%'` permitem aceleração por índice, diferentemente de regex.

- **Índices expressões/JSON:** pode-se criar *índices em expressões* usando o JSON1 do SQLite. Por exemplo, se há consultas frequentes em um campo interno de `json_value`, pode-se fazer:  
  ```sql
  CREATE INDEX idx_subcampo ON nodes(json_extract(json_value, '$.subcampo'));
  ```  
  O SQLite suporta índices sobre expressões como essa. Isso acelera filtros em subpropriedades de JSON armazenado.

- **Tabelas Virtuais (FTS5/R-Tree):** em geral, FTS5 é para texto completo e R-Tree para dados espaciais, e não são diretamente aplicáveis a paths hierárquicos. Uma técnica alternativa seria criar uma tabela FTS5 apenas dos *paths* (como texto tokenizado) se buscas por substring forem necessárias, mas na maioria dos casos basta usar o método de prefixos acima.  

- **Campo híbrido `json_value`:** a ideia do `json_value` é guardar *filhos primitivos diretos* (string/número) como JSON curto no registro pai, evitando criar muitas linhas extra. Uma heurística comum: se um objeto tem poucos filhos (por exemplo ≤3) e eles são de tamanho pequeno, armazená-los inline em `json_value`. Se for grande (muitos campos) ou estruturas aninhadas, é melhor fragmentar em linhas separadas em `nodes`. Em resumo, balancear para não estourar o número de linhas nem deixar `json_value` excessivamente longo.

# 4. Concorrência, Transações e Histórico

- **Configuração do SQLite:** use `PRAGMA journal_mode = WAL` (Write-Ahead Log) e `PRAGMA synchronous = NORMAL`. Em WAL, leitores podem coexistir com gravações, o que dá enorme ganho de throughput e reduz bloqueios. O modo `synchronous=NORMAL` evita bloqueios excessivos do disco mas mantém alta integridade no caso de falhas. Além disso, recomenda-se aumentar `PRAGMA mmap_size` (ex.: `268435456`) para habilitar I/O mapeado em memória, o que pode acelerar leituras intensivas compartilhando o cache do SO.  

- **Transações e batch:** sempre agrupe várias inserções/updates em uma mesma transação. Em WAL, commits repetidos têm menos overhead, mas ainda assim executar *uma transação global por documento* (pai+filhos) reduz locks e aumenta throughput. Use *prepared statements* para as inserções: ao executar o mesmo `INSERT` em loop, um statement preparado e reutilizado pode melhorar o desempenho em ~1.5×. Em Node.js, por exemplo, usar `db.transaction(fn)` do better-sqlite3 é essencial para desativar autocommit e fazer em lote.

- **Histórico atômico:** ao atualizar um nó pai e vários filhos, envolva tudo em **uma única transação**. Gere um novo `revision` (timestamp/UUID) que seja comum a todas as linhas modificadas. Assim, todas as entradas no `history` terão o mesmo `revision_nr`, garantindo consistência. Graças a WAL, leitores não travam: eles verão a versão antiga até o commit, depois enxergarão de repente todos os novos dados de forma atômica. Para registrar o histórico, pode-se inserir linhas na tabela `history` dentro da mesma transação ou usar *triggers* SQLite que disparam após as operações em `nodes`. Em qualquer caso, o modelo assegura que cada conjunto de mudanças tenha um único carimbo de revisão atômico. 

# 5. Esqueleto de Código (SQL → JSON)

A seguir um exemplo conceitual em C (pseudo-código) de como reconstruir o JSON consultando o `nodes`:

```c
// Suponha conexão SQLite *db e função utilitária JSON_Object* create_json().
JSON_Object* unflatten(sqlite3 *db, const char *prefix) {
    JSON_Object *root = create_json_object(); 
    sqlite3_stmt *stmt;
    // Seleciona linhas cujo path começa com 'prefix'
    sqlite3_prepare_v2(db, 
        "SELECT path, type, text_value, binary_value, json_value "
        "FROM nodes WHERE path LIKE ? ORDER BY path;", 
        -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, prefix, -1, SQLITE_STATIC);
    // Para cada linha retornada, inserir no objeto JSON
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *path = (const char*)sqlite3_column_text(stmt, 0);
        int type = sqlite3_column_int(stmt, 1);
        const char *text = (const char*)sqlite3_column_text(stmt, 2);
        const void *bin = sqlite3_column_blob(stmt, 3);
        const char *subjson = (const char*)sqlite3_column_text(stmt, 4);
        // Quebrar path em segmentos, e navegar/criar sub-objetos
        JSON_Object *node = root;
        char *parts = strdup(path);
        char *seg = strtok(parts, "/");
        while ((seg = strtok(NULL, "/"))) {
            if (!json_object_has_key(node, seg)) {
                // Cria novo objeto ou array conforme 'type'
                json_object_set_value(node, seg, create_json_object());
            }
            node = json_object_get_object(node, seg);
        }
        // Agora 'node' é o objeto final do path; atribuir valor
        if (type == 5) {               // string
            json_object_set_string(node, "_value", text);
        } else if (type == 8) {        // binário
            json_object_set_string(node, "_value", bin);
        } else if (type == 1 || type == 2) { 
            // objeto ou array: usar json_value (curto) se não vazio
            if (subjson) {
                json_parse_into(node, subjson);
            }
        }
        free(parts);
    }
    sqlite3_finalize(stmt);
    return root; // Objeto JSON reconstruído
}
```

Nesse esqueleto, a consulta SQL obtém todos os nós sob um prefixo, e o laço itera sobre cada path, quebrando-o em segmentos e inserindo valores em um objeto JSON aninhado. Na prática, usaríamos as APIs reais do simdjson/yyjson em C, ou estruturas JavaScript/JSON do Node.js. O importante é ilustrar que a reconstrução percorre os registros ordenados por `path` e monta recursivamente o objeto JSON original.

**Fontes:** A escolha C vs Node é fundamentada em comparativos de performance e arquitetura. As dicas de configuração de WAL, mmap e prepared statements vêm da documentação do SQLite e análises de performance. O uso de índices em prefixos está comprovado pelo plano de consulta do SQLite, enquanto índices sobre expressões JSON são suportados nativamente. Cada ponto acima foi validado nas fontes referenciadas.