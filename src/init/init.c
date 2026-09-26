// /sbin/init: PID 1. Mount things, load the GPU driver, configure the network,
// then keep q27-init alive on the console. The model is semantic PID 1; this
// is the part that must not crash. Built static; no userland needed.
//
// As /sbin/rescue (init=/sbin/rescue) it mounts the same trees and offers a
// three-item menu instead of starting the model.
//
// Kernel cmdline, all optional:
//   potemkin.disk=/dev/vda        persistent disk (ext4 or btrfs), default /dev/vda
//   potemkin.tty=/dev/tty1        console for q27-init
//   potemkin.net=A.B.C.D/N,GW,DNS static address for the first non-lo interface
//   llm= model= api_key= api_url= read by q27-init itself
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/reboot.h>
#include <net/if.h>
#include <net/route.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static char cmdline[4096];
static int con = -1;  // kernel console for our own messages

static void say(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0 && con >= 0) (void)!write(con, buf, n < (int)sizeof buf ? n : (int)sizeof buf - 1);
}

// Value of key= on the kernel cmdline, or def.
static const char* arg(const char* key, const char* def) {
    static char val[8][256];
    static int slot;
    size_t kl = strlen(key);
    for (char* p = cmdline; (p = strstr(p, key)); p += kl) {
        if ((p == cmdline || p[-1] == ' ') && p[kl] == '=') {
            char* v = val[slot++ & 7];
            size_t i = 0;
            for (p += kl + 1; *p && *p != ' ' && *p != '\n' && i < 255; ++p) v[i++] = *p;
            v[i] = 0;
            return v;
        }
    }
    return def;
}

static void mnt(const char* src, const char* dst, const char* type, unsigned long fl, const char* data) {
    mkdir(dst, 0755);
    if (mount(src, dst, type, fl, data) != 0 && errno != EBUSY)
        say("init: mount %s on %s: %s\n", src, dst, strerror(errno));
}

static void put(const char* path, const char* s) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) { (void)!write(fd, s, strlen(s)); close(fd); }
}

// ---------------------------------------------------------------- modules

// Load /lib/modules/potemkin/*.ko in the order given by the "load" file.
static void load_modules(void) {
    FILE* f = fopen("/lib/modules/potemkin/load", "r");
    if (!f) return;
    char name[256], path[512];
    while (fgets(name, sizeof name, f)) {
        name[strcspn(name, "\r\n")] = 0;
        if (!name[0] || name[0] == '#') continue;
        snprintf(path, sizeof path, "/lib/modules/potemkin/%s", name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) { say("init: %s: %s\n", path, strerror(errno)); continue; }
        if (syscall(SYS_finit_module, fd, "", 0) != 0 && errno != EEXIST)
            say("init: load %s: %s\n", name, strerror(errno));
        close(fd);
    }
    fclose(f);
}

// No udev and no nvidia-modprobe: make the device nodes libcuda opens.
static void nvidia_nodes(void) {
    if (access("/proc/driver/nvidia", F_OK) != 0) return;
    mknod("/dev/nvidiactl", S_IFCHR | 0666, makedev(195, 255));
    int n = 0;
    DIR* d = opendir("/proc/driver/nvidia/gpus");
    if (d) {
        struct dirent* e;
        while ((e = readdir(d))) if (e->d_name[0] != '.') ++n;
        closedir(d);
    }
    for (int i = 0; i < n; ++i) {
        char p[32];
        snprintf(p, sizeof p, "/dev/nvidia%d", i);
        mknod(p, S_IFCHR | 0666, makedev(195, i));
    }
    FILE* f = fopen("/proc/devices", "r");
    char line[128];
    while (f && fgets(line, sizeof line, f)) {
        int major;
        char nm[64];
        if (sscanf(line, "%d %63s", &major, nm) == 2 && !strcmp(nm, "nvidia-uvm")) {
            mknod("/dev/nvidia-uvm", S_IFCHR | 0666, makedev(major, 0));
            mknod("/dev/nvidia-uvm-tools", S_IFCHR | 0666, makedev(major, 1));
        }
    }
    if (f) fclose(f);
    say("init: nvidia: %d gpu(s)\n", n);
}

// ---------------------------------------------------------------- network

