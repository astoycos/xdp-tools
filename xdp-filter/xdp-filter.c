/* SPDX-License-Identifier: GPL-2.0 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <arpa/inet.h>

#include <net/if.h>
#include <linux/if_ether.h>

#include "params.h"
#include "logging.h"
#include "util.h"
#include "stats.h"
#include "common_kern_user.h"
#include "prog_features.h"

#define NEED_RLIMIT (20*1024*1024) /* 10 Mbyte */
#define PROG_NAME "xdp-filter"

struct flag_val map_flags_all[] = {
	{"src", MAP_FLAG_SRC},
	{"dst", MAP_FLAG_DST},
	{"tcp", MAP_FLAG_TCP},
	{"udp", MAP_FLAG_UDP},
	{}
};

struct flag_val map_flags_srcdst[] = {
	{"src", MAP_FLAG_SRC},
	{"dst", MAP_FLAG_DST},
	{}
};

struct flag_val map_flags_tcpudp[] = {
	{"tcp", MAP_FLAG_TCP},
	{"udp", MAP_FLAG_UDP},
	{}
};

static char *find_progname(__u32 features)
{
	struct prog_feature *feat;

	if (!features)
		return NULL;

	for (feat = prog_features; feat->prog_name; feat++) {
		if ((ntohl(feat->features) & features) == features)
			return feat->prog_name;
	}
	return NULL;
}

static __u32 find_features(const char *progname)
{
	struct prog_feature *feat;

	for (feat = prog_features; feat->prog_name; feat++) {
		if (is_prefix(progname, feat->prog_name))
			return ntohl(feat->features);
	}
	return 0;
}

int map_get_counter_flags(int fd, void *key, __u64 *counter, __u8 *flags)
{
	/* For percpu maps, userspace gets a value per possible CPU */
	unsigned int nr_cpus = libbpf_num_possible_cpus();
	__u64 values[nr_cpus];
	__u64 sum_ctr = 0;
	int i;

	if ((bpf_map_lookup_elem(fd, key, values)) != 0)
		return -ENOENT;

	/* Sum values from each CPU */
	for (i = 0; i < nr_cpus; i++) {
		__u8 flg = values[i] & MAP_FLAGS;

		if (!flg)
			return -ENOENT; /* not set */
		*flags = flg;
		sum_ctr += values[i] >> COUNTER_SHIFT;
	}
	*counter = sum_ctr;

	return 0;
}

int map_set_flags(int fd, void *key, __u8 flags)
{
	/* For percpu maps, userspace gets a value per possible CPU */
	unsigned int nr_cpus = libbpf_num_possible_cpus();
	__u64 values[nr_cpus];
	int i;

	if ((bpf_map_lookup_elem(fd, key, values)) != 0)
		memset(values, 0, sizeof(values));

	for (i = 0; i < nr_cpus; i++)
		values[i]  = flags ? (values[i] & ~MAP_FLAGS) | (flags & MAP_FLAGS) : 0;

	pr_debug("Setting new map value %llu from flags %u\n", values[0], flags);

	return bpf_map_update_elem(fd, key, &values, 0);
}

static const struct loadopt {
	bool help;
	struct iface iface;
	int features;
	bool force;
	bool skb_mode;
	bool whitelist_mode;
} defaults_load = {
	.features = FEAT_ALL,
};

struct flag_val load_features[] = {
	{"tcp", FEAT_TCP},
	{"udp", FEAT_UDP},
	{"ipv6", FEAT_IPV6},
	{"ipv4", FEAT_IPV4},
	{"ethernet", FEAT_ETHERNET},
	{"all", FEAT_ALL},
	{}
};

struct flag_val print_features[] = {
	{"tcp", FEAT_TCP},
	{"udp", FEAT_UDP},
	{"ipv6", FEAT_IPV6},
	{"ipv4", FEAT_IPV4},
	{"ethernet", FEAT_ETHERNET},
	{"whitelist", FEAT_WHITELIST},
	{"blacklist", FEAT_BLACKLIST},
	{}
};

