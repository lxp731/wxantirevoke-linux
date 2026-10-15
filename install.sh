#!/bin/sh
# 安装 wxantirevoke（全部在 $HOME 下，不需要 root，随时可 uninstall.sh）
#
#   ~/.local/lib/wxantirevoke/libwxantirevoke.so   补丁库（日常用）
#   ~/.local/lib/wxantirevoke/libwxprobe.so        探针库（诊断用）
#   ~/.local/bin/wechat-antirevoke                 启动器（带补丁）
#   ~/.local/bin/wechat-probe                      启动器（补丁 + 探针，排障用）
#   ~/.local/bin/wxantirevoke-check                补丁点自检（不启动微信）
#   ~/.local/bin/wxantirevoke-mark                 给探针日志打计数快照
#   ~/.local/share/applications/wechat.desktop     桌面项（覆盖系统同名项）
set -eu

SRC=$(cd "$(dirname "$0")" && pwd)
LIBDIR="$HOME/.local/lib/wxantirevoke"
BINDIR="$HOME/.local/bin"
APPDIR="$HOME/.local/share/applications"

echo "==> 编译"
gcc -shared -fPIC -O2 -Wall -Wextra -o "$SRC/libwxantirevoke.so" "$SRC/wxantirevoke.c"
gcc -shared -fPIC -O2 -Wall -Wextra -o "$SRC/probe/libwxprobe.so" "$SRC/probe/wxprobe.c" -lpthread

echo "==> 安装到 $LIBDIR / $BINDIR"
install -d "$LIBDIR" "$BINDIR" "$APPDIR"
install -m 0644 "$SRC/libwxantirevoke.so"          "$LIBDIR/libwxantirevoke.so"
install -m 0644 "$SRC/probe/libwxprobe.so"         "$LIBDIR/libwxprobe.so"
install -m 0755 "$SRC/launcher/wechat-antirevoke"  "$BINDIR/wechat-antirevoke"
install -m 0755 "$SRC/launcher/wechat-probe"       "$BINDIR/wechat-probe"
install -m 0755 "$SRC/launcher/wxantirevoke-check" "$BINDIR/wxantirevoke-check"
install -m 0755 "$SRC/launcher/wxantirevoke-mark"  "$BINDIR/wxantirevoke-mark"

echo "==> 写入桌面项 $APPDIR/wechat.desktop"
# 覆盖系统同名项：点"微信"图标启动的就是带补丁的版本
if [ -f "$APPDIR/wechat.desktop" ] && ! grep -q 'wechat-antirevoke' "$APPDIR/wechat.desktop"; then
    cp -n "$APPDIR/wechat.desktop" "$APPDIR/wechat.desktop.wxantirevoke-backup" 2>/dev/null || true
fi
cat > "$APPDIR/wechat.desktop" <<EOF
[Desktop Entry]
Name=wechat
Name[zh_CN]=微信
Exec=$BINDIR/wechat-antirevoke %U
StartupNotify=true
Terminal=false
Icon=/usr/share/icons/hicolor/512x512/apps/wechat.png
Type=Application
Categories=Utility;
Comment=Wechat Desktop (with wxantirevoke)
Comment[zh_CN]=微信桌面版（带防撤回）
EOF

command -v update-desktop-database >/dev/null 2>&1 && \
    update-desktop-database "$APPDIR" 2>/dev/null || true

echo
echo "==> 完成。自检："
"$BINDIR/wxantirevoke-check"
echo
echo "用法："
echo "  点桌面图标 / wechat-antirevoke          带防撤回启动"
echo "  WXANTIREVOKE=off wechat-antirevoke      本次不带补丁（对照排障）"
echo "  wechat-probe                            排障：补丁 + 探针一起加载"
echo "  卸载：$SRC/uninstall.sh"
