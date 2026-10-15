#!/bin/sh
# 完全卸载 wxantirevoke（不触碰 /opt/wechat，也不动微信的使用数据）
set -eu

BINDIR="$HOME/.local/bin"
APPDIR="$HOME/.local/share/applications"

rm -f "$HOME/.local/lib/wxantirevoke/libwxantirevoke.so" \
      "$HOME/.local/lib/wxantirevoke/libwxprobe.so"
rmdir "$HOME/.local/lib/wxantirevoke" 2>/dev/null || true

rm -f "$BINDIR/wechat-antirevoke" "$BINDIR/wechat-probe" \
      "$BINDIR/wxantirevoke-check" "$BINDIR/wxantirevoke-mark"

if [ -f "$APPDIR/wechat.desktop.wxantirevoke-backup" ]; then
    mv "$APPDIR/wechat.desktop.wxantirevoke-backup" "$APPDIR/wechat.desktop"
else
    rm -f "$APPDIR/wechat.desktop"     # 系统同名项会自动重新生效
fi

command -v update-desktop-database >/dev/null 2>&1 && \
    update-desktop-database "$APPDIR" 2>/dev/null || true

echo "已卸载。微信恢复为原版（/opt/wechat 自始至终未被修改）。"
echo "日志/状态目录（可选删除）: ${XDG_STATE_HOME:-$HOME/.local/state}/wxantirevoke"
