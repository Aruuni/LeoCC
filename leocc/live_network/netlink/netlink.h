#ifndef LEOCC_NETLINK_H
#define LEOCC_NETLINK_H

#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <linux/sort.h>
#include <linux/kstrtox.h>
#include <linux/limits.h>
#include <linux/printk.h>

#include <net/sock.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>

#define NETLINK_USER 30
#define RTT_SAMPLE_MAX 100

/* These live in leocc.c and are shared module-wide */
extern bool global_reconfiguration_trigger;
extern u32  min_rtt_fluctuation;

/* Keep your wire format identical to userspace */
struct rtt_data {
    u64 sec;
    u32 usec;
    char rtt_value_microseconds[16];
    u32 is_reconfig;
};

/* Per-network-namespace state */
struct leocc_nl_ns {
    struct sock *nl_sk;

    u32 rtt_samples[RTT_SAMPLE_MAX];
    u64 reconfiguration_trigger_time_ms;
    u32 reconfiguration_rtt_ms;

    u32 rtt_sample_count;
    u32 local_rtt_sample_max;
    u32 local_rtt_sample_min;

    bool min_rtt_fluctuation_collection;

    u32 reconfiguration_min_rtt;
    u32 reconfiguration_max_rtt;
};

/* Per-netns storage registration id */
static int leocc_nl_id;

/* Constants */
static const u32 global_reconfiguration_trigger_duration = 200; /* ms */

static int compare_func(const void *a, const void *b)
{
    u32 val_a = *(const u32 *)a;
    u32 val_b = *(const u32 *)b;

    if (val_a < val_b) return -1;
    if (val_a > val_b) return 1;
    return 0;
}

static void mpf(struct leocc_nl_ns *ns, u32 percentile_low, u32 percentile_high)
{
    ns->min_rtt_fluctuation_collection = false;

    if (ns->rtt_sample_count == 0)
        return;

    sort(ns->rtt_samples, ns->rtt_sample_count, sizeof(u32), compare_func, NULL);

    ns->reconfiguration_min_rtt =
        ns->rtt_samples[ns->rtt_sample_count * percentile_low / 100];
    ns->reconfiguration_max_rtt =
        ns->rtt_samples[ns->rtt_sample_count * percentile_high / 100];

    /* This is still global (as your CC code expects) */
    min_rtt_fluctuation = ns->reconfiguration_max_rtt - ns->reconfiguration_min_rtt;
}