static struct prog_option load_options[] = {
	DEFINE_OPTION("force", OPT_BOOL, struct loadopt, force,
		      .short_opt = 'F',
		      .help = "Force loading of XDP program"),
	DEFINE_OPTION("skb-mode", OPT_BOOL, struct loadopt, skb_mode,
		      .short_opt = 's',
		      .help = "Load XDP program in SKB (generic) mode"),
	DEFINE_OPTION("whitelist", OPT_BOOL, struct loadopt, whitelist_mode,
		      .short_opt = 'w',
		      .help = "Use filters in whitelist mode (default blacklist)"),
	DEFINE_OPTION("dev", OPT_IFNAME, struct loadopt, iface,
		      .positional = true,
		      .metavar = "<ifname>",
		      .required = true,
		      .help = "Load on device <ifname>"),
	DEFINE_OPTION("features", OPT_FLAGS, struct loadopt, features,
		      .short_opt = 'f',
		      .metavar = "<feats>",
		      .typearg = load_features,
		      .help = "Features to enable; default all"),
	END_OPTIONS
};

int do_load(const void *cfg, const char *pin_root_path)
{
	char errmsg[STRERR_BUFSIZE], featbuf[100];
	const struct loadopt *opt = cfg;
	struct bpf_object *obj = NULL;
	int features = opt->features;
	int err = EXIT_SUCCESS;
	char *progname;
	DECLARE_LIBBPF_OPTS(bpf_object_open_opts, opts,
			    .pin_root_path = pin_root_path);

	if (opt->whitelist_mode)
		features |= FEAT_WHITELIST;
	else
		features |= FEAT_BLACKLIST;

	print_flags(featbuf, sizeof(featbuf), print_features, features);
	pr_debug("Looking for eBPF program with features %s\n", featbuf);

	progname = find_progname(features);
	if (!progname) {
		pr_warn("Couldn't find an eBPF program with the requested feature set!\n");
		return EXIT_FAILURE;
	}

	pr_debug("Found prog '%s' matching feature set to be loaded on interface '%s'.\n",
		 progname, opt->iface.ifname);

	/* libbpf spits out a lot of unhelpful error messages while loading.
	 * Silence the logging so we can provide our own messages instead; this
	 * is a noop if verbose logging is enabled.
	 */
	silence_libbpf_logging();

retry:

	obj = open_bpf_file(progname, &opts);
	err = libbpf_get_error(obj);
	if (err) {
		obj = NULL;
		goto out;
	}


	err = bpf_object__load(obj);
	if (err) {
		pr_debug("Permission denied when loading eBPF object; "
			 "raising rlimit and retrying\n");

		if (!double_rlimit()) {
			bpf_object__close(obj);
			goto retry;
		}

		libbpf_strerror(err, errmsg, sizeof(errmsg));
		pr_warn("Couldn't load eBPF object: %s(%d)\n", errmsg, err);
		goto out;
	}

	err = attach_xdp_program(obj, NULL, &opt->iface, opt->force,
				 opt->skb_mode, pin_root_path);
	if (err) {
		pr_warn("Couldn't attach XDP program on iface '%s'\n",
			opt->iface.ifname);
		goto out;
	}

out:
	if (obj)
		bpf_object__close(obj);
	return err;
}

static int remove_unused_maps(const char *pin_root_path, __u32 features)
{
	int dir_fd, err = 0;

	dir_fd = open(pin_root_path, O_DIRECTORY);
	if (dir_fd < 0) {
		err = -errno;
		pr_warn("Unable to open pin directory %s: %s\n",
			pin_root_path, strerror(-err));
		goto out;
	}

	if (!(features & (FEAT_TCP | FEAT_UDP))) {
		err = unlink_pinned_map(dir_fd, textify(MAP_NAME_PORTS));
		if (err)
			goto out;
	}

	if (!(features & FEAT_IPV4)) {
		err = unlink_pinned_map(dir_fd, textify(MAP_NAME_IPV4));
		if (err)
			goto out;
	}

	if (!(features & FEAT_IPV6)) {
		err = unlink_pinned_map(dir_fd, textify(MAP_NAME_IPV6));
		if (err)
			goto out;
	}

	if (!(features & FEAT_ETHERNET)) {
		err = unlink_pinned_map(dir_fd, textify(MAP_NAME_ETHERNET));
		if (err)
			goto out;
	}

	if (!features) {
		char buf[PATH_MAX];

		err = unlink_pinned_map(dir_fd, textify(XDP_STATS_MAP_NAME));
		if (err)
			goto out;

		close(dir_fd);
		dir_fd = -1;

		err = check_snprintf(buf, sizeof(buf), "%s/%s", pin_root_path, "programs");
		pr_debug("Removing program directory %s\n", buf);
		err = rmdir(buf);
		if (err) {
			err = -errno;
			pr_warn("Unable to rmdir: %s\n", strerror(-err));
			goto out;
		}

		pr_debug("Removing pinning directory %s\n", pin_root_path);
		err = rmdir(pin_root_path);
		if (err) {
			err = -errno;
			pr_warn("Unable to rmdir: %s\n", strerror(-err));
			goto out;
		}
	}
out:
	if (dir_fd >= 0)
		close(dir_fd);

	return err;
}

