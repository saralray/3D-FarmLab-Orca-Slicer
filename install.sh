#!/usr/bin/env bash
#
# 3D-FarmLab-Orca-Slicer — Linux installer
#
# Installs the AppImage with desktop integration (menu entry + icon). AppImages
# are self-contained, so this only copies the file into place and registers a
# launcher — it does not require root.
#
# Usage:
#   ./install.sh [path/to/AppImage]   # install (auto-finds build/*.AppImage)
#   ./install.sh --system [AppImage]  # install for all users (needs sudo)
#   ./install.sh --uninstall          # remove a previous installation
#
set -euo pipefail

APP_NAME="3D-FarmLab-Orca-Slicer"
APP_ID="3d-farmlab-orca-slicer"          # desktop-file / icon basename
BIN_NAME="${APP_ID}.AppImage"
REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

SYSTEM=0
UNINSTALL=0
APPIMAGE=""
for arg in "$@"; do
    case "$arg" in
        --system)    SYSTEM=1 ;;
        --uninstall) UNINSTALL=1 ;;
        -h|--help)   sed -n '2,17p' "$0"; exit 0 ;;
        *)           APPIMAGE="$arg" ;;
    esac
done

if [[ "$SYSTEM" -eq 1 ]]; then
    BIN_DIR="/usr/local/bin"
    DESKTOP_DIR="/usr/local/share/applications"
    ICON_DIR="/usr/local/share/icons/hicolor/192x192/apps"
    SUDO="sudo"
else
    BIN_DIR="${HOME}/.local/bin"
    DESKTOP_DIR="${HOME}/.local/share/applications"
    ICON_DIR="${HOME}/.local/share/icons/hicolor/192x192/apps"
    SUDO=""
fi

run() { if [[ -n "$SUDO" ]]; then sudo "$@"; else "$@"; fi; }

if [[ "$UNINSTALL" -eq 1 ]]; then
    echo "Removing ${APP_NAME}..."
    run rm -f "${BIN_DIR}/${BIN_NAME}" \
              "${DESKTOP_DIR}/${APP_ID}.desktop" \
              "${ICON_DIR}/${APP_ID}.png"
    command -v update-desktop-database >/dev/null 2>&1 && run update-desktop-database "${DESKTOP_DIR}" 2>/dev/null || true
    echo "Done."
    exit 0
fi

# Locate the AppImage: explicit arg, else the newest one under build/.
if [[ -z "$APPIMAGE" ]]; then
    APPIMAGE="$(ls -t "${REPO_ROOT}"/build/*.AppImage 2>/dev/null | grep -iv appimagetool | head -n1 || true)"
fi
if [[ -z "$APPIMAGE" || ! -f "$APPIMAGE" ]]; then
    echo "Error: no AppImage found. Pass its path, or build one first with:" >&2
    echo "  ./build_linux.sh -i" >&2
    exit 1
fi
echo "Installing $(basename "$APPIMAGE") as ${APP_NAME}..."

run mkdir -p "$BIN_DIR" "$DESKTOP_DIR" "$ICON_DIR"
run install -m 0755 "$APPIMAGE" "${BIN_DIR}/${BIN_NAME}"

# Extract the icon shipped inside the AppImage (best-effort).
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
( cd "$TMP_DIR" && "${BIN_DIR}/${BIN_NAME}" --appimage-extract '*.png' >/dev/null 2>&1 || true )
ICON_SRC="$(find "$TMP_DIR/squashfs-root" -maxdepth 2 -iname '*.png' 2>/dev/null | head -n1 || true)"
if [[ -n "$ICON_SRC" ]]; then
    run install -m 0644 "$ICON_SRC" "${ICON_DIR}/${APP_ID}.png"
    ICON_LINE="Icon=${APP_ID}"
else
    ICON_LINE="Icon=applications-engineering"
fi

# Write the launcher.
DESKTOP_TMP="$(mktemp)"
cat > "$DESKTOP_TMP" <<EOF
[Desktop Entry]
Name=${APP_NAME}
GenericName=3D Printing Software
Comment=Slice and send prints to your 3D-FarmLab print farm
Exec=${BIN_DIR}/${BIN_NAME} %U
${ICON_LINE}
Terminal=false
Type=Application
Categories=Graphics;3DGraphics;Engineering;
MimeType=model/stl;model/3mf;application/vnd.ms-3mfdocument;application/prs.wavefront-obj;application/x-amf;model/step;
Keywords=3D;Printing;Slicer;gcode;stl;3mf;
StartupNotify=true
StartupWMClass=orca-slicer
EOF
run install -m 0644 "$DESKTOP_TMP" "${DESKTOP_DIR}/${APP_ID}.desktop"
rm -f "$DESKTOP_TMP"

command -v update-desktop-database >/dev/null 2>&1 && run update-desktop-database "${DESKTOP_DIR}" 2>/dev/null || true

echo "Installed:"
echo "  Binary : ${BIN_DIR}/${BIN_NAME}"
echo "  Launcher: ${DESKTOP_DIR}/${APP_ID}.desktop"
if [[ "$SYSTEM" -eq 0 ]] && ! echo ":$PATH:" | grep -q ":${BIN_DIR}:"; then
    echo "Note: ${BIN_DIR} is not on your PATH; launch from the app menu or add it to PATH."
fi
echo "Launch it from your application menu as \"${APP_NAME}\", or run: ${BIN_NAME}"
