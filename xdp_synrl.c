/*
 * XDP SYN rate-limit for HAProxy frontend ports.
 *
 * Drops TCP SYN packets (SYN=1, ACK=0) destined to dst port 2000/2001/2002
 * once a shared 10000-token bucket is empty. All other traffic is passed.
 *
 * Token bucket parameters:
 *   refill rate = 10000 tokens/sec  -> 1 token / 100_000 ns
 *   capacity    = 10000 tokens      -> up to 1 s of burst
 *
 * Fail-open: any error path returns XDP_PASS so a verifier-passable but
 * runtime-buggy program never blackholes HAProxy.
 */

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/in.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#define BUCKET_CAP   10000ULL
#define NS_PER_TOKEN 100000ULL   /* 10_000 tokens/s = 1 token per 100us */

#define PORT_A 2000
#define PORT_B 2001
#define PORT_C 2002

struct token_state {
    struct bpf_spin_lock lock;
    __u64 tokens;
    __u64 last_ns;
};

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __type(key, __u32);
    __type(value, struct token_state);
    __uint(max_entries, 1);
} bucket SEC(".maps");

SEC("xdp")
int xdp_synrl(struct xdp_md *ctx)
{
    void *data     = (void *)(long)ctx->data;
    void *data_end = (void *)(long)ctx->data_end;

    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end)
        return XDP_PASS;
    if (eth->h_proto != bpf_htons(ETH_P_IP))
        return XDP_PASS;

    struct iphdr *iph = (void *)(eth + 1);
    if ((void *)(iph + 1) > data_end)
        return XDP_PASS;
    if (iph->protocol != IPPROTO_TCP)
        return XDP_PASS;

    /* iph->ihl is in 32-bit words and the IP header is at minimum 20 bytes
     * (ihl == 5). Bound it for the verifier so the TCP-header offset is
     * provably in range. */
    __u32 ihl_bytes = iph->ihl * 4;
    if (ihl_bytes < sizeof(*iph))
        return XDP_PASS;
    if (ihl_bytes > 60)
        return XDP_PASS;

    struct tcphdr *tcph = (void *)iph + ihl_bytes;
    if ((void *)(tcph + 1) > data_end)
        return XDP_PASS;

    __u16 dport = bpf_ntohs(tcph->dest);
    if (dport != PORT_A && dport != PORT_B && dport != PORT_C)
        return XDP_PASS;

    /* Pure SYN only: SYN=1, ACK=0. Anything else (SYN+ACK, ACK, FIN, RST,
     * data segments) bypasses the rate limit. */
    if (!(tcph->syn && !tcph->ack))
        return XDP_PASS;

    __u32 k = 0;
    struct token_state *st = bpf_map_lookup_elem(&bucket, &k);
    if (!st)
        return XDP_PASS;

    __u64 now = bpf_ktime_get_ns();
    int allow = 0;

    bpf_spin_lock(&st->lock);

    /* Refill. last_ns == 0 means uninitialised; just seed it without
     * granting a huge token windfall from the epoch. */
    if (st->last_ns == 0) {
        st->last_ns = now;
    } else if (now > st->last_ns) {
        __u64 delta_ns = now - st->last_ns;
        __u64 add = delta_ns / NS_PER_TOKEN;
        if (add > 0) {
            __u64 t = st->tokens + add;
            if (t > BUCKET_CAP)
                t = BUCKET_CAP;
            st->tokens = t;
            /* Advance by exactly the time we consumed so fractional
             * tokens are preserved across calls. */
            st->last_ns += add * NS_PER_TOKEN;
        }
    }

    if (st->tokens > 0) {
        st->tokens -= 1;
        allow = 1;
    }

    bpf_spin_unlock(&st->lock);

    return allow ? XDP_PASS : XDP_DROP;
}

char _license[] SEC("license") = "GPL";