static const struct unloadopt {
	bool keep;
	struct iface iface;
} defaults_unload = {};

static struct prog_option unload_options[] = {
	DEFINE_OPTION("keep-maps", OPT_BOOL, struct unloadopt, keep,
		      .short_opt = 'k',
		      .help = "Don't destroy unused maps after unloading"),
	DEFINE_OPTION("dev", OPT_IFNAME, struct unloadopt, iface,
		      .positional = true,
		      .metavar = "<ifname>",
		      .required = true,
		      .help = "Load on device <ifname>"),
	END_OPTIONS
};

int do_unload(const void *cfg, const char *pin_root_path)
{
	struct if_nameindex *idx, *indexes = NULL;
	const struct unloadopt *opt = cfg;
	struct bpf_prog_info info = {};
	int err = EXIT_SUCCESS;
	char featbuf[100];
	__u32 feat, all_feats = 0;
	DECLARE_LIBBPF_OPTS(bpf_object_open_opts, opts,
			    .pin_root_path = pin_root_path);

	err = get_xdp_prog_info(opt->iface.ifindex, &info);
	if (err) {
		pr_warn("Couldn't find XDP program on interface %s\n",
			opt->iface.ifname);
		goto out;
	}

	feat = find_features(info.name);
	if (!feat) {
		pr_warn("Unrecognised XDP program on interface %s. Not removing.\n",
			opt->iface.ifname);
		err = EXIT_FAILURE;
		goto out;
	}

	print_flags(featbuf, sizeof(featbuf), print_features, feat);
	pr_debug("Removing XDP program with features %s from iface %s\n",
		 featbuf, opt->iface.ifname);

	err = detach_xdp_program(&opt->iface, pin_root_path);
	if (err) {
		pr_warn("Removing set xdp fd on iface %s failed (%d): %s\n",
			opt->iface.ifname, -err, strerror(-err));
		goto out;
	}

	if (opt->keep) {
		pr_debug("Not removing pinned maps because of --keep-maps option\n");
		goto out;
	}

	pr_debug("Checking map usage and removing unused maps\n");
	indexes = if_nameindex();
	if (!indexes) {
		err = -errno;
		pr_warn("Couldn't get list of interfaces: %s\n",
			strerror(-err));
		goto out;
	}

	for(idx = indexes; idx->if_index; idx++) {
		memset(&info, 0, sizeof(info));
		err = get_xdp_prog_info(idx->if_index, &info);
		if (err && err == -ENOENT)
			continue;
		else if (err) {
			pr_warn("Couldn't get XDP program info for ifindex %d: %s\n",
				idx->if_index, strerror(-err));
			goto out;
		}

		feat = find_features(info.name);
		if (feat)
			all_feats |= feat;
	}

	print_flags(featbuf, sizeof(featbuf), print_features, all_feats);
	pr_debug("Features still being used: %s\n", all_feats ? featbuf : "none");

	err = remove_unused_maps(pin_root_path, all_feats);
	if (err)
		goto out;

out:
	if (indexes)
		if_freenameindex(indexes);

	return err;
}

int print_ports(int map_fd)
{
	__u32 map_key = -1, next_key = 0;
	int err;

	printf("Filtered ports:\n");
	printf("  %-40s Mode             Hit counter\n", "");
	FOR_EACH_MAP_KEY(err, map_fd, map_key, next_key)
	{
		char buf[100];
		__u64 counter;
		__u8 flags;

		err = map_get_counter_flags(map_fd, &map_key, &counter, &flags);
		if (err == -ENOENT)
			continue;
		else if (err)
			return err;

		print_flags(buf, sizeof(buf), map_flags_all, flags);
		printf("  %-40u %-15s  %llu\n", ntohs(map_key), buf, counter);
	}
	return 0;
}

