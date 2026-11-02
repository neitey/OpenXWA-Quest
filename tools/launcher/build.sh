#!/usr/bin/env bash
# Builds the OpenXWA-VR Launcher.
#   ./build.sh 1.0.0-rc3
# Produces dist/OpenXWA-VR-Launcher.exe (Windows GUI program, the release build) and
# dist/openxwa-vr-launcher-linux (for testing).
set -euo pipefail

if [ $# -ne 1 ] || [ -z "$1" ]; then
	echo "usage: $0 <version>   (for example 1.0.0 or 1.0.0-rc3)" >&2
	exit 2
fi
VER="${1#v}"

cd "$(dirname "$0")"
GO="${GO:-go}"
if ! command -v "$GO" >/dev/null 2>&1 && [ -x /usr/local/go/bin/go ]; then
	GO=/usr/local/go/bin/go
fi

mkdir -p dist
LDFLAGS="-s -w -X main.version=$VER"

# LAUNCHER_ICON=<file.ico>: the program icon for the Windows build. The icon
# art is not in the repository: it is compiled into a resource object
# (rsrc_windows_amd64.syso, which go build links on Windows only) for this
# build and removed again. WINDRES: the llvm-mingw / mingw windres to use.
SYSO=rsrc_windows_amd64.syso
rm -f "$SYSO"
if [ -n "${LAUNCHER_ICON:-}" ]; then
	WINDRES="${WINDRES:-}"
	if [ -z "$WINDRES" ]; then
		for w in x86_64-w64-mingw32-windres /root/pcvr-deps/llvm-mingw/bin/x86_64-w64-mingw32-windres llvm-windres; do
			if command -v "$w" >/dev/null 2>&1; then WINDRES=$w; break; fi
		done
	fi
	[ -n "$WINDRES" ] || { echo "LAUNCHER_ICON needs windres (set WINDRES)" >&2; exit 1; }
	[ -f "$LAUNCHER_ICON" ] || { echo "no icon at $LAUNCHER_ICON" >&2; exit 1; }
	ICON_ABS="$(cd "$(dirname "$LAUNCHER_ICON")" && pwd)/$(basename "$LAUNCHER_ICON")"
	RC="$(mktemp --suffix=.rc)"
	printf '1 ICON "%s"\n' "$ICON_ABS" > "$RC"
	"$WINDRES" -O coff -i "$RC" -o "$SYSO"
	rm -f "$RC"
	trap 'rm -f "$SYSO"' EXIT
fi

# -H windowsgui: a GUI program, so no console window opens (--console makes one).
CGO_ENABLED=0 GOOS=windows GOARCH=amd64 "$GO" build -trimpath -ldflags "$LDFLAGS -H windowsgui" -o dist/OpenXWA-VR-Launcher.exe .
CGO_ENABLED=0 GOOS=linux GOARCH=amd64 "$GO" build -trimpath -ldflags "$LDFLAGS" -o dist/openxwa-vr-launcher-linux .

echo "Built version $VER:"
ls -l dist/OpenXWA-VR-Launcher.exe dist/openxwa-vr-launcher-linux
