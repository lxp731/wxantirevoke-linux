/*
 * wxprobe - 观察 Linux 版微信"撤回"实际走了哪条代码路径
 * =========================================================
 * 手法：把要观察的地址首字节换成 0xCC (int3)，自装 SIGTRAP 处理器：
 *   命中 → 记录时间/寄存器 → 还原原字节 → RIP 指回该地址重新执行。
 *   程序语义完全不变，只是被"计数 + 采样"。
 *
 * 特性：
 *   - 每 1 秒重新布防（启动期的无关命中不会导致错过真正的撤回事件）
 *   - 每秒守护 SIGTRAP 处理器（防被微信自己的 crashpad 覆盖）
 *   - 布防延迟 10 秒，避开启动阶段处理器安装窗口
 *   - "标记"机制：touch $HOME/.local/state/wxantirevoke/mark
 *     就会在日志里打印一次所有探针的当前计数快照，用来精确圈定测试窗口
 *
 * 构建: gcc -shared -fPIC -O2 -o libwxprobe.so wxprobe.c -lpthread
 * 使用: WXANTIREVOKE_LIB=$HOME/.local/lib/wxantirevoke/libwxprobe.so wechat-antirevoke
 * 日志: $HOME/.local/state/wxantirevoke/probe.log
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <link.h>
#include <ucontext.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>

/* ------------------------- 探针表 ------------------------- */

struct probe {
    uint64_t addr;          /* VA = pie_base + addr */
    const char *name;
    uint32_t hits;
    uint32_t cap;
    uint8_t  saved;
    uint8_t  inited;        /* saved 是否已从内存里采样过 */
    volatile int armed;
};

