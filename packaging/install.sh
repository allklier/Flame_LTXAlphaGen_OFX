#!/usr/bin/env bash
# LTX Alpha Gen for Flame: install or uninstall the plugin and its Flame Python hook.
#   sudo ./install.sh              install (replaces an earlier LTX Alpha Gen)
#   sudo ./install.sh --uninstall  remove both
# Installs:
#   /usr/OFX/Plugins/LTXAlphaGen.ofx.bundle             (macOS: /Library/OFX/Plugins)
#   /opt/Autodesk/shared/python/ltx_alphagen_hook.py   (refreshes the viewer when a job ends)
# Models, settings and the matte cache are per user and are not touched (see README).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PLUGINS=/usr/OFX/Plugins
[ "$(uname -s)" = Darwin ] && PLUGINS=/Library/OFX/Plugins  # where the Mac's OFX plugins live
BUNDLE=LTXAlphaGen.ofx.bundle
HOOK_DIR=/opt/Autodesk/shared/python
HOOK=ltx_alphagen_hook.py
HOOK_SIGNATURE="LTX Alpha Gen: Flame Python hook"  # first line of our hook's docstring

die() { echo "error: $*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "run with sudo (it writes to $PLUGINS)"
if pgrep -x flame >/dev/null 2>&1; then
  echo "warning: Flame is running. Quit Flame, then run this again." >&2
  exit 1
fi

# Our hook (any version), or nothing there: safe to replace or remove.
hook_is_ours() {
  local p="$HOOK_DIR/$HOOK"
  [ ! -e "$p" ] && [ ! -L "$p" ] && return 0
  [ -L "$p" ] && [ ! -e "$p" ] && return 0  # dangling link (e.g. a removed dev build)
  grep -q "$HOOK_SIGNATURE" "$p"
}

if [ "${1:-}" = "--uninstall" ]; then
  if [ -e "$PLUGINS/$BUNDLE" ] || [ -L "$PLUGINS/$BUNDLE" ]; then
    rm -rf "${PLUGINS:?}/$BUNDLE"
    echo "removed $PLUGINS/$BUNDLE"
  fi
  if hook_is_ours; then
    rm -f "$HOOK_DIR/$HOOK" && echo "removed $HOOK_DIR/$HOOK"
  else
    echo "left $HOOK_DIR/$HOOK alone: it is not the LTX Alpha Gen hook"
  fi
  echo "Done. Your models, settings (~/.config/ltx-alphagen) and cache (~/.cache/ltx-alphagen) are kept."
  exit 0
fi

[ -d "$HERE/$BUNDLE" ] || die "$BUNDLE not found next to this script"
[ -f "$HERE/$BUNDLE/Contents/Resources/flame_hook/$HOOK" ] || die "the bundle has no flame_hook/$HOOK"
hook_is_ours || die "$HOOK_DIR/$HOOK exists and is not the LTX Alpha Gen hook; nothing was changed"

mkdir -p "$PLUGINS"
if [ -e "$PLUGINS/$BUNDLE" ] || [ -L "$PLUGINS/$BUNDLE" ]; then
  echo "replacing $PLUGINS/$BUNDLE"
  rm -rf "${PLUGINS:?}/$BUNDLE"
fi
cp -R "$HERE/$BUNDLE" "$PLUGINS/"
chmod -R a+rX "$PLUGINS/$BUNDLE"
# macOS: a downloaded tarball marks every file as quarantined; the bundle is not notarised.
[ "$(uname -s)" = Darwin ] && xattr -dr com.apple.quarantine "$PLUGINS/$BUNDLE" 2>/dev/null || true
echo "installed $PLUGINS/$BUNDLE"

if [ -d "$HOOK_DIR" ]; then
  rm -f "$HOOK_DIR/$HOOK"  # also a link left by a dev install
  install -m 0644 "$HERE/$BUNDLE/Contents/Resources/flame_hook/$HOOK" "$HOOK_DIR/$HOOK"
  echo "installed $HOOK_DIR/$HOOK"
else
  echo "warning: $HOOK_DIR does not exist (is Flame installed?). The plugin works without the hook," >&2
  echo "         but the viewer will not refresh by itself when a job finishes." >&2
fi
echo "Done. Start Flame; the node is under LTX > LTX Alpha Gen. Then run Setup on its Setup tab."