static void netlink_recv_msg(struct sk_buff *skb)
{
    struct nlmsghdr *nlh;
    struct rtt_data *data;
    struct leocc_nl_ns *ns;
    struct net *net;

    if (!skb || !skb->sk)
        return;

    nlh = nlmsg_hdr(skb);
    if (!nlh || !nlmsg_ok(nlh, skb->len))
        return;

    data = (struct rtt_data *)nlmsg_data(nlh);
    if (!data)
        return;

    /* Figure out which netns this message came from */
    net = sock_net(skb->sk);
    ns = net_generic(net, leocc_nl_id);
    if (!ns)
        return;

    {
        u64 cur_time_ms = data->sec * 1000ULL + data->usec / 1000U;

        /* Auto-clear global trigger after duration */
        if (global_reconfiguration_trigger &&
            ns->reconfiguration_trigger_time_ms > 0 &&
            cur_time_ms >= ns->reconfiguration_trigger_time_ms +
                          global_reconfiguration_trigger_duration) {
            global_reconfiguration_trigger = false;
        }

        /* Start collection after "reconfiguration RTT" time has passed */
        if (!ns->min_rtt_fluctuation_collection &&
            ns->reconfiguration_trigger_time_ms > 0 &&
            cur_time_ms >= ns->reconfiguration_trigger_time_ms + ns->reconfiguration_rtt_ms) {

            ns->min_rtt_fluctuation_collection = true;

            ns->rtt_sample_count = 0;
            ns->reconfiguration_min_rtt = U32_MAX;
            ns->reconfiguration_max_rtt = 0;
            ns->local_rtt_sample_min = U32_MAX;
            ns->local_rtt_sample_max = 0;
        }

        /* If collecting, record samples */
        if (ns->min_rtt_fluctuation_collection) {
            u32 rtt_value = 0;

            if (kstrtouint(data->rtt_value_microseconds, 10, &rtt_value) == 0) {
                if (ns->rtt_sample_count < RTT_SAMPLE_MAX) {
                    ns->rtt_samples[ns->rtt_sample_count++] = rtt_value;

                    if (rtt_value < ns->local_rtt_sample_min)
                        ns->local_rtt_sample_min = rtt_value;
                    if (rtt_value > ns->local_rtt_sample_max)
                        ns->local_rtt_sample_max = rtt_value;
                } else {
                    /* buffer full -> compute fluctuation and stop */
                    mpf(ns, 5, 95);
                    ns->reconfiguration_trigger_time_ms = 0;
                }
            } else {
                pr_err("LEOCC netlink: invalid RTT value received\n");
            }
        }

        /* If reconfiguration message, set trigger + schedule collection */
        if (data->is_reconfig == 1) {
            u32 rtt_value_us = 0;

            global_reconfiguration_trigger = true;

            if (kstrtouint(data->rtt_value_microseconds, 10, &rtt_value_us) == 0) {
                ns->reconfiguration_trigger_time_ms = cur_time_ms;
                ns->reconfiguration_rtt_ms = rtt_value_us / 1000U;

                pr_info("[LEOCC] Reconfig detected in netns=%p: will start RTT collection after %u ms\n",
                        net, ns->reconfiguration_rtt_ms);
            } else {
                pr_err("LEOCC netlink: invalid RTT value during reconfig\n");
            }
            return;
        }
    }
}

/* Called once per netns (root + each Mininet host netns) */
static int __net_init leocc_nl_init_net(struct net *net)
{
    struct netlink_kernel_cfg cfg = {
        .input = netlink_recv_msg,
    };
    struct leocc_nl_ns *ns = net_generic(net, leocc_nl_id);

    ns->nl_sk = netlink_kernel_create(net, NETLINK_USER, &cfg);
    if (!ns->nl_sk) {
        pr_alert("LEOCC: error creating netlink socket in netns=%p\n", net);
        return -ENOMEM;
    }

    /* init per-netns fields */
    ns->reconfiguration_trigger_time_ms = 0;
    ns->reconfiguration_rtt_ms = 0;
    ns->rtt_sample_count = 0;
    ns->local_rtt_sample_max = 0;
    ns->local_rtt_sample_min = U32_MAX;
    ns->min_rtt_fluctuation_collection = false;
    ns->reconfiguration_min_rtt = U32_MAX;
    ns->reconfiguration_max_rtt = 0;

    pr_info("LEOCC: netlink socket created in netns=%p\n", net);
    return 0;
}

static void __net_exit leocc_nl_exit_net(struct net *net)
{
    struct leocc_nl_ns *ns = net_generic(net, leocc_nl_id);

    if (ns->nl_sk) {
        netlink_kernel_release(ns->nl_sk);
        ns->nl_sk = NULL;
        pr_info("LEOCC: netlink socket closed in netns=%p\n", net);
    }
}

static struct pernet_operations leocc_nl_ops = {
    .init = leocc_nl_init_net,
    .exit = leocc_nl_exit_net,
    .id   = &leocc_nl_id,
    .size = sizeof(struct leocc_nl_ns),
};

static int netlink_init(void)
{
    /*
     * This registers per-netns init/exit hooks.
     * Linux will call leocc_nl_init_net() for all existing netns,
     * and also for any new netns created later (e.g. Mininet hosts).
     */
    return register_pernet_subsys(&leocc_nl_ops);
}

static void netlink_exit(void)
{
    unregister_pernet_subsys(&leocc_nl_ops);
}

#endif /* LEOCC_NETLINK_H */
