#include <uapi/linux/bpf.h>

#include <bpf_endian.h>
#include <bpf_helpers.h>

#include <uapi/linux/if_ether.h>
#include <uapi/linux/ip.h>

#include <linux/hash.h>


/**
 * Copied from <uapi/linux/tcp.h>,
 * which by itself causes errors related to `atomic64_t`.
 */

#define IPPROTO_TCP 6

#define TH_FIN  0x01
#define TH_SYN  0x02
#define TH_RST  0x04
#define TH_PUSH 0x08
#define TH_ACK  0x10
#define TH_URG  0x20
#define TH_ECE  0x40
#define TH_CWR  0x80

struct tcphdr {
        __u16   source;
        __u16   dest;
        __u32   seq;
        __u32   ack_seq;
        union {
            u16 flags;
            struct {
#if defined(__LITTLE_ENDIAN_BITFIELD)
        __u16   res1:4,
                doff:4,
                fin:1,
                syn:1,
                rst:1,
                psh:1,
                ack:1,
                urg:1,
                ece:1,
                cwr:1;
#elif defined(__BIG_ENDIAN_BITFIELD)
        __u16   doff:4,
                res1:4,
                cwr:1,
                ece:1,
                urg:1,
                ack:1,
                psh:1,
                rst:1,
                syn:1,
                fin:1;
#else
#error  "Adjust your <asm/byteorder.h> defines"
#endif
            };
        };
        __u16   window;
        __u16   check;
        __u16   urg_ptr;
};

/* eBPF requires all functions to be inlined */
#define INTERNAL static __attribute__((always_inline))