static void ifup(int s, const char* ifname, const char* addr, int prefix) {
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (addr) {
        struct sockaddr_in* sin = (struct sockaddr_in*)&ifr.ifr_addr;
        sin->sin_family = AF_INET;
        inet_pton(AF_INET, addr, &sin->sin_addr);
        if (ioctl(s, SIOCSIFADDR, &ifr) != 0) say("init: %s addr: %s\n", ifname, strerror(errno));
        uint32_t mask = prefix ? htonl(~0u << (32 - prefix)) : 0;
        sin->sin_addr.s_addr = mask;
        ioctl(s, SIOCSIFNETMASK, &ifr);
    }
    ioctl(s, SIOCGIFFLAGS, &ifr);
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    ioctl(s, SIOCSIFFLAGS, &ifr);
}

static void net(void) {
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return;
    ifup(s, "lo", "127.0.0.1", 8);
    const char* spec = arg("potemkin.net", NULL);
    if (!spec) { close(s); return; }
    char buf[256];
    snprintf(buf, sizeof buf, "%s", spec);
    char* addr = strtok(buf, ",");
    char* gw = strtok(NULL, ",");
    char* dns = strtok(NULL, ",");
    char* slash = addr ? strchr(addr, '/') : NULL;
    int prefix = 24;
    if (slash) { *slash = 0; prefix = atoi(slash + 1); }

    // First interface that is not lo.
    char ifname[IFNAMSIZ] = "";
    DIR* d = opendir("/sys/class/net");
    struct dirent* e;
    while (d && (e = readdir(d)))
        if (e->d_name[0] != '.' && strcmp(e->d_name, "lo")) { snprintf(ifname, sizeof ifname, "%.15s", e->d_name); break; }
    if (d) closedir(d);
    if (!ifname[0] || !addr) { say("init: no network interface\n"); close(s); return; }
    ifup(s, ifname, addr, prefix);
    if (gw) {
        struct rtentry rt;
        memset(&rt, 0, sizeof rt);
        struct sockaddr_in* g = (struct sockaddr_in*)&rt.rt_gateway;
        g->sin_family = AF_INET;
        inet_pton(AF_INET, gw, &g->sin_addr);
        ((struct sockaddr_in*)&rt.rt_dst)->sin_family = AF_INET;
        ((struct sockaddr_in*)&rt.rt_genmask)->sin_family = AF_INET;
        rt.rt_flags = RTF_UP | RTF_GATEWAY;
        if (ioctl(s, SIOCADDRT, &rt) != 0 && errno != EEXIST) say("init: route: %s\n", strerror(errno));
    }
    if (dns) {
        mkdir("/etc", 0755);
        FILE* f = fopen("/etc/resolv.conf", "w");
        if (f) { fprintf(f, "nameserver %s\n", dns); fclose(f); }
    }
    say("init: %s %s/%d gw %s\n", ifname, addr, prefix, gw ? gw : "-");
    close(s);
}

// ---------------------------------------------------------------- storage

static const char* trees[] = {"data", "state", "generated", "store", "intent", "cache", "snapshots", "models", NULL};

static void storage(void) {
    const char* disk = arg("potemkin.disk", "/dev/vda");
    mkdir("/persist", 0755);
    int ok = 0;
    for (int tries = 0; tries < 50 && !ok; ++tries) {
        if (access(disk, F_OK) == 0)
            ok = mount(disk, "/persist", "ext4", 0, NULL) == 0 || mount(disk, "/persist", "btrfs", 0, NULL) == 0;
        if (!ok) usleep(100000);  // the disk may still be probing
    }
    if (!ok) say("init: %s not mounted (%s); trees live in RAM this boot\n", disk, strerror(errno));
    for (const char** t = trees; *t; ++t) {
        char src[64], dst[64];
        snprintf(src, sizeof src, "/persist/%s", *t);
        snprintf(dst, sizeof dst, "/%s", *t);
        mkdir(src, 0755);
        mkdir(dst, 0755);
        if (ok && mount(src, dst, NULL, MS_BIND, NULL) != 0) say("init: bind %s: %s\n", dst, strerror(errno));
    }
    mkdir("/generated/bin", 0755);
}

// ---------------------------------------------------------------- q27-init

static pid_t start_model(const char* tty) {
    pid_t pid = fork();
    if (pid != 0) return pid;
    setsid();
    int fd = open(tty, O_RDWR);
    if (fd < 0) _exit(126);
    ioctl(fd, TIOCSCTTY, 1);
    dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
    if (fd > 2) close(fd);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    char* argv[] = {"/usr/bin/q27-init", "--tty", "-", "--sysroot", "/usr/lib/potemkin",
                    "--cgroup", "/sys/fs/cgroup", "--prefix-cache", "/cache",
                    "--engine-log", "/cache/q27-init.log", NULL};
    char* envp[] = {"PATH=/generated/bin", "HOME=/data", "TERM=linux", NULL};
    execve(argv[0], argv, envp);
    _exit(127);
}

