#!/bin/bash
# Downloads required dependencies: ImGui and nlohmann/json
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
LIB_DIR="$PROJECT_DIR/lib"

IMGUI_VERSION="v1.80"
JSON_VERSION="v3.11.3"

echo "=== Realm Report: Dependency Setup ==="

# --- ImGui ---
IMGUI_DIR="$LIB_DIR/imgui"
if [ -d "$IMGUI_DIR" ] && [ -f "$IMGUI_DIR/imgui.h" ]; then
    echo "[OK] ImGui already present"
else
    echo "[DL] Downloading ImGui $IMGUI_VERSION..."
    rm -rf "$IMGUI_DIR"
    mkdir -p "$IMGUI_DIR"
    IMGUI_URL="https://github.com/ocornut/imgui/archive/refs/tags/${IMGUI_VERSION}.tar.gz"
    curl -sL "$IMGUI_URL" | tar xz --strip-components=1 -C "$IMGUI_DIR"
    echo "[OK] ImGui downloaded"
fi

# --- nlohmann/json ---
JSON_DIR="$LIB_DIR/nlohmann"
if [ -d "$JSON_DIR" ] && [ -f "$JSON_DIR/json.hpp" ]; then
    echo "[OK] nlohmann/json already present"
else
    echo "[DL] Downloading nlohmann/json $JSON_VERSION..."
    mkdir -p "$JSON_DIR"
    curl -sL "https://github.com/nlohmann/json/releases/download/${JSON_VERSION}/json.hpp" \
        -o "$JSON_DIR/json.hpp"
    echo "[OK] nlohmann/json downloaded"
fi

echo "=== Setup complete ==="
