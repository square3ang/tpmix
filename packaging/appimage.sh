#!/bin/sh
# Build tpmix-x86_64.AppImage with linuxdeploy + its Qt plugin.
# Run from the repo root on the oldest distro you want to support (glibc).
set -eu

TOOLS=${TOOLS:-build/tools}
mkdir -p "$TOOLS"
for t in linuxdeploy-x86_64.AppImage linuxdeploy-plugin-qt-x86_64.AppImage; do
  [ -x "$TOOLS/$t" ] || {
    repo=${t%-x86_64.AppImage}
    curl -fsSL -o "$TOOLS/$t" \
      "https://github.com/linuxdeploy/$repo/releases/download/continuous/$t"
    chmod +x "$TOOLS/$t"
  }
done

make clean
make

rm -rf build/AppDir
export QMAKE=${QMAKE:-$(command -v qmake6 || command -v qmake)}
# Qt >= 6.10 merged the wayland platform plugins into libqwayland.so
plugdir=$("$QMAKE" -query QT_INSTALL_PLUGINS)/platforms
for w in libqwayland-generic.so libqwayland.so; do
  [ -f "$plugdir/$w" ] && export EXTRA_PLATFORM_PLUGINS=$w
done
# CI runners have no FUSE
export APPIMAGE_EXTRACT_AND_RUN=1
export PATH="$PWD/$TOOLS:$PATH"
linuxdeploy-x86_64.AppImage --appdir build/AppDir \
  --executable tpmix \
  --desktop-file packaging/tpmix.desktop \
  --icon-file packaging/tpmix.svg \
  --custom-apprun packaging/AppRun \
  --plugin qt --output appimage
