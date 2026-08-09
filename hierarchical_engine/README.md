# Hierarchical Engine - Build Guide

Documentação completa para compilação da extensão SQLite em C/C++.

---

##  Pré-requisitos

### 1. Compilador C/C++

#### **Linux (Ubuntu/Debian)**
```bash
sudo apt-get update
sudo apt-get install -y build-essential
```

#### **Linux (Fedora/RHEL)**
```bash
sudo dnf install -y gcc gcc-c++ make
```

#### **macOS**
```bash
# Instala o Xcode Command Line Tools
xcode-select --install
```

#### **Windows**

**Opção A - Visual Studio Build Tools (Recomendado)**
1. Baixe o [Visual Studio Build Tools](https://visualstudio.microsoft.com/visual-cpp-build-tools/)
2. Durante a instalação, selecione:
   - ✅ **"Desenvolvimento para Desktop com C++"**
   - ✅ **Windows 10/11 SDK**
3. Use o **x64 Native Tools Command Prompt** para compilar

**Opção B - MSYS2/MinGW**
```bash
# Baixe e instale o MSYS2 de https://www.msys2.org/
# No terminal MSYS2:
pacman -S mingw-w64-x86_64-gcc
pacman -S mingw-w64-x86_64-sqlite3
```

**Opção C - WSL (Windows Subsystem for Linux)**
```bash
# No PowerShell (como Admin):
wsl --install

# Dentro do WSL (Ubuntu):
sudo apt-get update
sudo apt-get install -y build-essential
```

---

### 2. Dependências do Projeto

Estrutura de pastas necessária:

```
hierarchical_engine/
├── build.sh                    # Script de build (este arquivo)
├── yyjson.c                    # Parser JSON ultrarrápido
├── yyjson.h
├── src/                        # Código do motor (Clean Architecture)
│   ├── he_types.h              # Tipos de domínio (TYPE_*, structs)
│   ├── he_utils.c/h            # Utils puras (uuid, paths, wildcards, json)
│   ├── he_repo.c/h             # Persistência (transações, insert/delete, ensure)
│   ├── he_mapper.c/h           # Flatten JSON→nodes + deep merge
│   ├── he_query.c/h            # Query engine (filtros/ordenação/paginação)
│   ├── he_csv.c/h              # Parser/serializer CSV (RFC 4180)
│   ├── he_services.c/h         # Use-cases (set/update/extract/query/export/import)
│   └── he_extension.c          # Controllers SQL + entry point sqlite3_extension_init
└── sqlite/                     # SQLite amalgamation
    ├── sqlite3.c
    ├── sqlite3.h
    └── sqlite3ext.h
```

> O antigo `hierarchical_engine.c` (monólito de ~3.3k linhas) foi dividido
> em módulos `src/he_*.{c,h}`. Apenas `he_extension.c` contém os controllers
> SQL e o `sqlite3_extension_init`; os demais módulos são camadas com
> dependência unidirecional: `extension → services → {mapper, query, csv, repo}`.

#### **Como obter o SQLite Amalgamation**

```bash
# 1. Acesse https://www.sqlite.org/download.html
# 2. Baixe: sqlite-amalgamation-XXXXXXX.zip
# 3. Extraia e mova os arquivos:

mkdir -p sqlite
cp sqlite-amalgamation-*/sqlite3.c sqlite/
cp sqlite-amalgamation-*/sqlite3.h sqlite/
cp sqlite-amalgamation-*/sqlite3ext.h sqlite/
```

#### **Como obter o yyjson**

```bash
# Clone o repositório ou baixe manualmente
git clone https://github.com/ibireme/yyjson.git

# Copie os arquivos necessários
cp yyjson/src/yyjson.c .
cp yyjson/src/yyjson.h .
```

---

## 🚀 Como Compilar

### Passo 1: Verificar estrutura de arquivos

```bash
# Liste os arquivos para confirmar que tudo está no lugar
ls -la
ls -la sqlite/
```

**Saída esperada:**
```
yyjson.c
yyjson.h
src/
├── he_types.h
├── he_utils.c    he_utils.h
├── he_repo.c     he_repo.h
├── he_mapper.c   he_mapper.h
├── he_query.c    he_query.h
├── he_csv.c      he_csv.h
├── he_services.c he_services.h
└── he_extension.c
sqlite/
├── sqlite3.c
├── sqlite3.h
└── sqlite3ext.h
build.sh
```

### Passo 2: Dar permissão de execução (Linux/macOS)

```bash
chmod +x build.sh
```

### Passo 3: Executar o build

```bash
./build.sh
```

---

## Saída da Compilação

O script gerará **um** dos seguintes arquivos, dependendo do sistema:

| Sistema Operacional | Arquivo Gerado              |
| ------------------- | --------------------------- |
| **Linux**           | `hierarchical_engine.so`    |
| **macOS**           | `hierarchical_engine.dylib` |
| **Windows**         | `hierarchical_engine.dll`   |

### Verificando o arquivo gerado

**Linux/macOS:**
```bash
ls -lh hierarchical_engine.*
file hierarchical_engine.*
```

**Windows:**
```cmd
dir hierarchical_engine.dll
```

---

## Solução de Problemas

### Erro: "sqlite3.h: No such file or directory"

**Causa:** Arquivos do SQLite não estão na pasta `sqlite/`

**Solução:**
```bash
# Verifique se os arquivos existem
ls sqlite/sqlite3.h sqlite/sqlite3ext.h

# Se não existirem, baixe o amalgamation conforme instruído acima
```

---

### Erro: "yyjson.h: No such file or directory"

**Causa:** Biblioteca yyjson não foi baixada

**Solução:**
```bash
# Baixe o yyjson
git clone https://github.com/ibireme/yyjson.git
cp yyjson/src/yyjson.c .
cp yyjson/src/yyjson.h .
```

---

### Erro: "Permission denied" ao executar ./build.sh

**Causa:** Script sem permissão de execução

**Solução:**
```bash
chmod +x build.sh
./build.sh
```

---

### Erro: "cl: command not found" (Windows)

**Causa:** Tentando usar MSVC sem o Visual Studio instalado

**Solução:**
- **Opção 1:** Instale o Visual Studio Build Tools
- **Opção 2:** Use Git Bash/MSYS2 com MinGW (gcc)
- **Opção 3:** Use WSL e compile como Linux

---

### Erro: "undefined reference to `pthread_create`" (Linux)

**Causa:** Biblioteca pthread não linkada

**Solução:** O script já inclui `-lpthread`, mas se persistir:
```bash
sudo apt-get install libpthread-stubs0-dev
```

---

### Erro no Windows: "cannot find -lsqlite3"

**Causa:** Tentando linkar com biblioteca externa em vez de compilar o sqlite3.c

**Solução:** O script já inclui `sqlite3.c` na compilação. Verifique se o arquivo existe:
```bash
ls sqlite/sqlite3.c
```

---

##  Testando a Extensão

Após compilar com sucesso, teste no Node.js:

```typescript
import Database from 'better-sqlite3';

const db = new Database('test.db');

// Carregar a extensão
db.loadExtension('./hierarchical_engine');

// Testar função
const result = db.prepare('SELECT ingest_json(?, ?)').get('test', '{"hello":"world"}');
console.log('Sucesso:', result);

db.close();
```

---

## Flags de Compilação

O script usa as seguintes otimizações:

| Flag              | Descrição                           |
| ----------------- | ----------------------------------- |
| `-O3`             | Otimização máxima de velocidade     |
| `-shared` / `/LD` | Gera biblioteca dinâmica (.so/.dll) |
| `-fPIC`           | Position Independent Code (Linux)   |
| `-I`              | Include path para headers           |
| `-lpthread`       | Linka biblioteca de threads         |
| `-ldl`            | Linka biblioteca de dynamic loading |
| `-lm`             | Linka biblioteca matemática         |

---

## Notas por Plataforma

### Linux
- Requer `gcc` ou `clang`
- Saída: `.so` (Shared Object)
- Compatível com melhor-sqlite3 nativamente

### macOS
- Requer Xcode Command Line Tools
- Saída: `.dylib` (Dynamic Library)
- Flags especiais: `-dynamiclib -undefined suppress -flat_namespace`

### Windows (MSVC)
- Requer Visual Studio Build Tools
- Use **x64 Native Tools Command Prompt**
- Saída: `.dll` (Dynamic Link Library)
- Compatível com Node.js 64-bit

### Windows (MinGW/WSL)
- GCC para Windows ou Linux
- Saída: `.dll` (MinGW) ou `.so` (WSL)
- WSL requer Node.js rodando dentro do WSL

---

## Rebuild Limpo

Para recompilar do zero:

```bash
# Remover arquivos anteriores
rm -f hierarchical_engine.so
rm -f hierarchical_engine.dylib
rm -f hierarchical_engine.dll

# Executar build novamente
./build.sh
```

---

##  Tamanho Esperado do Binário

Devido à inclusão do `sqlite3.c` completo, o binário será grande:

| Plataforma | Tamanho Aproximado |
| ---------- | ------------------ |
| Linux      | ~2.5 MB            |
| macOS      | ~3.0 MB            |
| Windows    | ~2.8 MB            |

Isso é **normal** e esperado, pois estamos compilando o SQLite inteiro junto com a extensão.

---

##  Próximos Passos

Após compilar com sucesso:

1. ✅ Verifique se o arquivo `.so`/`.dll`/`.dylib` foi gerado
2. ✅ Instale o `better-sqlite3` no Node.js: `npm install better-sqlite3`
3. ✅ Use o arquivo TypeScript `index.ts` para testar
4. ✅ Execute: `npx ts-node index.ts`

---

## Suporte

Se encontrar erros não documentados:

1. Execute o build com verbose:
   ```bash
   bash -x ./build.sh
   ```

2. Verifique a versão do compilador:
   ```bash
   gcc --version
   # ou
   cl
   ```

3. Confirme que está usando Node.js 64-bit:
   ```bash
   node -p "process.arch"
   # Deve retornar: x64
   ```

---

**Build bem-sucedido**  
Agora você tem uma extensão SQLite ultra-rápida para manipulação de JSON hierárquico!