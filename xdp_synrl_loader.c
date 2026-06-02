/*
 * Loader for xdp_synrl.o.
 *
 * Attaches the program in native (xdpdrv) mode and waits for SIGTERM/SIGINT
 * to detach cleanly. Object path and interface are CLI-overridable; default
 * interface is the one carrying the default route.
 */

#define _GNU_SOURCE
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <errno.h>
#include <getopt.h>
#include <linux/if_link.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_OBJ_PATH "/usr/local/lib/xdpdrop/xdp_synrl.o"
#define PROG_NAME        "xdp_synrl"

static volatile sig_atomic_t stop_flag = 0;
static int detach_ifindex = 0;

static void on_signal(int sig)
{
    (void)sig;
    stop_flag = 1;
}

/* Look up the interface name that owns the default IPv4 route by reading
 * /proc/net/route. Returns 0 on success. */
static int default_iface(char *out, size_t out_len)
{
    FILE *f = fopen("/proc/net/route", "r");
    if (!f)
        return -1;

    char line[512];
    if (!fgets(line, sizeof(line), f)) {       /* skip header */
        fclose(f);
        return -1;
    }

    int found = -1;
    while (fgets(line, sizeof(line), f)) {
        char iface[IF_NAMESIZE];
        unsigned long dest, mask;
        if (sscanf(line, "%15s %lx %*x %*x %*d %*d %*d %lx", iface, &dest, &mask) != 3)
            continue;
        if (dest == 0 && mask == 0) {
            if (strlen(iface) >= out_len)
                break;
            strcpy(out, iface);
            found = 0;
            break;
        }
    }
    fclose(f);
    return found;
}

static void usage(const char *p)
{
    fprintf(stderr,
            "Usage: %s [--iface=<name>] [--obj=<path>]\n"
            "  --iface  interface to attach (default: route to 0.0.0.0/0)\n"
            "  --obj    BPF object file (default: %s)\n",
            p, DEFAULT_OBJ_PATH);
}

int main(int argc, char **argv)
{
    char iface[IF_NAMESIZE] = {0};
    const char *obj_path = DEFAULT_OBJ_PATH;

    static const struct option opts[] = {
        {"iface", required_argument, NULL, 'i'},
        {"obj",   required_argument, NULL, 'o'},
        {"help",  no_argument,       NULL, 'h'},
        {0, 0, 0, 0},
    };

    int c;
    while ((c = getopt_long(argc, argv, "i:o:h", opts, NULL)) != -1) {
        switch (c) {
        case 'i':
            if (strlen(optarg) >= sizeof(iface)) {
                fprintf(stderr, "iface name too long\n");
                return 2;
            }
            strcpy(iface, optarg);
            break;
        case 'o':
            obj_path = optarg;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 2;
        }
    }

    if (iface[0] == '\0' && default_iface(iface, sizeof(iface)) != 0) {
        fprintf(stderr, "could not determine default interface\n");
        return 1;
    }

    int ifindex = if_nametoindex(iface);
    if (!ifindex) {
        fprintf(stderr, "if_nametoindex(%s): %s\n", iface, strerror(errno));
        return 1;
    }

    struct bpf_object *obj = bpf_object__open_file(obj_path, NULL);
    if (!obj || libbpf_get_error(obj)) {
        fprintf(stderr, "bpf_object__open_file(%s): %s\n",
                obj_path, strerror(-libbpf_get_error(obj)));
        return 1;
    }

    if (bpf_object__load(obj)) {
        fprintf(stderr, "bpf_object__load: %s\n", strerror(errno));
        bpf_object__close(obj);
        return 1;
    }

    struct bpf_program *prog = bpf_object__find_program_by_name(obj, PROG_NAME);
    if (!prog) {
        fprintf(stderr, "program %s not found in %s\n", PROG_NAME, obj_path);
        bpf_object__close(obj);
        return 1;
    }

    int prog_fd = bpf_program__fd(prog);
    if (prog_fd < 0) {
        fprintf(stderr, "bpf_program__fd: %d\n", prog_fd);
        bpf_object__close(obj);
        return 1;
    }

    /* DRV_MODE = native only; never silently fall back to skb (generic). */
    __u32 flags = XDP_FLAGS_DRV_MODE | XDP_FLAGS_UPDATE_IF_NOEXIST;
    int err = bpf_xdp_attach(ifindex, prog_fd, flags, NULL);
    if (err) {
        fprintf(stderr, "bpf_xdp_attach(%s): %s\n", iface, strerror(-err));
        bpf_object__close(obj);
        return 1;
    }
    detach_ifindex = ifindex;

    fprintf(stderr, "xdp_synrl attached on %s (ifindex %d), mode=native\n",
            iface, ifindex);

    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    while (!stop_flag)
        pause();

    fprintf(stderr, "xdp_synrl detaching from %s\n", iface);
    bpf_xdp_detach(detach_ifindex, XDP_FLAGS_DRV_MODE, NULL);
    bpf_object__close(obj);
    return 0;
}
