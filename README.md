# wxantirevoke-linux

Linux 版微信（腾讯官方 Universal 版）**防撤回**。

> 仓库：<https://github.com/lxp731/wxantirevoke-linux>

做法：`LD_PRELOAD` 一个库，在微信主程序加载后、`main()` 之前改掉内存里的
**一条指令**。**不修改 `/opt/wechat` 下任何文件**，卸载 = 不再加载这个库。

- 适用版本：官方微信 **4.1.13.26（x86_64）**
- 依赖：`gcc`（只在安装时用来编译库）
- 不需要 root

---

## 效果

别人撤回消息后：

| | 结果 |
|---|---|
| 原消息（文字/图片/引用…） | ✅ **保留**，照常显示 |
| "xxx 撤回了一条消息" 提示 | 仍然出现（不隐藏） |
| 手机 / 群成员 / 网页版 | 照常显示"已撤回"（补丁只影响本机这个客户端） |

即等价于 macOS 版 [`wechat-antirecall`](https://github.com/fzlzjerry/wechat-antirecall) 的
`revoke-tip` 模式。想连提示也不要（静默模式）见 [docs/原理.md](docs/原理.md#静默模式可选未实现)。

---

## 第 0 步：你的微信是哪种安装方式？

本工具是**按二进制打补丁**的，所以只跟 `/opt/wechat/wechat` 这个文件有关，
跟用哪个发行版/包管理器装的无所谓 —— 只要是同一个官方构建即可。

| 安装方式 | 主程序路径 | 支持情况 |
|---|---|---|
| Arch / CachyOS：AUR `wechat-bin` | `/opt/wechat/wechat` | ✅ 已实测（4.1.13.26） |
| Debian / Ubuntu：官方 `.deb` | `/opt/wechat/wechat` | ✅ 同一份二进制，可直接用（见下） |
| Arch：AUR `wechat-universal-bwrap`（bwrap 沙箱） | `/opt/wechat/wechat` | ⚠️ 理论可用，需把库带进沙箱，未验证 |
| Flatpak（社区打包，如 `wechat-universal-flatpak`） | 沙箱内 `/app/...` | ⚠️ 需往沙箱里放库并加 `LD_PRELOAD`，未验证 |

先确认文件对不对（**这一步很关键**）：

```bash
ls -l /opt/wechat/wechat
sha256sum /opt/wechat/wechat
# 已知的 4.1.13.26 x86_64：
# 2749535f299ba67feb8ef80cab898bb0531fa8f376b16f8a72db706811c84132
```

- 哈希一致 → 直接照本文档装，开箱可用
- 哈希不一致（别的版本 / arm64 / LoongArch）→ 补丁点不同，本库会**自动跳过不写入**
  （不会改坏程序），需要按 [docs/重新定位补丁点.md](docs/重新定位补丁点.md) 重新定位

---

## 第 1 步：安装微信本身（已装可跳过）

### Arch / CachyOS（AUR）

```bash
yay -S wechat-bin          # 或 paru -S wechat-bin
```

### Debian / Ubuntu（官方 deb）

腾讯官方只提供 `.deb`，amd64 / arm64 / LoongArch 三个架构各一个包。
**本文档验证的是 amd64 版**。

```bash
# 1) 下载官方 deb（约 750MB）
#    也可以从 https://linux.weixin.qq.com/ 页面下载
wget https://dldir1v6.qq.com/weixin/Universal/Linux/WeChatLinux_x86_64.deb

# 2) 安装（apt 会自己处理依赖）
sudo apt install ./WeChatLinux_x86_64.deb

#    如果你的系统不支持 `apt install ./xxx.deb`，等价写法：
#    sudo dpkg -i WeChatLinux_x86_64.deb
#    sudo apt -f install          # 补齐缺失依赖

# 3) 验证
which wechat                     # /usr/bin/wechat
sha256sum /opt/wechat/wechat     # 4.1.13.26 应为 2749535f…c84132
```

包信息（从 deb 的 control 里读出来的，供核对）：

```
Package: wechat          Version: 4.1.13.26     Architecture: amd64
Depends: fonts-noto-cjk | google-noto-cjk-fonts
Installed-Size: 约 750MB
postinst: ln -snf /opt/wechat/wechat /usr/bin/wechat
```

也就是说：装完就是 `/opt/wechat/wechat` + `/usr/bin/wechat` 软链，
和 Arch 的 `wechat-bin` 完全一样的布局（AUR 包本身就是解包这个 deb 重打包的）。

> 如果启动报缺 `.so`：deb 只声明了中文字体依赖，其余系统库要自己补齐。
> 可以拿 AUR 包的依赖列表当参考
> （`nss` / `libxcb-*` / `libxkbcommon*` / `libxcomposite` / `libxdamage` /
> `libxrandr` / `alsa-lib` / `libpulse` / `libdrm` / `mesa` / `libglvnd` /
> `cairo` / `pango` / `freetype2` / `fontconfig` / `expat` / `zlib` / `glib2` …，
> 对应的 Debian 包名按报错补装即可）。
>
> 卸载微信：`sudo apt remove wechat`（postrm 会清掉 `/usr/bin/wechat`）。

### 其他发行版

官方没有 rpm / 官方 Flatpak。要么用第三方打包（Fedora 有若干 COPR、
社区 flatpak），要么用 `alien` 把 deb 转 rpm。
无论哪种，只要最终主程序是上面那个哈希的 `/opt/wechat/wechat`，本文档一样适用。

---

## 第 2 步：安装本工具

```bash
git clone https://github.com/lxp731/wxantirevoke-linux.git
cd wxantirevoke-linux
./install.sh
```

只想拿最新一份、不要历史：

```bash
git clone --depth 1 https://github.com/lxp731/wxantirevoke-linux.git
```

没装 git 的话，直接下载仓库 zip 解压，或在 GitHub 页面上按 <kbd>Code → Download ZIP</kbd>，
进目录跑 `./install.sh` 效果一样（`.so` 是脚本里现编译的，所以仓库里不含二进制）。

想装到别的位置就换 `git clone` 的目标路径，其余步骤不变。

装完的东西（都在 `$HOME` 下，不需要 sudo）：

| 路径 | 作用 |
|---|---|
| `~/.local/lib/wxantirevoke/libwxantirevoke.so` | 补丁库 |
| `~/.local/lib/wxantirevoke/libwxprobe.so` | 探针库（排障用） |
| `~/.local/bin/wechat-antirevoke` | 启动器（带补丁） |
| `~/.local/bin/wechat-probe` | 启动器（补丁 + 探针，排障用） |
| `~/.local/bin/wxantirevoke-check` | 补丁点自检（不启动微信） |
| `~/.local/bin/wxantirevoke-mark` | 给探针日志打计数快照 |
| `~/.local/share/applications/wechat.desktop` | 桌面项覆盖 → **点"微信"图标即带补丁** |

安装脚本最后会自动跑一次自检，应看到：

```
  [可用] newmsgid(0x60df783)  偏移 0x60de783  原始字节 = e8b89e2d01
```

桌面项覆盖是 XDG 标准行为（`~/.local/share/applications` 优先于
`/usr/share/applications`），Debian/Ubuntu 的 GNOME / KDE 同样生效。

## 使用

```bash
wechat-antirevoke                      # 带防撤回启动（或直接点桌面图标）
WXANTIREVOKE=off wechat-antirevoke     # 本次不带补丁（对照排障）
WXANTIREVOKE_VERBOSE=1 wechat-antirevoke   # 打印补丁细节
```

> 单实例机制：**必须完全退出微信再启动**，否则只是聚焦已有窗口。

确认生效：

```bash
grep wxantirevoke ~/.local/state/wxantirevoke/wechat.log
# 防撤回已生效 (构建: wechat-bin 4.1.13.26 (deb build 2025-10), 本次写入 1 处, ...)
```

## 卸载

```bash
./uninstall.sh
```

微信立即恢复原版；`/opt/wechat` 自始至终没被改过。

## 微信升级后

补丁点会随版本变化。升级后先自检：

```bash
wxantirevoke-check
```

- `[可用]` → 照常用
- `[失效]` → 补丁点变了。此时**不会改坏程序**（库写入前逐字节校验，不匹配只报错、不写入），
  只是防撤回暂时失效。重新定位的方法见 [docs/重新定位补丁点.md](docs/重新定位补丁点.md)。

---

## 文档

| 文档 | 内容 |
|---|---|
| [docs/原理.md](docs/原理.md) | 微信撤回的处理链、补丁点、为什么这样改、效果与限制 |
| [docs/排查记录.md](docs/排查记录.md) | 三次尝试的完整记录（v1/v2 为什么空转、v3 为什么成功）、探针实测日志 |
| [docs/探针-wxprobe.md](docs/探针-wxprobe.md) | 运行时探针工具：原理、用法、探针点表、开发时踩过的坑 |
| [docs/重新定位补丁点.md](docs/重新定位补丁点.md) | 微信升级后如何重新找到补丁点 |

## 安全性

1. 写入前**逐字节校验**原始内容，不匹配只报错、绝不写入 → 升级后只会失效，不会改坏程序
2. 只对 `/proc/self/exe` basename 为 `wechat` 的进程生效；`crashpad_handler`、
   `RadiumWMPF/WeChatAppEx` 等子进程自动跳过
3. 只 `mprotect` 目标那一页，写完立刻恢复 `r-x`，并回读校验
4. 不落盘：`pacman -Qkk` / `dpkg --verify wechat` 都不会报文件被改
5. 开关：`WXANTIREVOKE=off`，或直接不用启动器

## 仓库结构

```
wxantirevoke.c         补丁库源码（LD_PRELOAD）
probe/wxprobe.c        探针库源码（int3 + SIGTRAP，诊断用）
launcher/              wechat-antirevoke / wechat-probe / wxantirevoke-check / wxantirevoke-mark
install.sh            编译 + 安装（gcc，无需 root）
uninstall.sh          完全卸载
docs/                 原理 / 排查记录 / 探针工具 / 重新定位补丁点
```

`*.so` 是 `install.sh` 里现编译的，已在 `.gitignore` 中，不入库。