static const struct portopt {
	unsigned int mode;
	unsigned int proto;
	__u16 port;
	bool print_status;
	bool remove;
} defaults_port = {
	.mode = MAP_FLAG_DST,
	.proto = MAP_FLAG_TCP | MAP_FLAG_UDP,
};

static struct prog_option port_options[] = {
	DEFINE_OPTION("port", OPT_U16, struct portopt, port,
		      .positional = true,
		      .metavar = "<port>",
		      .required = true,
		      .help = "Port to add or remove"),
	DEFINE_OPTION("remove", OPT_BOOL, struct portopt, remove,
		      .short_opt = 'r',
		      .help = "Remove port instead of adding"),
	DEFINE_OPTION("status", OPT_BOOL, struct portopt, print_status,
		      .short_opt = 's',
		      .help = "Print status of filtered ports after changing"),
	DEFINE_OPTION("mode", OPT_FLAGS, struct portopt, mode,
		      .short_opt = 'm',
		      .metavar = "<mode>",
		      .typearg = map_flags_srcdst,
		      .help = "Filter mode; default dst"),
	DEFINE_OPTION("proto", OPT_FLAGS, struct portopt, proto,
		      .short_opt = 'p',
		      .metavar = "<proto>",
		      .typearg = map_flags_tcpudp,
		      .help = "Protocol to filter; default tcp,udp"),
	END_OPTIONS
};


int do_port(const void *cfg, const char *pin_root_path)
{
	int map_fd = -1, err = EXIT_SUCCESS;
	char modestr[100], protostr[100];
	const struct portopt *opt = cfg;
	__u8 flags = 0;
	__u64 counter;
	__u32 map_key;

	print_flags(modestr, sizeof(modestr), map_flags_srcdst, opt->mode);
	print_flags(protostr, sizeof(protostr), map_flags_tcpudp, opt->proto);
	pr_debug("%s %s port %u mode %s\n", opt->remove ? "Removing" : "Adding",
		 protostr, opt->port, modestr);

	map_fd = get_pinned_map_fd(pin_root_path, textify(MAP_NAME_PORTS), NULL);
	if (map_fd < 0) {
		pr_warn("Couldn't find port filter map; is xdp-filter loaded "
			"with the right features (udp and/or tcp)?\n");
		err = EXIT_FAILURE;
		goto out;
	}

	map_key = htons(opt->port);

	err = map_get_counter_flags(map_fd, &map_key, &counter, &flags);
	if (err && err != -ENOENT)
		goto out;

	if (opt->remove)
		flags &= ~(opt->mode | opt->proto);
	else
		flags |= opt->mode | opt->proto;

	if (!(flags & (MAP_FLAG_DST | MAP_FLAG_SRC)) ||
	    !(flags & (MAP_FLAG_TCP | MAP_FLAG_UDP)))
		flags = 0;

	err = map_set_flags(map_fd, &map_key, flags);
	if (err)
		goto out;

	if (opt->print_status) {
		err = print_ports(map_fd);
		if (err)
			goto out;
	}

out:
	if (map_fd >= 0)
		close(map_fd);
	return err;
}

int __print_ips(int map_fd, int af)
{
	struct ip_addr map_key = {.af = af}, next_key = {};
	int err;

	FOR_EACH_MAP_KEY(err, map_fd, map_key.addr, next_key.addr)
	{
		char flagbuf[100], addrbuf[100];
		__u64 counter;
		__u8 flags;

		err = map_get_counter_flags(map_fd, &map_key.addr, &counter, &flags);
		if (err == -ENOENT)
			continue;
		else if (err)
			return err;

		print_flags(flagbuf, sizeof(flagbuf), map_flags_srcdst, flags);
		print_addr(addrbuf, sizeof(addrbuf), &map_key);
		printf("  %-40s %-15s  %llu\n", addrbuf, flagbuf, counter);
	}

	return 0;
}

