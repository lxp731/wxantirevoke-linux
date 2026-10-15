/*
 * wxantirevoke - Linux 版微信防撤回（LD_PRELOAD 内存补丁，不改磁盘文件）
 * =====================================================================
 *
 * 原理
 *   微信 PC 4.x 的撤回逻辑：收到撤回后，客户端会
 *     (1) 解析出被撤回消息的 newmsgid；
 *     (2) 构造一条"撤回提示"的 sysmsg（type="revokemsg"）；
 *     (3) 按 newmsgid 找到原消息，把它替换成撤回提示。
 *
 *   本库在内存里把第 (3) 步掐掉：让"按 newmsgid 查原消息"恒失败，
 *   原消息因而保留（静默防撤回，不显示"xx 撤回了一条消息"）。
 *   磁盘上的 /opt/wechat/wechat 完全不动，卸载 = 不加载这个库。
 *
 * 目标（wechat-bin 4.1.13.26 / /opt/wechat/wechat）
 *
 *   探针实测的链路（wxprobe）：
 *     parseRevokeXML(0x60df1e0) 解析出 <newmsgid> 存进消息对象 +0x1c8
 *       └─ 0x6adc230 APPLY: 用 [obj+0x1c8] 构造撤回提示消息(0x60ecaa0)
 *          └─ 0x6514320 按 newmsgid 查原消息 → 命中则替换
 *
 *   我们不碰任何分支（前两次改分支都证明是空转：那条 `has("revokemsg")`
 *   分支从未走到，而 APPLY 里的 guard 在这个事件里本来就走 6add29d），
 *   而是让解析出的 newmsgid 恒为 0 —— 原消息便无法被匹配/覆盖，
 *   而撤回提示照常插入。等价于 macOS 版 wechat-antirecall 的 revoke-tip
 *   补丁（str xzr）与 Windows 版 4.1.7+ 的 mov→sub。
 *
 * 安全性
 *   - 只对本进程（可执行文件名 == wechat）生效，子进程自动跳过
 *   - 写入前逐字节校验原始内容，不匹配就只报错、绝不写入
 *     => 微信升级后本库只会"失效"，不会把程序改坏
 *   - 只 mprotect 目标所在的那一页，写完立刻恢复 r-x
 *   - 不匹配任何已知版本时，可通过 WXANTIREVOKE=off 关闭
 *
 * 构建:  gcc -shared -fPIC -O2 -o libwxantirevoke.so wxantirevoke.c
 * 使用:  LD_PRELOAD=/path/libwxantirevoke.so /opt/wechat/wechat
 * 调试:  WXANTIREVOKE_VERBOSE=1 LD_PRELOAD=... /opt/wechat/wechat
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <link.h>
#include <sys/mman.h>
#include <errno.h>

/* ------------------------------------------------------------------ */
/* 补丁点表：每个条目都必须字节级核对，核对不过就跳过                   */
/* ------------------------------------------------------------------ */

#define PATCH_SITE_NEWMSGID 0x60df783UL  /* 把解析出的 newmsgid 取成 0 */

struct patch_point {
    const char *name;
    uint64_t vaddr;          /* PIE base + vaddr */
    const char *expected;    /* 期望的原始字节（hex） */
    const char *replacement; /* 替换字节（长度必须与 expected 相同） */
};

struct known_build {
    const char *label;
    const struct patch_point *points;
    size_t npoints;
};

static const struct patch_point pts_4_1_13_26[] = {
    /* 把"从 sysmsg 解析出的 newmsgid"强制取成 0。
     * 0x60df783 原本是:  call 0x73b9640   ; node -> uint64（取 <newmsgid> 的值）
     * 改成:              xor eax,eax ; nop x3  => rax = 0
     * 紧随其后的 `mov [rbx+0x1c8], rax` 就把 newmsgid 写成 0。
     *
     * 这正是 macOS 版 wechat-antirecall 的 "revoke-tip" 补丁
     * （把 str x0,[x19,#newmsgid] 改成 str xzr），其文档结论：
     *   "把 newmsgid 写零 —— 微信因此保留原消息但仍显示撤回提示"
     * Windows 版对 4.1.7+ 用的 `mov [rsi+disp],rax -> sub [rsi+disp],rax`
     * 是同一思路的另一种做法。
     */
    { "newmsgid(0x60df783) -> force 0",
      PATCH_SITE_NEWMSGID,
      "E8 B8 9E 2D 01",
      "31 C0 90 90 90" },
};

static const struct known_build known_builds[] = {
    { "wechat-bin 4.1.13.26 (deb build 2025-10)", pts_4_1_13_26,
      sizeof(pts_4_1_13_26) / sizeof(pts_4_1_13_26[0]) },
};

#define NBUILDS (sizeof(known_builds) / sizeof(known_builds[0]))

/* ------------------------------------------------------------------ */

static int g_verbose;

#define LOG(...) do { if (g_verbose) fprintf(stderr, "[wxantirevoke] " __VA_ARGS__); } while (0)
#define INFO(...) fprintf(stderr, "[wxantirevoke] " __VA_ARGS__)
#define ERR(...)  fprintf(stderr, "[wxantirevoke][!] " __VA_ARGS__)

static ssize_t parse_hex(const char *s, uint8_t *out, size_t outsz)
{
    size_t n = 0;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (n >= outsz || !s[1]) return -1;
        char b[3] = { s[0], s[1], 0 }, *end = NULL;
        long v = strtol(b, &end, 16);
        if (end != b + 2) return -1;
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return (ssize_t)n;
}

