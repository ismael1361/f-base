Investigação Arquitetural: Motor de Armazenamento Hierárquico (JSON <-> Path) com SQLite, Regex e Avaliação de Desempenho (C vs. Node.js)

## 1. Contexto e Objetivo do Projeto

Estamos desenvolvendo um sistema de gerenciamento de dados relacional que atua como um **banco de dados hierárquico (estilo documento/árvore) rodando sobre o SQLite**. A proposta central é fragmentar e mapear documentos JSON arbitrários em uma estrutura de nós (`nodes`) baseada em chaves de caminho (`path`, ex.: `/usuarios/100/endereco/cidade`), utilizando tabelas otimizadas com `WITHOUT ROWID`, controle de revisões/histórico temporal e busca hierárquica acelerada por **plugins de Regex (`REGEXP`)**.

O objetivo desta pesquisa é determinar a **melhor arquitetura e tecnologia para o motor de compilação/conversão ("Build Engine")** responsável por transformar JSON em fluxos de inserção/atualização de nós e, inversamente, reconstruir árvores JSON a partir de consultas SQL por path, garantindo **baixíssima latência e alto throughput**.

---

## 2. Esquema de Banco de Dados de Referência

O modelo relacional base opera com as seguintes tabelas (com foco no uso de `WITHOUT ROWID` para otimização de cache B-Tree em chaves primárias textuais):

```sql
CREATE TABLE settings (
    name TEXT PRIMARY KEY,
    value TEXT
) WITHOUT ROWID;

CREATE TABLE nodes (
    path TEXT PRIMARY KEY,
    type INTEGER NOT NULL, -- 1=object, 2=array, 5=string, 8=binary, 9=reference
    text_value TEXT,       -- para strings longas ou referências
    binary_value BLOB,     -- para binários
    json_value TEXT,       -- otimização: armazena filhos primitivos diretos de objetos em JSON curto
    created INTEGER NOT NULL,
    modified INTEGER NOT NULL,
    revision_nr INTEGER NOT NULL,
    revision TEXT NOT NULL -- UUID/Timestamp sortável ("transaction timestamp")
) WITHOUT ROWID;

CREATE TABLE history (
    path TEXT NOT NULL,
    action TEXT NOT NULL,  -- insert, set, update, delete
    time INTEGER NOT NULL,
    revision_nr INTEGER NOT NULL,
    revision TEXT NOT NULL
);

CREATE TABLE indexes (
    name TEXT NOT NULL PRIMARY KEY,
    path TEXT NOT NULL,
    type TEXT NOT NULL,
    key TEXT NOT NULL,
    included_keys TEXT,
    table_name TEXT NOT NULL,
    created INTEGER NOT NULL,
    updated INTEGER NOT NULL,
    stats_entries INTEGER NOT NULL,
    stats_values INTEGER NOT NULL
) WITHOUT ROWID;

CREATE TABLE logs (
    action TEXT NOT NULL,
    success TINYINT NOT NULL,
    error TEXT,
    date INTEGER,
    details TEXT
);

```

---

## 3. Diretrizes e Eixos de Investigação

### Eixo 1: O Motor de Conversão (Build Engine: JSON $\leftrightarrow$ Nodes) — C vs. Node.js

Avalie criticamente as duas linguagens propostas para atuar como o parser/serializer e intermediador do banco de dados, focando em **tempo de resposta (latência de computação e I/O)** e **uso de memória**:

1. **Abordagem em C / C++ (Nativo ou SQLite Loadable Extension):**
* Avaliar o uso de bibliotecas de parsing JSON ultrarrápidas baseadas em SIMD (ex.: `simdjson` ou `yyjson`) para achatar (flatten) e reconstruir (unflatten) o JSON.
* Investigar a viabilidade de encapsular essa lógica **diretamente dentro do SQLite como uma Extensão Carregável em C (Loadable Extension) ou Funções Virtuais (VFS/VTab)**, eliminando o overhead de IPC/comunicação de processo.
* Analisar a complexidade de gerenciamento de memória no manuseio de strings e caminhos (paths) recursivos.


2. **Abordagem em Node.js (V8 Runtime):**
* Avaliar o desempenho usando drivers síncronos de baixo overhead (ex.: `better-sqlite3` ou wrappers em N-API/Rust como `node-sqlite3-wasm` / bindings nativos).
* Medir o custo de serialização/deserialização no V8 (`JSON.parse` / `JSON.stringify`) comparado à iteração de chaves para montagem dos paths.
* Investigar o uso de **Worker Threads** ou **Addons Nativos (C++/Rust via Node-API)** para realizar o *flattening/unflattening* pesado sem bloquear o Event Loop.


3. **Matriz Comparativa Obrigatória:**
* **Throughput (ops/sec):** Inserção de documento JSON de 10KB (divisão em 50+ nós) e leitura/reconstrução desse mesmo documento.
* **Overhead de Troca de Contexto:** Custo de boundary crossing entre a linguagem de host e o motor SQLite.
* **Manutenibilidade e Ecossistema:** Facilidade de evolução do código e integração com bibliotecas externas.



