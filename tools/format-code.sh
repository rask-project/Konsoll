#!/bin/bash
# Formats the application's code: C++ / Objective-C++ with clang-format
# (.clang-format) and QML with qmlformat (.qmlformat.ini).
#
# Usage: tools/format-code.sh [--check] [--changed] [FILE...]
#   (none)     format every C++, Objective-C++ and QML file in place
#   --check    change nothing; list the files that need formatting and exit 1
#   --changed  only the files changed against HEAD, plus untracked ones
#   FILE...    only these files
#
# Tools: CLANG_FORMAT and QMLFORMAT override them; otherwise clang-format is
# taken from PATH and qmlformat from PATH or the Qt kit (the build's
# CMAKE_PREFIX_PATH, then /opt/Qt/*/macos/bin).
set -euo pipefail

cd "$(dirname "$0")/.."

CHECK=false
CHANGED=false
FILES=()
for arg in "$@"; do
    case "$arg" in
    --check | -n | --dry-run) CHECK=true ;;
    --changed) CHANGED=true ;;
    -h | --help)
        sed -n '2,13s/^# \{0,1\}//p' "$0"
        exit 0
        ;;
    -*)
        echo "format-code.sh: unknown option $arg" >&2
        exit 2
        ;;
    *) FILES+=("$arg") ;;
    esac
done

find_qmlformat() {
    if [[ -n "${QMLFORMAT:-}" ]]; then
        echo "$QMLFORMAT"
        return
    fi
    if command -v qmlformat >/dev/null 2>&1; then
        command -v qmlformat
        return
    fi
    local cache prefix candidate
    for cache in build/*/CMakeCache.txt; do
        [[ -f "$cache" ]] || continue
        prefix="$(sed -n 's/^CMAKE_PREFIX_PATH:[A-Z]*=//p' "$cache" | head -n 1)"
        # <Qt>/macos/lib/cmake -> <Qt>/macos/bin/qmlformat
        candidate="${prefix%/lib/cmake*}/bin/qmlformat"
        if [[ -n "$prefix" && -x "$candidate" ]]; then
            echo "$candidate"
            return
        fi
    done
    for candidate in /opt/Qt/*/macos/bin/qmlformat "$HOME"/Qt/*/macos/bin/qmlformat; do
        if [[ -x "$candidate" ]]; then
            echo "$candidate"
            return
        fi
    done
}

CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"
if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
    echo "format-code.sh: $CLANG_FORMAT not found (brew install clang-format, or set CLANG_FORMAT)" >&2
    exit 127
fi
QMLFORMAT_BIN="$(find_qmlformat)"
if [[ -z "$QMLFORMAT_BIN" || ! -x "$QMLFORMAT_BIN" ]]; then
    echo "format-code.sh: qmlformat not found (set QMLFORMAT to <Qt>/macos/bin/qmlformat)" >&2
    exit 127
fi

# The candidate files: given, changed, or every tracked and untracked
# (not ignored) file, so build/ and generated code stay out.
list_files() {
    if ((${#FILES[@]} > 0)); then
        printf '%s\n' "${FILES[@]}"
    elif $CHANGED; then
        git diff --name-only --diff-filter=ACMR HEAD
        git ls-files --others --exclude-standard
    else
        git ls-files --cached --others --exclude-standard
    fi
}

CPP_FILES=()
QML_FILES=()
while IFS= read -r file; do
    [[ -f "$file" ]] || continue
    case "$file" in
    tests/fixtures/*) ;;
    *.cpp | *.h | *.mm) CPP_FILES+=("$file") ;;
    *.qml) QML_FILES+=("$file") ;;
    esac
done < <(list_files | sort -u)

JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
status=0

if ((${#CPP_FILES[@]} > 0)); then
    if $CHECK; then
        # clang-format reports each line that would change; list the files.
        needs="$(printf '%s\0' "${CPP_FILES[@]}" |
            xargs -0 -n 8 -P "$JOBS" "$CLANG_FORMAT" --dry-run -Werror --style=file 2>&1 |
            sed -n 's/^\([^:]*\):[0-9]*:[0-9]*: error: .*/\1: needs clang-format/p' | sort -u || true)"
        if [[ -n "$needs" ]]; then
            echo "$needs"
            status=1
        fi
    else
        printf '%s\0' "${CPP_FILES[@]}" | xargs -0 -n 8 -P "$JOBS" "$CLANG_FORMAT" -i --style=file
    fi
fi

if ((${#QML_FILES[@]} > 0)); then
    if $CHECK; then
        for file in "${QML_FILES[@]}"; do
            if ! "$QMLFORMAT_BIN" "$file" | cmp -s - "$file"; then
                echo "$file: needs qmlformat"
                status=1
            fi
        done
    else
        printf '%s\0' "${QML_FILES[@]}" | xargs -0 -n 8 -P "$JOBS" "$QMLFORMAT_BIN" -i
    fi
fi

if ! $CHECK; then
    echo "Formatted ${#CPP_FILES[@]} C++/Objective-C++ and ${#QML_FILES[@]} QML files."
fi
exit $status