int print_ips()
{
	char pin_root_path[PATH_MAX];
	int map_fd4, map_fd6;
	int err = 0;

	err = get_bpf_root_dir(pin_root_path, sizeof(pin_root_path), PROG_NAME);
	if (err)
		goto out;

	map_fd6 = get_pinned_map_fd(pin_root_path, textify(MAP_NAME_IPV6), NULL);
	map_fd4 = get_pinned_map_fd(pin_root_path, textify(MAP_NAME_IPV4), NULL);
	if (map_fd4 < 0 && map_fd6 < 0) {
		err = -ENOENT;
		goto out;
	}

	printf("Filtered IP addresses:\n");
	printf("  %-40s Mode             Hit counter\n", "");

	if (map_fd6 >= 0) {
		err = __print_ips(map_fd6, AF_INET6);
		if (err)
			goto out;
	}

	if (map_fd4 >= 0)
		err = __print_ips(map_fd4, AF_INET);

out:
	if (map_fd4 >= 0)
		close(map_fd4);
	if (map_fd6 >= 0)
		close(map_fd6);
	return err;
}


static int __do_address(const char *pin_root_path,
			const char *map_name, const char *feat_name,
			void *map_key, bool remove, int mode)
{
	int map_fd = -1, err = 0;
	__u8 flags = 0;
	__u64 counter;

	map_fd = get_pinned_map_fd(pin_root_path, map_name, NULL);
	if (map_fd < 0) {
		pr_warn("Couldn't find filter map; is xdp-filter loaded "
			"with the %s feature?\n", feat_name);
		err = -ENOENT;
		goto out;
	}

	err = map_get_counter_flags(map_fd, map_key, &counter, &flags);
	if (err && err != -ENOENT)
		goto out;

	if (remove)
		flags &= ~mode;
	else
		flags |= mode;

	err = map_set_flags(map_fd, map_key, flags);
	if (err)
		goto out;

out:
	return err ?: map_fd;
}

static const struct ipopt {
	unsigned int mode;
	struct ip_addr addr;
	bool print_status;
	bool remove;
} defaults_ip = {
	.mode = MAP_FLAG_DST,
};

static struct prog_option ip_options[] = {
	DEFINE_OPTION("addr", OPT_IPADDR, struct ipopt, addr,
		      .positional = true,
		      .metavar = "<addr>",
		      .required = true,
		      .help = "Address to add or remove"),
	DEFINE_OPTION("remove", OPT_BOOL, struct ipopt, remove,
		      .short_opt = 'r',
		      .help = "Remove address instead of adding"),
	DEFINE_OPTION("status", OPT_BOOL, struct ipopt, print_status,
		      .short_opt = 's',
		      .help = "Print status of filtered addresses after changing"),
	DEFINE_OPTION("mode", OPT_FLAGS, struct ipopt, mode,
		      .short_opt = 'm',
		      .metavar = "<mode>",
		      .typearg = map_flags_srcdst,
		      .help = "Filter mode; default dst"),
	END_OPTIONS
};

static int do_ip(const void *cfg, const char *pin_root_path)
{
	int map_fd = -1, err = EXIT_SUCCESS;
	char modestr[100], addrstr[100];
	const struct ipopt *opt = cfg;
	struct ip_addr addr = opt->addr;
	bool v6;

	print_flags(modestr, sizeof(modestr), map_flags_srcdst, opt->mode);
	print_addr(addrstr, sizeof(addrstr), &opt->addr);
	pr_debug("%s addr %s mode %s\n", opt->remove ? "Removing" : "Adding",
		 addrstr, modestr);

	v6 = (opt->addr.af == AF_INET6);

	map_fd = __do_address(pin_root_path,
			      v6 ? textify(MAP_NAME_IPV6) : textify(MAP_NAME_IPV4),
			      v6 ? "ipv6" : "ipv4",
			      &addr.addr, opt->remove, opt->mode);
	if (map_fd < 0) {
		err = map_fd;
		goto out;
	}

	if (opt->print_status) {
		err = print_ips();
		if (err)
			goto out;
	}

out:
	if (map_fd >= 0)
		close(map_fd);
	return err;
}

