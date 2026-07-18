#!/bin/sh

# =============================================================================
# Build Script Cross-Platform (POSIX) para hierarchical_engine
# Funciona com: sh, bash, dash, Git Bash, MSYS2, WSL
# =============================================================================

set -e

# Configurações
SRC_DIR=$(cd "$(dirname "$0")" && pwd)
OUTPUT_NAME="hierarchical_engine"
SQLITE_DIR="$SRC_DIR/sqlite"

# Cores (funciona na maioria dos terminais modernos)
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo "${BLUE}========================================${NC}"
echo "${BLUE}Build Script - Hierarchical Engine${NC}"
echo "${BLUE}========================================${NC}"

# =============================================================================
# Detectar Sistema Operacional
# =============================================================================
detect_os() {
    OS="Unknown"

    # Tenta detectar via 'uname' (disponível em Unix, Git Bash, MSYS2, WSL)
    if command -v uname > /dev/null 2>&1; then
        OS_NAME=$(uname -s)
        case "$OS_NAME" in
            Linux*)     OS="Linux" ;;
            Darwin*)    OS="macOS" ;;
            CYGWIN*|MINGW*|MSYS*) OS="Windows" ;;
        esac
    fi

    # Fallback: variáveis de ambiente típicas do Windows
    if [ "$OS" = "Unknown" ]; then
        if [ -n "$COMSPEC" ] || [ -n "$ComSpec" ] || [ -n "$WINDIR" ]; then
            OS="Windows"
        fi
    fi

    echo "${YELLOW}Sistema detectado:${NC} $OS"
}

# =============================================================================
# Detectar Compilador Disponível
# =============================================================================
detect_compiler() {
    if command -v clang > /dev/null 2>&1; then
        CC="clang"
    elif command -v gcc > /dev/null 2>&1; then
        CC="gcc"
    elif command -v cl > /dev/null 2>&1; then
        CC="cl"
    else
        echo "${RED}Erro: Nenhum compilador encontrado (clang, gcc ou cl)${NC}"
        exit 1
    fi
    echo "${GREEN}Compilador encontrado:${NC} $CC"
}

# =============================================================================
# Verificar arquivos necessários
# =============================================================================
check_files() {
    echo "${YELLOW}Verificando arquivos...${NC}"
    
    # Lista de arquivos (sem usar arrays do Bash)
    FILES="$SRC_DIR/hierarchical_engine.c $SRC_DIR/yyjson.c $SRC_DIR/yyjson.h $SQLITE_DIR/sqlite3.c $SQLITE_DIR/sqlite3.h $SQLITE_DIR/sqlite3ext.h"
    
    for file in $FILES; do
        if [ ! -f "$file" ]; then
            echo "${RED}Erro: Arquivo não encontrado: $file${NC}"
            exit 1
        fi
    done
    
    echo "${GREEN}Todos os arquivos necessários encontrados!${NC}"
}

# =============================================================================
# Build para Linux/macOS (gcc/clang)
# =============================================================================
build_unix() {
    echo "${BLUE}Iniciando build para $OS...${NC}"
    
    if [ "$OS" = "macOS" ]; then
        EXT=".dylib"
        SHARED_FLAGS="-dynamiclib -undefined suppress -flat_namespace"
    else
        EXT=".so"
        SHARED_FLAGS="-shared -fPIC"
    fi
    
    # Silencia falso positivo do GCC 12+ no sqlite3 amalgamado
    WARN_FLAGS="-Wno-stringop-overread"
    
    OUTPUT_FILE="$SRC_DIR/${OUTPUT_NAME}${EXT}"
    
    echo "${YELLOW}Compilando...${NC}"
    
    $CC $SHARED_FLAGS $WARN_FLAGS -O3 \
        -I"$SQLITE_DIR" \
        -I"$SRC_DIR" \
        "$SRC_DIR/hierarchical_engine.c" \
        "$SRC_DIR/yyjson.c" \
        "$SQLITE_DIR/sqlite3.c" \
        -o "$OUTPUT_FILE" \
        -lpthread -ldl -lm
    
    echo "${GREEN}Build concluído com sucesso!${NC}"
    echo "${GREEN}Arquivo gerado:${NC} $OUTPUT_FILE"
}

# =============================================================================
# Build para Windows (MSVC - cl)
# =============================================================================
build_windows_msvc() {
    echo "${BLUE}Iniciando build para Windows (MSVC)...${NC}"
    
    OUTPUT_FILE="$SRC_DIR/${OUTPUT_NAME}.dll"
    
    echo "${YELLOW}Compilando com MSVC...${NC}"
    
    # Nota: No MSVC, usamos barras normais, o compilador entende
    cl /LD /O2 /I"$SQLITE_DIR" /I"$SRC_DIR" \
        "$SRC_DIR/hierarchical_engine.c" \
        "$SRC_DIR/yyjson.c" \
        "$SQLITE_DIR/sqlite3.c" \
        /Fe:"$OUTPUT_FILE"
    
    echo "${GREEN}Build concluído com sucesso!${NC}"
    echo "${GREEN}Arquivo gerado:${NC} $OUTPUT_FILE"
}

# =============================================================================
# Build para Windows (MinGW/Git Bash - gcc)
# =============================================================================
build_windows_mingw() {
    echo "${BLUE}Iniciando build para Windows (MinGW/GCC)...${NC}"
    
    OUTPUT_FILE="$SRC_DIR/${OUTPUT_NAME}.dll"
    
    echo "${YELLOW}Compilando com GCC...${NC}"
    
    # Silencia falso positivo do GCC 12+ no sqlite3 amalgamado
    WARN_FLAGS="-Wno-stringop-overread"
    
    $CC -shared -O3 $WARN_FLAGS \
        -I"$SQLITE_DIR" \
        -I"$SRC_DIR" \
        "$SRC_DIR/hierarchical_engine.c" \
        "$SRC_DIR/yyjson.c" \
        "$SQLITE_DIR/sqlite3.c" \
        -o "$OUTPUT_FILE"
    
    echo "${GREEN}Build concluído com sucesso!${NC}"
    echo "${GREEN}Arquivo gerado:${NC} $OUTPUT_FILE"
}

# =============================================================================
# Função principal
# =============================================================================
main() {
    echo ""
    detect_os
    detect_compiler
    echo ""
    check_files
    echo ""
    
    case "$OS" in
        Linux)   build_unix ;;
        macOS)   build_unix ;;
        Windows)
            if [ "$CC" = "cl" ]; then
                build_windows_msvc
            else
                build_windows_mingw
            fi
            ;;
        *)
            echo "${RED}Sistema operacional não suportado: $OS${NC}"
            exit 1
            ;;
    esac
    
    echo ""
    echo "${BLUE}========================================${NC}"
    echo "${GREEN}Build finalizado!${NC}"
    echo "${BLUE}========================================${NC}"
}

main "$@"