#!/usr/bin/env bash
# Resolve winject-manager path and build it.
# Usage: ensure_winject_manager <repo_root> [build_manager_arm|build_manager_x86]
# Second arg selects the CMake tree; default is from uname -m.
ensure_winject_manager() {
    local root="${1:?repo root required}"
    local tree="${2:-}"

    if [[ -z "$tree" ]]; then
        case "$(uname -m)" in
            aarch64|arm64) tree=build_manager_arm ;;
            *)             tree=build_manager_x86 ;;
        esac
    fi

    MANAGER_BUILD="${WINJECT_MANAGER_BUILD:-$root/$tree}"
    MANAGER="${WINJECT_MANAGER:-$MANAGER_BUILD/winject-manager}"

    _manager_matches_host() {
        [[ -x "$MANAGER" ]] || return 1
        case "$(uname -m)" in
            aarch64|arm64)
                file "$MANAGER" | grep -qE 'ARM aarch64|aarch64' ;;
            x86_64|amd64)
                file "$MANAGER" | grep -qE 'x86-64|80386' ;;
            *)
                return 1 ;;
        esac
    }

    echo "building winject-manager in $MANAGER_BUILD..." >&2
    cmake -S "$root/src/manager" -B "$MANAGER_BUILD" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$MANAGER_BUILD" -j"$(nproc)"
    MANAGER="$MANAGER_BUILD/winject-manager"

    if ! _manager_matches_host; then
        echo "error: $MANAGER is not built for $(uname -m)" >&2
        return 1
    fi
}