int print_ethers(int map_fd)
{
	struct mac_addr map_key = {}, next_key = {};
	int err;

	printf("Filtered MAC addresses:\n");
	printf("  %-40s Mode             Hit counter\n", "");
	FOR_EACH_MAP_KEY(err, map_fd, map_key, next_key)
	{
		char modebuf[100], addrbuf[100];
		__u64 counter;
		__u8 flags;

		err = map_get_counter_flags(map_fd, &map_key, &counter, &flags);
		if (err == -ENOENT)
			continue;
		else if (err)
			return err;

		print_flags(modebuf, sizeof(modebuf), map_flags_srcdst, flags);
		print_macaddr(addrbuf, sizeof(addrbuf), &map_key);
		printf("  %-40s %-15s  %llu\n", addrbuf, modebuf, counter);
	}
	return 0;
}

static const struct etheropt {
	unsigned int mode;
	struct mac_addr addr;
	bool print_status;
	bool remove;
} defaults_ether = {
		.mode = MAP_FLAG_DST,
};

static struct prog_option ether_options[] = {
	DEFINE_OPTION("addr", OPT_MACADDR, struct etheropt, addr,
		      .positional = true,
		      .metavar = "<addr>",
		      .required = true,
		      .help = "Address to add or remove"),
	DEFINE_OPTION("remove", OPT_BOOL, struct etheropt, remove,
		      .short_opt = 'r',
		      .help = "Remove address instead of adding"),
	DEFINE_OPTION("status", OPT_BOOL, struct etheropt, print_status,
		      .short_opt = 's',
		      .help = "Print status of filtered addresses after changing"),
	DEFINE_OPTION("mode", OPT_FLAGS, struct etheropt, mode,
		      .short_opt = 'm',
		      .metavar = "<mode>",
		      .typearg = map_flags_srcdst,
		      .help = "Filter mode; default dst"),
	END_OPTIONS
};

static int do_ether(const void *cfg, const char *pin_root_path)
{
	int err = EXIT_SUCCESS, map_fd = -1;
	const struct etheropt *opt = cfg;
	struct mac_addr addr = opt->addr;
	char modestr[100], addrstr[100];

	print_flags(modestr, sizeof(modestr), map_flags_srcdst, opt->mode);
	print_macaddr(addrstr, sizeof(addrstr), &opt->addr);
	pr_debug("%s addr %s mode %s\n", opt->remove ? "Removing" : "Adding",
		 addrstr, modestr);

	map_fd = __do_address(pin_root_path, textify(MAP_NAME_ETHERNET),
			      "ethernet", &addr.addr, opt->remove, opt->mode);
	if (map_fd < 0) {
		err = map_fd;
		goto out;
	}

	if (opt->print_status) {
		err = print_ethers(map_fd);
		if (err)
			goto out;
	}

out:
	if (map_fd >= 0)
		close(map_fd);
	return err;
}

static struct prog_option status_options[] = {
	END_OPTIONS
};

int do_status(const void *cfg, const char *pin_root_path)
{
	struct if_nameindex *idx, *indexes = NULL;
	int err = EXIT_SUCCESS, map_fd = -1;
	char errmsg[STRERR_BUFSIZE];
	struct stats_record rec = {};
	struct bpf_map_info info;

	map_fd = get_pinned_map_fd(pin_root_path, textify(XDP_STATS_MAP_NAME), &info);
	if (map_fd < 0) {
		err = map_fd;
		pr_warn("Couldn't find stats map. Maybe xdp-filter is not loaded?\n");
		goto out;
	}
	rec.stats[XDP_DROP].enabled = true;
	rec.stats[XDP_PASS].enabled = true;

	err = stats_collect(map_fd, info.type, &rec);
	if (err)
		goto out;

	printf("CURRENT XDP-FILTER STATUS:\n\n");
	printf("Aggregate per-action statistics:\n");
	err = stats_print_one(&rec);
	if (err)
		goto out;
	printf("\n");

	printf("Loaded on interfaces:\n");
	printf("  %-40s Enabled features\n", "");

	indexes = if_nameindex();
	if (!indexes) {
		err = -errno;
		libbpf_strerror(err, errmsg, sizeof(errmsg));
		pr_warn("Couldn't get list of interfaces: %s\n", errmsg);
		goto out;
	}

	for(idx = indexes; idx->if_index; idx++) {
		struct bpf_prog_info info = {};
		char featbuf[100];
		__u32 feat;

		err = get_xdp_prog_info(idx->if_index, &info);
		if (err && err == -ENOENT)
			continue;
		else if (err)
			goto out;

		feat = find_features(info.name);
		if (feat) {
			print_flags(featbuf, sizeof(featbuf), print_features, feat);
			printf("  %-40s %s\n", idx->if_name, featbuf);
		}
	}
	if_freenameindex(indexes);
	indexes = NULL;
	err = EXIT_SUCCESS;
	printf("\n");

	map_fd = get_pinned_map_fd(pin_root_path, textify(MAP_NAME_PORTS), NULL);
	if (map_fd >= 0) {
		err = print_ports(map_fd);
		if (err)
			goto out;
		printf("\n");
	}
	close(map_fd);
	map_fd = -1;

	err = print_ips(pin_root_path);
	if (err)
		goto out;

	printf("\n");

	map_fd = get_pinned_map_fd(pin_root_path, textify(MAP_NAME_ETHERNET), NULL);
	if (map_fd >= 0) {
		err = print_ethers(map_fd);
		if (err)
			goto out;
	}

	printf("\n");

out:
	if (indexes)
		if_freenameindex(indexes);
	if (map_fd >= 0)
		close(map_fd);
	return err;
}