static struct probe probes[] = {
    { 0x60df020, "wrapper:Message::ParseContent",  0, 3000, 0, 0, 0 },
    { 0x60df1e0, "parseRevokeXML(entry)",          0, 3000, 0, 0, 0 },
    { 0x60df42a, "  .has_revokemsg判",             0, 3000, 0, 0, 0 },
    { 0x60df432, "  .revoke分支",                  0, 3000, 0, 0, 0 },
    { 0x60df69e, "  .非revoke分支",                0, 3000, 0, 0, 0 },
    { 0x60df788, "  .newmsgid写入",                0, 3000, 0, 0, 0 },
    { 0x60dfe40, "fallback_handler",               0, 3000, 0, 0, 0 },
    { 0x60ecaa0, "revokemsg_xml_builder(发送侧)",  0, 3000, 0, 0, 0 },
    /* —— revokemessage 类型 —— */
    { 0x62b9ce0, "RevokeMessage_A(ctor)",          0, 3000, 0, 0, 0 },
    { 0x62beea0, "RevokeMessage_B",                0, 3000, 0, 0, 0 },
    { 0x62bf6c0, "RevokeMessage_C",                0, 3000, 0, 0, 0 },
    { 0x62bfb20, "RevokeMessage_D",                0, 3000, 0, 0, 0 },
    { 0x62c0000, "RevokeMessage_E",                0, 3000, 0, 0, 0 },
    /* —— revokebatchmessage 类型 —— */
    { 0x62c0720, "RevokeBatch_A",                  0, 3000, 0, 0, 0 },
    { 0x62c08e0, "RevokeBatch_B",                  0, 3000, 0, 0, 0 },
    { 0x62c0fd0, "RevokeBatch_C",                  0, 3000, 0, 0, 0 },
    { 0x62c1750, "RevokeBatch_D",                  0, 3000, 0, 0, 0 },
    { 0x62c1960, "RevokeBatch_E",                  0, 3000, 0, 0, 0 },
    { 0x62c1cc0, "RevokeBatch_F",                  0, 3000, 0, 0, 0 },
    { 0x62c21a0, "RevokeBatch_G",                  0, 3000, 0, 0, 0 },
    /* —— 其它 revoke 子系统 —— */
    { 0x65ff0f0, "is_revoke",                      0, 3000, 0, 0, 0 },
    { 0x6874f30, "revoke_time",                    0, 3000, 0, 0, 0 },
    { 0x6909fb0, "revokehistoryinjoinroommsg",     0, 3000, 0, 0, 0 },
    { 0x60ebda0, "reeditrevoke",                   0, 3000, 0, 0, 0 },
    { 0x8ddc1b0, "RevokeTask_fn",                  0, 3000, 0, 0, 0 },
    { 0x79b0360, "tab_tips_revoke_id",             0, 3000, 0, 0, 0 },
    { 0x79b0f10, "tab_tips_revoke_id(2)",          0, 3000, 0, 0, 0 },
    { 0x60e0030, "link_revoke/revoke-history",     0, 3000, 0, 0, 0 },
    { 0x60e65b0, "link_revoke/revoke-history(2)",  0, 3000, 0, 0, 0 },
    /* —— 撤回应用链（探针实测发现：build 提示XML → 按 newmsgid 查原消息 → 应用） —— */
    { 0x6adc230, "APPLY:RevokeApply_fn(entry)",    0, 3000, 0, 0, 0 },
    { 0x6adcc66, "APPLY:.build_tip_xml#1",         0, 3000, 0, 0, 0 },
    { 0x6adccf9, "APPLY:.lookup_by_newmsgid",      0, 3000, 0, 0, 0 },
    { 0x6adcd99, "APPLY:.guard test r13,r13",      0, 3000, 0, 0, 0 },
    { 0x6add29d, "APPLY:.guard_skip_target",       0, 3000, 0, 0, 0 },
    { 0x6add137, "APPLY:.guard_other_branch",      0, 3000, 0, 0, 0 },
    { 0x6addc1c, "APPLY:.build_tip_xml#2",         0, 3000, 0, 0, 0 },
    { 0x6514320, "APPLY:lookup_fn(entry)",         0, 3000, 0, 0, 0 },
    { 0x6524de0, "APPLY:wrapper2(entry)",          0, 3000, 0, 0, 0 },
    { 0x6acd550, "APPLY:chain 0x6acd550(entry)",   0, 3000, 0, 0, 0 },
    { 0x6ac86f0, "APPLY:chain 0x6ac86f0(entry)",   0, 3000, 0, 0, 0 },
    { 0x6ad6820, "APPLY:chain 0x6ad6820(entry)",   0, 3000, 0, 0, 0 },
    { 0x6ad5e50, "APPLY:chain 0x6ad5e50(entry)",   0, 3000, 0, 0, 0 },
};
#define NBUILTIN (sizeof(probes)/sizeof(probes[0]))

/* 运行时可用 WXPROBE_EXTRA="0x<vaddr>:名字,0x<vaddr>:名字" 追加探针（自测用） */
#define MAXEXTRA 16
static struct probe extra[MAXEXTRA];
static char extra_names[MAXEXTRA][80];
static size_t nextra;

static struct probe *g_plist[64];
static size_t g_pn;

static void build_plist(void)
{
    g_pn = 0;
    for (size_t i = 0; i < NBUILTIN && g_pn < 64; i++) g_plist[g_pn++] = &probes[i];
    for (size_t i = 0; i < nextra && g_pn < 64; i++)    g_plist[g_pn++] = &extra[i];
}

static void add_extra(const char *spec)
{
    if (nextra >= MAXEXTRA) return;
    while (*spec == ' ' || *spec == ',') spec++;
    if (!*spec) return;
    char *end = NULL;
    unsigned long long va = strtoull(spec, &end, 0);
    if (end == spec || *end != ':') return;
    const char *nm = end + 1;
    const char *e2 = nm;
    while (*e2 && *e2 != ',') e2++;
    if (e2 == nm) return;
    struct probe *p = &extra[nextra];
    memset(p, 0, sizeof(*p));
    p->addr = va;
    p->cap = 3000;
    size_t l = (size_t)(e2 - nm);
    if (l >= sizeof(extra_names[0])) l = sizeof(extra_names[0]) - 1;
    memcpy(extra_names[nextra], nm, l);
    extra_names[nextra][l] = 0;
    p->name = extra_names[nextra];
    nextra++;
}