static void exe_basename(char *buf, size_t sz)
{
    char path[4096];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) { snprintf(buf, sz, "?"); return; }
    path[n] = 0;
    const char *p = strrchr(path, '/');
    snprintf(buf, sz, "%.*s", (int)(sz - 1), p ? p + 1 : path);
}

static void hexdump_line(const char *tag, const uint8_t *p, size_t n)
{
    fprintf(stderr, "[wxantirevoke] %s:", tag);
    for (size_t i = 0; i < n; i++) fprintf(stderr, " %02X", p[i]);
    fprintf(stderr, "\n");
}

struct find_ctx { uintptr_t base; int found; };

static int dl_cb(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    struct find_ctx *ctx = data;
    if (info->dlpi_name == NULL || info->dlpi_name[0] == '\0') {
        for (int i = 0; i < info->dlpi_phnum; i++) {
            if (info->dlpi_phdr[i].p_type == PT_LOAD) {
                ctx->base  = (uintptr_t)info->dlpi_addr;
                ctx->found = 1;
                return 1;
            }
        }
    }
    return 0;
}

static uintptr_t main_base(void)
{
    struct find_ctx ctx = { 0, 0 };
    dl_iterate_phdr(dl_cb, &ctx);
    return ctx.found ? ctx.base : 0;
}

static int rw_page(uintptr_t addr)
{
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t page = addr & ~((uintptr_t)ps - 1);
    if (mprotect((void *)page, (size_t)ps, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        ERR("mprotect(%p, RWX) 失败: %s\n", (void *)page, strerror(errno));
        return 0;
    }
    return 1;
}

static void rx_page(uintptr_t addr)
{
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t page = addr & ~((uintptr_t)ps - 1);
    mprotect((void *)page, (size_t)ps, PROT_READ | PROT_EXEC);
}

enum { SITE_SKIP = 0, SITE_PATCHED, SITE_ALREADY, SITE_MISMATCH };

static int apply_point(const struct patch_point *pp, uintptr_t base)
{
    uint8_t exp[64], rep[64];
    ssize_t ne = parse_hex(pp->expected, exp, sizeof(exp));
    ssize_t nr = parse_hex(pp->replacement, rep, sizeof(rep));
    if (ne <= 0 || ne != nr) {
        ERR("%s: 补丁定义非法\n", pp->name);
        return SITE_SKIP;
    }

    uint8_t *p = (uint8_t *)(base + pp->vaddr);
    if (memcmp(p, rep, (size_t)nr) == 0) return SITE_ALREADY;
    if (memcmp(p, exp, (size_t)ne) != 0) {
        if (g_verbose) {
            hexdump_line("实际字节", p, (size_t)(ne > 16 ? 16 : ne));
            hexdump_line("期望字节", exp, (size_t)ne);
            fprintf(stderr, "[wxantirevoke] 目标地址 = base(%p) + %#lx = %p\n",
                    (void *)base, (unsigned long)pp->vaddr, (void *)p);
        }
        return SITE_MISMATCH;
    }

    if (g_verbose) {
        hexdump_line("写入前", p, (size_t)ne);
    }
    if (!rw_page((uintptr_t)p)) return SITE_SKIP;
    memcpy(p, rep, (size_t)nr);
    rx_page((uintptr_t)p);

    /* 回读校验：确认内存里的字节真的变成了替换值 */
    if (memcmp(p, rep, (size_t)nr) != 0) {
        ERR("%s: 写入后回读校验失败！\n", pp->name);
        return SITE_SKIP;
    }
    if (g_verbose) {
        hexdump_line("写入后", p, (size_t)nr);
    }
    return SITE_PATCHED;
}

__attribute__((constructor))
static void wxantirevoke_init(void)
{
    const char *env = getenv("WXANTIREVOKE");
    if (env && (strcmp(env, "off") == 0 || strcmp(env, "0") == 0)) return;

    g_verbose = getenv("WXANTIREVOKE_VERBOSE") != NULL;

    char exe[256];
    exe_basename(exe, sizeof(exe));
    if (strcmp(exe, "wechat") != 0) {           /* RadiumWMPF / crashpad 等子进程 */
        LOG("非微信主程序 (%s)，跳过\n", exe);
        return;
    }

    uintptr_t base = main_base();
    if (!base) { ERR("找不到主程序基址，跳过\n"); return; }
    LOG("exe=%s  pie_base=%p\n", exe, (void *)base);

    /* 逐个已知版本尝试：靠"目标字节是否匹配"来识别，无需版本号配置 */
    int matched_build = -1, patched = 0, mismatch = 0, already = 0;

    for (size_t b = 0; b < NBUILDS; b++) {
        const struct known_build *kb = &known_builds[b];
        int bad = 0, have = 0;

        for (size_t i = 0; i < kb->npoints; i++) {
            switch (apply_point(&kb->points[i], base)) {
            case SITE_PATCHED:  patched++;   have = 1; break;
            case SITE_ALREADY:  already++;   have = 1; break;
            case SITE_MISMATCH: bad++;   mismatch++;  break;
            default:            bad++;                break;
            }
        }
        if (have && bad == 0) { matched_build = (int)b; break; }
    }

    if (matched_build >= 0) {
        INFO("防撤回已生效 (构建: %s, 本次写入 %d 处, 已是补丁状态 %d 处)\n",
             known_builds[matched_build].label, patched, already);
    } else if (mismatch) {
        ERR("字节校验不匹配 —— 微信版本已变化，本次未做任何修改\n");
        ERR("  防撤回失效，但程序不会被改坏。请更新补丁点表后重试。\n");
        ERR("  （排障：WXANTIREVOKE_VERBOSE=1 查看细节）\n");
    } else {
        LOG("没有可用的补丁点，跳过\n");
    }
}
