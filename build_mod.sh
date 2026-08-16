#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

find_macos_tool() {
    local formula="$1"
    local executable="$2"
    local prefix

    if ! command -v brew >/dev/null 2>&1; then
        return 1
    fi

    prefix="$(brew --prefix "$formula" 2>/dev/null)" || return 1
    if [[ -x "$prefix/bin/$executable" ]]; then
        printf '%s\n' "$prefix/bin/$executable"
        return 0
    fi

    return 1
}

if [[ "$(uname -s)" == "Darwin" ]]; then
    if [[ -z "${CC:-}" ]]; then
        CC="$(find_macos_tool llvm clang)" || {
            echo "Homebrew LLVM is required. Install it with: brew install llvm" >&2
            exit 1
        }
    fi

    if [[ -z "${LD:-}" ]]; then
        LD="$(find_macos_tool lld ld.lld || find_macos_tool llvm ld.lld)" || {
            echo "Homebrew LLD is required. Install it with: brew install lld" >&2
            exit 1
        }
    fi
else
    CC="${CC:-clang}"
    LD="${LD:-ld.lld}"
fi

if [[ -n "${RECOMP_MOD_TOOL:-}" ]]; then
    mod_tool="$RECOMP_MOD_TOOL"
elif [[ -x "$SCRIPT_DIR/RecompModTool" ]]; then
    mod_tool="$SCRIPT_DIR/RecompModTool"
elif [[ -x "$SCRIPT_DIR/../mnsg-recomp-example/RecompModTool" ]]; then
    # Reuse the local tool from the configured example checkout when available.
    mod_tool="$SCRIPT_DIR/../mnsg-recomp-example/RecompModTool"
elif command -v RecompModTool >/dev/null 2>&1; then
    mod_tool="$(command -v RecompModTool)"
else
    echo "RecompModTool was not found." >&2
    echo "Place it at $SCRIPT_DIR/RecompModTool or set RECOMP_MOD_TOOL to its path." >&2
    exit 1
fi

make clean
make CC="$CC" LD="$LD" "$@"
"$mod_tool" mod.toml build

artifact="$SCRIPT_DIR/build/mnsg_team_up.nrm"
if [[ ! -f "$artifact" ]]; then
    echo "Build completed, but the expected artifact was not created: $artifact" >&2
    exit 1
fi

echo "Done: $artifact"
