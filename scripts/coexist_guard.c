/*
 * coexist_guard — DCMI per-device coexistence guard for the pearl miner.
 *
 * Lets the miner share Ascend dies with other tenants (vLLM, training, ...) without the crash you
 * get from two processes running kernels on one die at once. The guard owns the "who's on the NPU"
 * decision; the miner is a passive endpoint that pauses/resumes on signal (see the coexist block in
 * src/miner.c). Run it ALONGSIDE the miners, in the SAME container (launch.sh starts it for you).
 *
 * WHY DCMI AND NOT fanotify (learned the hard way on bz-ascend, kernel 6.6):
 *   - Ascend compute processes hold /dev/davinci_manager open, NOT /dev/davinci<N>, so a per-device
 *     fanotify mark never fires for a tenant.
 *   - The manager char-device emits NO fsnotify open events at all (inode/mount/fs marks: 0 events
 *     while a real open succeeds), so fanotify can't catch tenant startup either.
 *   - Each container's /dev is a separate tmpfs, so fanotify marks don't cross namespaces anyway.
 *   DCMI (the driver's per-device compute-process table, what npu-smi is built on) is the only
 *   reliable signal. It lists every process holding device memory (so it catches an idle-but-loaded
 *   vLLM, not just an active one) and TRANSLATES pids into the caller's pid namespace.
 *
 * SELF-DISCOVERY: per poll, per device, we read the DCMI process list and classify each entry:
 *   - pid > 0 AND /proc/<pid>/comm matches the miner regex (default "ascend_prl")  -> OUR miner
 *   - anything else (a tenant's pid, or a pid 0 == a process in another namespace we can't see)
 *     -> FOREIGN.
 * If a die has a foreign tenant, pause its miner(s) (SIGUSR1, wait for ACK or -t timeout); when the
 * die is the miner's alone again, resume (SIGUSR2). No pids to wire: the miner can even restart and
 * the guard re-finds it. Because an unidentifiable (other-namespace) process counts as foreign, the
 * guard yields correctly even to a vLLM in a different container; running the miner container with
 * --pid=host additionally lets the guard NAME that pid in its logs (optional, not required).
 *
 * Tradeoff vs the (impossible) fanotify path: detection is poll-latency, not instant-at-open. Keep
 * -p small (default 500ms); a tenant shows up in DCMI at its init (memory alloc), before it runs
 * inference kernels, so the practical race is small. The airtight fix is a cooperative signal from
 * the tenant (e.g. a vLLM hook doing `kill -USR1 <miner>`), which this SIGUSR1 interface supports.
 *
 * Build: gcc -O2 -I/usr/local/dcmi -o coexist_guard coexist_guard.c -L/usr/local/dcmi -ldcmi \
 *            -Wl,-rpath,/usr/local/dcmi
 * Run:   ./coexist_guard [-p poll_ms] [-t ack_s] [-m miner_regex] [-n notify_cmd] <dev>...
 */
#define _GNU_SOURCE
#include <errno.h>
#include <regex.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "dcmi_interface_api.h"

#define MAXDEV  16
#define MAXPROC 256
#define MAXMINE 16

static volatile sig_atomic_t g_ack = 0;
static void on_ack(int s) { (void)s; g_ack = 1; }       /* miner -> guard: "I'm quiescent" */

static const char *g_notify = 0;
static int g_ack_timeout_ms = 8000;
static regex_t g_minere;

struct managed { int logic, card, device, paused; };

static void notify(const char *state, struct managed *m) {
    if (!g_notify) return;
    char cmd[600];
    snprintf(cmd, sizeof cmd, "%s %s %d", g_notify, state, m->logic);
    int rc = system(cmd); (void)rc;
}

/* is this pid one of our miners? (pid>0 and its comm matches the miner regex) */
static int is_miner(int pid) {
    if (pid <= 0) return 0;
    char p[64], comm[64] = "";
    snprintf(p, sizeof p, "/proc/%d/comm", pid);
    FILE *f = fopen(p, "r");
    if (!f) return 0;                                   /* can't read -> not confirmed ours -> foreign */
    if (fgets(comm, sizeof comm, f)) comm[strcspn(comm, "\n")] = 0;
    fclose(f);
    return regexec(&g_minere, comm, 0, 0, 0) == 0;
}