/* Log only in debug. */
#ifndef NDEBUG
#define bpf_printk(fmt, ...)                                    \
({                                                              \
               char ____fmt[] = fmt;                            \
               bpf_trace_printk(____fmt, sizeof(____fmt),       \
                                ##__VA_ARGS__);                 \
})
#define LOG(fmt, ...) bpf_printk(fmt "\n", ##__VA_ARGS__)
#else
#define LOG(fmt, ...)
#endif

/* eBPF lacks these functions, but LLVM provides builtins */
#ifndef memset
#define memset(dest, chr, n)   __builtin_memset((dest), (chr), (n))
#endif

#ifndef memcpy
#define memcpy(dest, src, n)   __builtin_memcpy((dest), (src), (n))
#endif

#ifndef memmove
#define memmove(dest, src, n)  __builtin_memmove((dest), (src), (n))
#endif


/**
 * Packet processing context.
 */
struct Packet {
    /* For verification to for passing to BPF helpers. */
    struct xdp_md* ctx;

    /* Layer headers (may be NULL on lower stages) */
    struct ethhdr* ether;
    struct iphdr* ip;
    struct tcphdr* tcp;
};

struct bpf_map_def SEC("maps") xdp_syn_attacked = {
	.type        = BPF_MAP_TYPE_ARRAY,
	.key_size    = sizeof(__u32),
	.value_size  = sizeof(_Bool), // Actualy value is not needed
	.max_entries = 32,
};

struct bpf_map_def SEC("maps") xdp_syn_cookie_ok = {
        .type        = BPF_MAP_TYPE_ARRAY,
        .key_size    = sizeof(__u32),
        .value_size  = sizeof(_Bool), // Actualy value is not needed
        .max_entries = 100000,
};


/**
 * Cookie computation
 */

struct FourTuple {
    u32 saddr;
    u32 daddr;
    u16 sport;
    u16 dport;
};

INTERNAL u32
cookie_counter() {
    return bpf_ktime_get_ns() >> (10 + 10 + 10 + 3); /* 8.6 sec */
}

INTERNAL u32
hash_crc32(u32 data, u32 seed) {
    return hash_32(seed | data, 32); /* TODO: use better hash */
}

INTERNAL u32
cookie_hash_count(u32 seed, u32 count) {
    return hash_crc32(count, seed);
}

INTERNAL u32
cookie_hash_base(struct FourTuple t, u32 seqnum) {
    /* TODO: randomize periodically from external source */
    u32 cookie_seed = 42;

    u32 res = hash_crc32(((u64)t.daddr << 32) | t.saddr, cookie_seed);
    return hash_crc32(((u64)t.dport << 48) | ((u64)seqnum << 16) | (u64)t.sport, res);
}

INTERNAL u32
cookie_make(struct FourTuple tuple, u32 seqnum, u32 count) {
    return seqnum + cookie_hash_count(cookie_hash_base(tuple, seqnum), count);
}

INTERNAL int
cookie_check(struct FourTuple tuple, u32 seqnum, u32 cookie, u32 count) {
    u32 hb = cookie_hash_base(tuple, seqnum);
    cookie -= seqnum;
    if (cookie == cookie_hash_count(hb, count)) {
        return 1;
    }
    return cookie == cookie_hash_count(hb, count - 1);
}


/**
 * Calculate sum of 16-bit words from `data` of `size` bytes,
 * Size is assumed to be even, from 0 to MAX_CSUM_BYTES.
 */
#define MAX_CSUM_WORDS 32
#define MAX_CSUM_BYTES (MAX_CSUM_WORDS * 2)

INTERNAL u32
sum16(const void* data, u32 size, const void* data_end) {
    u32 s = 0;
#pragma unroll
    for (u32 i = 0; i < MAX_CSUM_WORDS; i++) {
        if (2*i >= size) {
            return s; /* normal exit */
        }
        if (data + 2*i + 1 + 1 > data_end) {
            return 0; /* should be unreachable */
        }
        s += ((const u16*)data)[i];
    }
    return s;
}

/**
 * A handy version of `sum16()` for 32-bit words.
 * Does not actually conserve any instructions.
 */
INTERNAL u32
sum16_32(u32 v) {
    return (v >> 16) + (v & 0xffff);
}

/**
 * Carry upper bits and compute one's complement for a checksum.
 */
INTERNAL u16
carry(u32 csum) {
    csum = (csum & 0xffff) + (csum >> 16);
    csum = (csum & 0xffff) + (csum >> 16); // loop
    return ~csum;
}

INTERNAL int
process_tcp_syn(struct Packet* packet) {
    struct xdp_md* ctx   = packet->ctx;
    struct ethhdr* ether = packet->ether;
    struct iphdr*  ip    = packet->ip;
    struct tcphdr* tcp   = packet->tcp;

    /* Required to verify checksum calculation */
    const void* data_end = (void*)ctx->data_end;

    /* Validate IP header length */
    const u32 ip_len = ip->ihl * 4;
    if ((void*)ip + ip_len > data_end) {
        return XDP_DROP; /* malformed packet */
    }
    if (ip_len > MAX_CSUM_BYTES) {
        return XDP_ABORTED; /* implementation limitation */
    }

    /* Validate TCP length */
    const u32 tcp_len = tcp->doff * 4;
    if ((void*)tcp + tcp_len > data_end) {
        return XDP_DROP; /* malformed packet */
    }
    if (tcp_len > MAX_CSUM_BYTES) {
        return XDP_ABORTED; /* implementation limitation */
    }

    /* Create SYN-ACK with cookie */
    struct FourTuple tuple = {ip->saddr, ip->daddr, tcp->source, tcp->dest};
    const u32 cookie = cookie_make(tuple, bpf_ntohl(tcp->seq), cookie_counter());
    tcp->ack_seq = bpf_htonl(bpf_ntohl(tcp->seq) + 1);
    tcp->seq = bpf_htonl(cookie);
    tcp->ack = 1;

    /* Reverse TCP ports */
    const u16 temp_port = tcp->source;
    tcp->source = tcp->dest;
    tcp->dest = temp_port;

    /* Reverse IP direction */
    const u32 temp_ip = ip->saddr;
    ip->saddr = ip->daddr;
    ip->daddr = temp_ip;

    /* Reverse Ethernet direction */
    struct ethhdr temp_ether = *ether;
    memcpy(ether->h_dest, temp_ether.h_source, ETH_ALEN);
    memcpy(ether->h_source, temp_ether.h_dest, ETH_ALEN);

    /* Clear IP options */
    memset(ip + 1, ip_len - sizeof(struct iphdr), 0);

    /* Update IP checksum */
    ip->check = 0;
    ip->check = carry(sum16(ip, ip_len, data_end));

    /* Update TCP checksum */
    u32 tcp_csum = 0;
    tcp_csum += sum16_32(ip->saddr);
    tcp_csum += sum16_32(ip->daddr);
    tcp_csum += 0x0600;
    tcp_csum += tcp_len << 8;
    tcp->check = 0;
    tcp_csum += sum16(tcp, tcp_len, data_end);
    tcp->check = carry(tcp_csum);

    /* Send packet back */
    return XDP_TX;
}

INTERNAL int
process_tcp_ack(struct Packet* packet) {
    struct iphdr*  ip    = packet->ip;
    struct tcphdr* tcp   = packet->tcp;

    const struct FourTuple tuple = {
            ip->saddr, ip->daddr, tcp->source, tcp->dest};
    if (cookie_check(
            tuple,
            bpf_ntohl(tcp->seq) - 1,
            bpf_ntohl(tcp->ack_seq) - 1,
            cookie_counter())) {
        /* TODO: remember legitimate client */
        LOG("      cookie matches for client %x . Saving to BPF map.", ip->saddr);
	_Bool value = 1;
	__be32 key = ip->saddr;
	bpf_map_update_elem(&xdp_syn_cookie_ok, &key, &value, BPF_ANY);
    } else {
        LOG("      cookie mismatch");
        return XDP_DROP;
    }
    return XDP_PASS;
}

INTERNAL int
process_tcp(struct Packet* packet) {
    struct tcphdr* tcp   = packet->tcp;

    LOG("    TCP(sport=%d dport=%d flags=0x%x)",
            bpf_ntohs(tcp->source), bpf_ntohs(tcp->dest),
            bpf_ntohs(tcp->flags) & 0xff);

    switch (bpf_ntohs(tcp->flags) & (TH_SYN | TH_ACK)) {
    case TH_SYN:
        return process_tcp_syn(packet);
    case TH_ACK:
        return process_tcp_ack(packet);
    default:
        return XDP_PASS;
    }
}

INTERNAL int
process_ip(struct Packet* packet) {
    struct iphdr* ip = packet->ip;

    LOG("  IP(src=0x%x dst=0x%x proto=%d)",
        &ip->saddr, &ip->daddr, ip->protocol);

    if (ip->protocol != IPPROTO_TCP) {
        return XDP_PASS;
    }

    /* Check if client has passed SYN cookie challenge */
    __be32 key = ip->saddr;
    _Bool rec=NULL;
    rec = bpf_map_lookup_elem(&xdp_syn_cookie_ok, &key);
    if (rec)
	    return XDP_PASS;

    struct tcphdr* tcp = (struct tcphdr*)(ip + 1);
    if ((void*)(tcp + 1) > (void*)packet->ctx->data_end) {
        return XDP_DROP; /* malformed packet */
    }
    packet->tcp = tcp;

    return process_tcp(packet);
}

INTERNAL int
process_ether(struct Packet* packet) {
    struct ethhdr* ether = packet->ether;

    LOG("Ether(proto=0x%x)", bpf_ntohs(ether->h_proto));

    if (ether->h_proto != bpf_ntohs(ETH_P_IP)) {
        return XDP_PASS;
    }

    struct iphdr* ip = (struct iphdr*)(ether + 1);
    if ((void*)(ip + 1) > (void*)packet->ctx->data_end) {
        return XDP_DROP; /* malformed packet */
    }
    packet->ip = ip;
    return process_ip(packet);
}

SEC("prog")
int xdp_main(struct xdp_md* ctx) {
    struct Packet packet;
    packet.ctx = ctx;

    struct ethhdr* ether = (struct ethhdr*)(void*)ctx->data;
    if ((void*)(ether + 1) > (void*)ctx->data_end) {
        return XDP_PASS; /* what are you? */
    }

    packet.ether = ether;
    return process_ether(&packet);
}

char _license[] SEC("license") = "GPL";