static const struct pollopt {
	__u32 interval;
} defaults_poll = {};

static struct prog_option poll_options[] = {
	DEFINE_OPTION("interval", OPT_U32, struct pollopt, interval,
		      .short_opt = 'i',
		      .metavar = "<interval>",
		      .help = "Polling interval in milliseconds"),
	END_OPTIONS
};

int do_poll(const void *cfg, const char *pin_root_path)
{
	int err = EXIT_SUCCESS, map_fd = -1;
 	const struct pollopt *opt = cfg;

	map_fd = get_pinned_map_fd(pin_root_path, textify(XDP_STATS_MAP_NAME), NULL);
	if (map_fd < 0) {
		err = map_fd;
		pr_warn("Couldn't find stats map. Maybe xdp-filter is not loaded?\n");
		goto out;
	}

	prog_lock_release(0);
	err = stats_poll(map_fd, pin_root_path, textify(XDP_STATS_MAP_NAME),
			 opt->interval);
	if (err)
		goto out;


out:
	return err;
}

int do_help(const void *cfg, const char *pin_root_path)
{
	fprintf(stderr,
		"Usage: xdp-filter COMMAND [options]\n"
		"\n"
		"COMMAND can be one of:\n"
		"       load        - load xdp-filter on an interface\n"
		"       unload      - unload xdp-filter from an interface\n"
		"       port        - add a port to the blacklist\n"
		"       ip          - add an IP address to the blacklist\n"
		"       ether       - add an Ethernet MAC address to the blacklist\n"
		"       status      - show current xdp-filter status\n"
		"       poll        - poll statistics output\n"
		"       help        - show this help message\n"
		"\n"
		"Use 'xdp-filter COMMAND --help' to see options for each command\n");
	return -1;
}


static const struct prog_command cmds[] = {
	DEFINE_COMMAND(load, "Load xdp-filter on an interface"),
	DEFINE_COMMAND(unload, "Unload xdp-filter from an interface"),
	DEFINE_COMMAND(port, "Add or remove ports from xdp-filter"),
	DEFINE_COMMAND(ip, "Add or remove IP addresses from xdp-filter"),
	DEFINE_COMMAND(ether, "Add or remove MAC addresses from xdp-filter"),
	DEFINE_COMMAND(poll, "Poll xdp-filter statistics"),
	DEFINE_COMMAND_NODEF(status, "Show xdp-filter status"),
	{.name = "help", .func = do_help, .no_cfg = true},
	END_COMMANDS
};

union all_opts {
	struct loadopt load;
	struct unloadopt unload;
	struct portopt port;
	struct ipopt ip;
	struct etheropt ether;
	struct pollopt poll;
};

int main(int argc, char **argv)
{
	if (argc > 1)
		return dispatch_commands(argv[1], argc-1, argv+1,
					 cmds, sizeof(union all_opts),
					 PROG_NAME);

	return do_help(NULL, NULL);
}