static uintptr_t g_base;
static uintptr_t g_exec_lo, g_exec_hi;   /* 主程序可执行段的 VA 范围 */
static int g_fd = -1;
static char g_logpath[4096];
static char g_markpath[4096];

/* ------------------------ 信号安全日志 ------------------------ */

static char *put_str(char *p, const char *s) { while (*s) *p++ = *s++; return p; }

static char *put_hex(char *p, uint64_t v)
{
    char t[13]; t[12] = 0;
    for (int i = 11; i >= 0; i--) { int d = v & 0xf; t[i] = d < 10 ? '0' + d : 'a' + d - 10; v >>= 4; }
    return put_str(p, t);
}

static char *put_dec(char *p, uint64_t v)
{
    char t[21]; int i = 20; t[i] = 0;
    if (!v) t[--i] = '0';
    while (v) { t[--i] = '0' + (v % 10); v /= 10; }
    return put_str(p, t + i);
}

static char *put_ts(char *p)
{
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    p = put_dec(p, (uint64_t)ts.tv_sec); *p++ = '.';
    int ms = (int)(ts.tv_nsec / 1000000);
    for (int d = 100000; d >= 1; d /= 10) *p++ = '0' + (ms / d) % 10;
    return p;
}

static void emit(const char *b, size_t n) { if (g_fd >= 0) { ssize_t r = write(g_fd, b, n); (void)r; } }

static void log_hit(struct probe *pr, ucontext_t *uc)
{
    const greg_t *g = uc->uc_mcontext.gregs;
    char b[400], *p = b;
    p = put_ts(p);
    p = put_str(p, " HIT ");  p = put_str(p, pr->name);
    p = put_str(p, "  n=");   p = put_dec(p, pr->hits);
    if (pr->hits == 1) {
        p = put_str(p, "\n    rax="); p = put_hex(p, (uint64_t)g[REG_RAX]);
        p = put_str(p, " rbx=");      p = put_hex(p, (uint64_t)g[REG_RBX]);
        p = put_str(p, " rcx=");      p = put_hex(p, (uint64_t)g[REG_RCX]);
        p = put_str(p, " rdx=");      p = put_hex(p, (uint64_t)g[REG_RDX]);
        p = put_str(p, "\n    rsi="); p = put_hex(p, (uint64_t)g[REG_RSI]);
        p = put_str(p, " rdi=");      p = put_hex(p, (uint64_t)g[REG_RDI]);
        p = put_str(p, " rbp=");      p = put_hex(p, (uint64_t)g[REG_RBP]);
        p = put_str(p, " rsp=");      p = put_hex(p, (uint64_t)g[REG_RSP]);
        p = put_str(p, " tid=");      p = put_dec(p, (uint64_t)syscall(SYS_gettid));
    } else {
        p = put_str(p, "  rax="); p = put_hex(p, (uint64_t)g[REG_RAX]);
        p = put_str(p, " rdi=");  p = put_hex(p, (uint64_t)g[REG_RDI]);
        p = put_str(p, " rsi=");  p = put_hex(p, (uint64_t)g[REG_RSI]);
    }
    *p++ = '\n';
    emit(b, (size_t)(p - b));
}

/* ------------------------ 页面权限 ------------------------ */