### Eixo 2: Modelagem e Performance de Consulta com Regex e Paths

A navegação em árvores materializadas no banco relacional depende de consultas eficientes sobre o atributo `path`.

1. **Uso do Plugin Regex (`REGEXP` match):**
* Avaliar o impacto de performance ao realizar buscas hierárquicas usando expressões regulares (ex.: `SELECT * FROM nodes WHERE path REGEXP '^/usuarios/[0-9]+/pedidos'`).
* Comparar o custo computacional de `REGEXP` versus operadores nativos de índice do SQLite, como `GLOB` (ex.: `path GLOB '/usuarios/*/pedidos/*'` ou prefix ranges `path >= '/a/' AND path < '/b/'`).
* Como criar índices expression-based ou tabelas virtuais (FTS5 / R-Tree) para evitar *Full Table Scans* em buscas por Regex profundo?


2. **Otimização do Campo Híbrido (`json_value`):**
* O esquema propõe guardar filhos primitivos diretos dentro de `json_value` para evitar explosão de linhas. Analise quais as regras heurísticas ideais para decidir o que vira uma linha na tabela `nodes` e o que permanece compactado em `json_value`.
* Avaliar o uso das funções JSON nativas do SQLite (`json_extract`, `json_patch`) trabalhando em conjunto com os caminhos.



### Eixo 3: Concorrência, Transações e Histórico temporal

1. **Configuração do Engine SQLite:**
* Para máxima resposta em gravações intensivas que geram múltiplas linhas por documento, qual a melhor configuração do SQLite? (Avaliar `WAL mode`, `SYNCHRONOUS = NORMAL`, `mmap_size`, e *Prepared Statement Caching*).


2. **Gerenciamento do Lote de Revisão (`revision` e `revision_nr`):**
* Como estruturar transações atômicas para que uma atualização de 1 nó pai e 20 nós filhos gere um único registro de `history` consistente sem travar o leitor?



---

## 4. Entregáveis Esperados da Análise

1. **Parecer Técnico Definitivo (C vs. Node.js):** Recomendação justificada da pilha tecnológica para o Build Engine com base nas premissas de velocidade e complexidade.
2. **Desenho da Arquitetura do Conversor:** Fluxograma explicativo de como o JSON entra, como o algoritmo de *flattening* calcula as chaves path e como os *statements* SQL em lote (batching) são gerados.
3. **Guia de Otimização SQL & Regex:** Lista de índices recomendados (`CREATE INDEX ...`) e estratégias de reescrita de queries para maximizar o uso da B-Tree do `WITHOUT ROWID`.
4. **Esqueleto de Código / PoC (Proof of Concept):** Exemplo conceitual da função principal de reversão (SQL $\rightarrow$ JSON Object) na linguagem vencedora.

---

## Síntese Inicial e Diretrizes de Engenharia

Para adiantar os pontos cruciais dessa investigação técnica, o cenário aponta para dinâmicas muito específicas em relação a performance:

* **O Gargalo Real não é a Linguagem, mas o IPC / Troca de Contexto:** Se você usar Node.js e fizer 100 queries individuais (`INSERT INTO nodes ...`) para salvar os nós de um único documento JSON, a ponte JavaScript $\rightarrow$ C++ (SQLite) será o gargalo de latência. A chave para a resposta rápida em **Node.js** com `better-sqlite3` é usar transações em bloco (`db.transaction(fn)`) ou instruções com múltiplos valores ou *Prepared Statements* reutilizados.
* **A Vantagem Imbatível do C (Loadable Extension):** Se você compilar o motor de conversão como uma **Extensão nativa do SQLite em C**, você pode passar a string JSON inteira para uma função SQL customizada: `SELECT * FROM json_to_nodes_vtab(?1);` ou `CALL upsert_json(?1);`. Usando bibliotecas como **`yyjson`** (o parser JSON em C mais rápido da atualidade), a conversão acontece inteiramente na memória nativa do SQLite, superando o Node.js em ordens de magnitude na velocidade de resposta pura.
* **Cuidado com o Regex no SQLite:** O operador `REGEXP` no SQLite é acionado por uma função externa que, por padrão, **não utiliza índices B-Tree**, resultando em um *Full Table Scan* (varredura de tabela completa). Para que o projeto tenha alto desempenho de leitura em árvores complexas, a investigação deve priorizar o uso de prefixos indexáveis (`WHERE path >= '/raiz/' AND path < '/raiz0'`) ou o operador `GLOB` para o primeiro nível de filtragem, aplicando o Regex apenas como refino no conjunto de dados retornado.

Aprofundar a investigação nestes três pontos com uma prova de conceito (PoC) será o passo decisivo para validar a arquitetura final.