// ---------------------------------------------------------------- rescue

static void rescue(const char* tty) {
    int fd = open(tty, O_RDWR);
    if (fd < 0) return;
    char c;
    for (;;) {
        dprintf(fd, "\r\n[ potemkin rescue ]\r\n 1) roll /generated and /state back to the newest snapshot\r\n"
                    " 2) move /generated aside (the store keeps every binary)\r\n 3) reboot\r\n> ");
        if (read(fd, &c, 1) != 1) continue;
        if (c == '1') {
            int best = 0;
            DIR* d = opendir("/snapshots");
            struct dirent* e;
            while (d && (e = readdir(d))) if (atoi(e->d_name) > best) best = atoi(e->d_name);
            if (d) closedir(d);
            if (!best) { dprintf(fd, "\r\nno snapshots\r\n"); continue; }
            // Swap the live trees for the snapshot's by renaming on the disk.
            char a[128], b[128];
            const char* both[] = {"state", "generated"};
            for (int i = 0; i < 2; ++i) {
                snprintf(a, sizeof a, "/persist/%s", both[i]);
                snprintf(b, sizeof b, "/persist/%s.before-rescue.%ld", both[i], (long)time(NULL));
                umount(a + strlen("/persist"));  // the bind mount at /state or /generated
                rename(a, b);
                snprintf(b, sizeof b, "/persist/snapshots/%d/%s", best, both[i]);
                rename(b, a);
            }
            dprintf(fd, "\r\nrolled back to snapshot %d; old trees kept as *.before-rescue.*\r\n", best);
        } else if (c == '2') {
            char b[128];
            snprintf(b, sizeof b, "/persist/generated.aside.%ld", (long)time(NULL));
            umount("/generated");
            rename("/persist/generated", b);
            mkdir("/persist/generated", 0755);
            mkdir("/persist/generated/bin", 0755);
            dprintf(fd, "\r\nmoved to %s\r\n", b + 8);
        } else if (c == '3') {
            sync();
            reboot(RB_AUTOBOOT);
        }
    }
}

// ---------------------------------------------------------------- main

int main(int argc, char** argv) {
    (void)argc;
    mnt("proc", "/proc", "proc", 0, NULL);
    mnt("sysfs", "/sys", "sysfs", 0, NULL);
    mnt("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mnt("devpts", "/dev/pts", "devpts", 0, "ptmxmode=0666");
    mnt("tmpfs", "/tmp", "tmpfs", 0, NULL);
    con = open("/dev/console", O_WRONLY | O_CLOEXEC);
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd >= 0) { (void)!read(fd, cmdline, sizeof cmdline - 1); close(fd); }
    mnt("cgroup2", "/sys/fs/cgroup", "cgroup2", 0, NULL);
    put("/sys/fs/cgroup/cgroup.subtree_control", "+memory +pids");
    mkdir("/sys/fs/cgroup/potemkin", 0755);
    put("/sys/fs/cgroup/potemkin/cgroup.subtree_control", "+memory +pids");

    load_modules();
    nvidia_nodes();
    net();
    storage();

    const char* tty = arg("potemkin.tty", "/dev/tty1");
    const char* me = strrchr(argv[0], '/');
    if (!strcmp(me ? me + 1 : argv[0], "rescue")) { rescue(tty); return 0; }

    signal(SIGINT, SIG_IGN);  // Ctrl-Alt-Del and friends are not a way down
    pid_t model = start_model(tty);
    time_t last_start = time(NULL);
    for (;;) {
        int st;
        pid_t p = wait(&st);
        if (p < 0) { if (errno == ECHILD) sleep(1); continue; }
        if (p != model) continue;  // reparented orphans: reaped, forgotten
        if (WIFEXITED(st) && WEXITSTATUS(st) == 3) {
            say("init: reboot requested\n");
            sync();
            reboot(RB_AUTOBOOT);
        }
        say("init: q27-init exited (%s %d); restarting\n", WIFEXITED(st) ? "status" : "signal",
            WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
        if (time(NULL) - last_start < 5) sleep(5);  // don't spin on a crash loop
        model = start_model(tty);
        last_start = time(NULL);
    }
}