static int page_rwx(void *q)
{
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t pg = (uintptr_t)q & ~((uintptr_t)ps - 1);
    return mprotect((void *)pg, (size_t)ps, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
}

static void page_rx(void *q)
{
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t pg = (uintptr_t)q & ~((uintptr_t)ps - 1);
    mprotect((void *)pg, (size_t)ps, PROT_READ | PROT_EXEC);
}

/* ------------------------ SIGTRAP ------------------------ */

static struct sigaction g_old;
static void on_trap(int sig, siginfo_t *si, void *uctx);

static struct sigaction our_sa(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_trap;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    return sa;
}

static void on_trap(int sig, siginfo_t *si, void *uctx)
{
    ucontext_t *uc = (ucontext_t *)uctx;
    uint64_t pc = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];

    if (pc > 0) {
        uint64_t a = pc - 1;
        for (size_t i = 0; i < g_pn; i++) {
            struct probe *pr = g_plist[i];
            /* 注意：pr->addr 是 VA，命中现场是运行时地址，必须加上基址再比 */
            if (!pr->armed || (g_base + pr->addr) != a) continue;

            pr->hits++;
            log_hit(pr, uc);

            uint8_t *q = (uint8_t *)(g_base + pr->addr);
            if (page_rwx(q)) {          /* r-x 页面直接写会 SIGSEGV！ */
                *q = pr->saved;
                page_rx(q);
            }
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)a;   /* 重新执行 */
            if (pr->hits >= pr->cap) pr->armed = 0;
            return;
        }
    }
    if (g_old.sa_flags & SA_SIGINFO) {
        if (g_old.sa_sigaction) g_old.sa_sigaction(sig, si, uctx);
    } else if (g_old.sa_handler != SIG_DFL && g_old.sa_handler != SIG_IGN) {
        g_old.sa_handler(sig);
    } else {
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

static int in_exec(uint64_t va);

/* ------------------------ 巡检线程 ------------------------ */

static void *watch_thread(void *arg)
{
    (void)arg;
    double delay = 10.0;
    const char *ds = getenv("WXPROBE_DELAY");
    if (ds) { double v = atof(ds); if (v >= 0) delay = v; }
    struct timespec t0 = { (time_t)delay, (long)((delay - (double)(time_t)delay) * 1e9) };
    nanosleep(&t0, NULL);

    char b[512], *p = b;
    p = put_ts(p); p = put_str(p, " === 开始布防 ===\n");
    emit(b, (size_t)(p - b));

    struct sigaction sa = our_sa();

    int first = 1;
    int tick = 0;
    while (1) {
        /* 1. 抢回 SIGTRAP（微信的 crashpad 可能在启动时装过自己的） */
        struct sigaction cur;
        if (sigaction(SIGTRAP, NULL, &cur) == 0 && cur.sa_sigaction != on_trap) {
            g_old = cur;
            sigaction(SIGTRAP, &sa, NULL);
        }
        /* 2. 首次采样 + 重新布防 */
        int armed = 0, skipped = 0;
        for (size_t i = 0; i < g_pn; i++) {
            struct probe *pr = g_plist[i];
            if (pr->hits >= pr->cap) { skipped++; continue; }
            if (!in_exec(pr->addr)) { pr->cap = 0; skipped++; continue; }
            uint8_t *q = (uint8_t *)(g_base + pr->addr);
            if (!pr->inited) {
                if (*q == 0xCC) { pr->cap = 0; skipped++; continue; }  /* 已经是断点，别动 */
                pr->saved = *q;
                pr->inited = 1;
            }
            if (*q == 0xCC) { pr->armed = 1; armed++; continue; }      /* 已布防 */
            if (*q != pr->saved) { skipped++; continue; }              /* 字节变了，不动 */
            if (page_rwx(q)) { *q = 0xCC; page_rx(q); pr->armed = 1; armed++; }
        }
        if (first) {
            first = 0;
            char b2[256], *r = b2;
            r = put_ts(r);
            r = put_str(r, " === 布防完毕: 已装 "); r = put_dec(r, (uint64_t)armed);
            r = put_str(r, " / 共 ");             r = put_dec(r, (uint64_t)g_pn);
            r = put_str(r, " 个探针 (跳过 ");       r = put_dec(r, (uint64_t)skipped);
            r = put_str(r, ") ===\n");
            emit(b2, (size_t)(r - b2));
        }
        /* 3. 标记文件 → 打印计数快照 */
        if (access(g_markpath, F_OK) == 0) {
            unlink(g_markpath);
            char m[4096], *r = m;
            r = put_ts(r); r = put_str(r, " === MARK 计数快照 ===\n");
            for (size_t i = 0; i < g_pn && (size_t)(r - m) < sizeof(m) - 100; i++) {
                r = put_str(r, "    "); r = put_dec(r, g_plist[i]->hits);
                r = put_str(r, "  ");   r = put_str(r, g_plist[i]->name);
                r = put_str(r, "\n");
            }
            emit(m, (size_t)(r - m));
        }
        /* 4. 每 30 秒一条心跳（带计数），确保能看到活性 */
        if (++tick % 30 == 0) {
            char h[1024], *r = h;
            r = put_ts(r); r = put_str(r, " --- 心跳 ");
            r = put_str(r, "armed="); r = put_dec(r, (uint64_t)armed);
            unsigned long total = 0;
            for (size_t i = 0; i < g_pn; i++) total += g_plist[i]->hits;
            r = put_str(r, " 总命中="); r = put_dec(r, total);
            r = put_str(r, " ---\n");
            emit(h, (size_t)(r - h));
        }
        struct timespec it = { 1, 0 };
        nanosleep(&it, NULL);
    }
    return NULL;
}

/* ------------------------ 入口 ------------------------ */

static int phdr_cb(struct dl_phdr_info *info, size_t sz, void *d)
{
    (void)sz;
    if (info->dlpi_name == NULL || info->dlpi_name[0] == 0) {
        for (int i = 0; i < info->dlpi_phnum; i++) {
            const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
            if (ph->p_type != PT_LOAD) continue;
            if (ph->p_flags & PF_X) {
                g_exec_lo = ph->p_vaddr;
                g_exec_hi = ph->p_vaddr + ph->p_memsz;
            }
            if (d) *(uintptr_t *)d = (uintptr_t)info->dlpi_addr;
        }
        return 1;
    }
    return 0;
}

static int in_exec(uint64_t va) { return va >= g_exec_lo && va < g_exec_hi; }

__attribute__((constructor))
static void wxprobe_init(void)
{
    char path[4096];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) return;
    path[n] = 0;
    const char *b = strrchr(path, '/');
    if (strcmp(b ? b + 1 : path, "wechat") != 0) return;
    if (getenv("WXPROBE") && strcmp(getenv("WXPROBE"), "off") == 0) return;

    uintptr_t base = 0;
    dl_iterate_phdr(phdr_cb, &base);
    g_base = base;

    const char *ext = getenv("WXPROBE_EXTRA");
    if (ext) {
        char tmp[1024];
        snprintf(tmp, sizeof(tmp), "%s", ext);
        char *save = NULL, *tok = strtok_r(tmp, ",", &save);
        while (tok) { add_extra(tok); tok = strtok_r(NULL, ",", &save); }
    }
    build_plist();

    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    const char *lp = getenv("WXPROBE_LOG");
    if (!lp) { snprintf(g_logpath, sizeof(g_logpath), "%s/.local/state/wxantirevoke/probe.log", home); lp = g_logpath; }
    snprintf(g_markpath, sizeof(g_markpath), "%s/.local/state/wxantirevoke/mark", home);

    g_fd = open(lp, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (g_fd < 0) return;

    struct sigaction sa = our_sa();
    sigaction(SIGTRAP, &sa, &g_old);

    /* 地址越界（微信升级后）的探针直接剔除，并记录下来 */
    int bad = 0;
    for (size_t i = 0; i < g_pn; i++)
        if (!in_exec(g_plist[i]->addr)) { g_plist[i]->cap = 0; g_plist[i]->armed = 0; bad++; }

    pthread_t th;
    int ok = (pthread_create(&th, NULL, watch_thread, NULL) == 0);
    if (ok) pthread_detach(th);

    char buf[512], *p = buf;
    p = put_ts(p);
    p = put_str(p, " === wxprobe loaded base="); p = put_hex(p, base);
    p = put_str(p, " probes=");                  p = put_dec(p, (uint64_t)g_pn);
    p = put_str(p, " 越界=");                    p = put_dec(p, (uint64_t)bad);
    p = put_str(p, " thread=");                  p = put_dec(p, (uint64_t)ok);
    p = put_str(p, " log=");                     p = put_str(p, lp);
    p = put_str(p, "\n");
    emit(buf, (size_t)(p - buf));
}