/* fill miner pids on the device into mp[] (<=cap), return foreign-process count. -1 on query error. */
static int scan_device(struct managed *m, int *mp, int cap, int *nminer) {
    struct dcmi_proc_mem_info procs[MAXPROC];
    int n = MAXPROC;
    if (dcmi_get_device_resource_info(m->card, m->device, procs, &n) != 0) return -1;
    int foreign = 0; *nminer = 0;
    for (int i = 0; i < n; i++) {
        if (is_miner(procs[i].proc_id)) { if (*nminer < cap) mp[(*nminer)++] = procs[i].proc_id; }
        else foreign++;                                  /* tenant pid, or pid 0 (other namespace) */
    }
    return foreign;
}

static void pause_dev(struct managed *m, int *mp, int nminer) {
    if (m->paused) return;
    g_ack = 0;
    for (int i = 0; i < nminer; i++)
        if (kill(mp[i], SIGUSR1)) fprintf(stderr, "[guard] dev%d signal %d: %s\n", m->logic, mp[i], strerror(errno));
    int waited = 0; struct timespec ts = { 0, 20L * 1000 * 1000 };
    while (!g_ack && waited < g_ack_timeout_ms) { nanosleep(&ts, 0); waited += 20; }
    printf("[guard] dev%d PAUSE %d miner(s) (%s, %dms)\n",
           m->logic, nminer, g_ack ? "ACK" : "no-ack/timeout", waited);
    m->paused = 1; notify("pause", m);
}
static void resume_dev(struct managed *m, int *mp, int nminer) {
    if (!m->paused) return;
    for (int i = 0; i < nminer; i++) kill(mp[i], SIGUSR2);
    printf("[guard] dev%d RESUME %d miner(s)\n", m->logic, nminer);
    m->paused = 0; notify("resume", m);
}

int main(int argc, char **argv) {
    setvbuf(stdout, 0, _IOLBF, 0);
    int poll_ms = 500;
    const char *minre = "ascend_prl";
    struct managed mg[MAXDEV]; int nmg = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) poll_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) g_ack_timeout_ms = atoi(argv[++i]) * 1000;
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) minre = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) g_notify = argv[++i];
        else {
            if (nmg >= MAXDEV) { fprintf(stderr, "[guard] too many devices\n"); return 1; }
            mg[nmg].logic = atoi(argv[i]); mg[nmg].paused = 0; nmg++;
        }
    }
    if (nmg == 0) {
        fprintf(stderr, "usage: %s [-p poll_ms] [-t ack_s] [-m miner_regex] [-n notify_cmd] <dev>...\n"
                        "  Poll DCMI per device; pause the miner (SIGUSR1) when a foreign tenant appears,\n"
                        "  resume (SIGUSR2) when the die is the miner's alone again. Miners are found by\n"
                        "  comm matching <miner_regex> (default ascend_prl). Run in the miners' container.\n", argv[0]);
        return 1;
    }
    if (regcomp(&g_minere, minre, REG_EXTENDED | REG_NOSUB)) { fprintf(stderr, "[guard] bad -m regex\n"); return 1; }
    if (dcmi_init() != 0) { fprintf(stderr, "[guard] dcmi_init failed (need driver access)\n"); return 2; }
    for (int i = 0; i < nmg; i++) {
        if (dcmi_get_card_id_device_id_from_logicid(&mg[i].card, &mg[i].device, (unsigned)mg[i].logic) != 0) {
            fprintf(stderr, "[guard] davinci%d -> card/device map failed\n", mg[i].logic); return 2;
        }
        printf("[guard] managing davinci%d (card %d dev %d)\n", mg[i].logic, mg[i].card, mg[i].device);
    }
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_ack; sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, 0);
    signal(SIGPIPE, SIG_IGN);
    printf("[guard] poll %dms, ack timeout %ds, miner=/%s/, %d device(s)\n",
           poll_ms, g_ack_timeout_ms / 1000, minre, nmg);

    struct timespec pt = { poll_ms / 1000, (long)(poll_ms % 1000) * 1000000 };
    for (;;) {
        for (int i = 0; i < nmg; i++) {
            int mp[MAXMINE], nminer = 0;
            int f = scan_device(&mg[i], mp, MAXMINE, &nminer);
            if (f < 0) continue;                          /* transient DCMI error: hold state */
            if (f > 0 && nminer > 0) pause_dev(&mg[i], mp, nminer);
            else if (f == 0) resume_dev(&mg[i], mp, nminer);
        }
        nanosleep(&pt, 0);
    }
    return 0;
}
