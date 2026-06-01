/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef __PROCESS_NEW_BPF_COMMON_H
#define __PROCESS_NEW_BPF_COMMON_H

/*
 * Common BPF helpers for process_new: PID filtering + unified map update.
 * Included by process_new.bpf.c BEFORE all feature modules.
 * References maps and flags defined in the glue file.
 */

static __always_inline bool is_pid_tracked(void)
{
	if (!filter_pids)
		return true;  /* no filter mode: trace all */
	u32 pid = bpf_get_current_pid_tgid() >> 32;
	return bpf_map_lookup_elem(&tracked_pids, &pid) != NULL;
}

static __always_inline bool is_cgroup_tracked(void)
{
	if (!filter_cgroup)
		return true;
	u64 cgroup_id = bpf_get_current_cgroup_id();
	if (cgroup_id == target_cgroup_id)
		return true;
	if (!filter_cgroup_children)
		return false;
	return bpf_map_lookup_elem(&tracked_cgroups, &cgroup_id) != NULL;
}

static __always_inline bool is_event_tracked(void)
{
	return is_cgroup_tracked() && is_pid_tracked();
}

static __always_inline void update_agg_map(struct agg_key *key, u64 count, u64 bytes)
{
	struct agg_value *val = bpf_map_lookup_elem(&event_agg_map, key);
	if (val) {
		__sync_fetch_and_add(&val->count, count);
		if (bytes)
			__sync_fetch_and_add(&val->total_bytes, bytes);
		val->last_ts = bpf_ktime_get_ns();
		bpf_get_current_comm(val->comm, sizeof(val->comm));
	} else {
		struct agg_value new_val = {};
		new_val.count = count;
		new_val.total_bytes = bytes;
		new_val.first_ts = bpf_ktime_get_ns();
		new_val.last_ts = new_val.first_ts;
		bpf_get_current_comm(new_val.comm, sizeof(new_val.comm));

		if (bpf_map_update_elem(&event_agg_map, key, &new_val, BPF_NOEXIST) < 0) {
			/* map full: bump overflow counter */
			u32 zero = 0;
			u64 *overflow = bpf_map_lookup_elem(&agg_overflow_count, &zero);
			if (overflow)
				__sync_fetch_and_add(overflow, 1);
		}
	}
}

/* Format "fd=N" into a detail buffer without bpf_snprintf.
 * Use fixed offsets only; older verifiers reject variable stack writes here.
 */
static __always_inline void format_fd_detail(char *buf, int buf_len, int fd)
{
	if (buf_len < 11)
		return;

	buf[0] = 'f'; buf[1] = 'd'; buf[2] = '=';

	unsigned int ufd;
	if (fd < 0) {
		buf[3] = '-';
		ufd = (unsigned int)(-fd);
	} else {
		buf[3] = '0';
		ufd = (unsigned int)fd;
	}

	if (ufd > 999999U)
		ufd = 999999U;
	buf[4] = '0' + (ufd / 100000U) % 10U;
	buf[5] = '0' + (ufd / 10000U) % 10U;
	buf[6] = '0' + (ufd / 1000U) % 10U;
	buf[7] = '0' + (ufd / 100U) % 10U;
	buf[8] = '0' + (ufd / 10U) % 10U;
	buf[9] = '0' + ufd % 10U;
	buf[10] = '\0';
}

/* Format "NNN.NNN.NNN.NNN:PPPPP" without variable stack offsets. */
static __always_inline void format_ipv4_port(char *buf, int buf_len, u32 ip, u16 port)
{
	if (buf_len < 22)
		return;

	u8 o0 = ip & 0xFF;
	u8 o1 = (ip >> 8) & 0xFF;
	u8 o2 = (ip >> 16) & 0xFF;
	u8 o3 = (ip >> 24) & 0xFF;
	unsigned int p = port;

	buf[0] = '0' + (o0 / 100U) % 10U;
	buf[1] = '0' + (o0 / 10U) % 10U;
	buf[2] = '0' + o0 % 10U;
	buf[3] = '.';
	buf[4] = '0' + (o1 / 100U) % 10U;
	buf[5] = '0' + (o1 / 10U) % 10U;
	buf[6] = '0' + o1 % 10U;
	buf[7] = '.';
	buf[8] = '0' + (o2 / 100U) % 10U;
	buf[9] = '0' + (o2 / 10U) % 10U;
	buf[10] = '0' + o2 % 10U;
	buf[11] = '.';
	buf[12] = '0' + (o3 / 100U) % 10U;
	buf[13] = '0' + (o3 / 10U) % 10U;
	buf[14] = '0' + o3 % 10U;
	buf[15] = ':';
	buf[16] = '0' + (p / 10000U) % 10U;
	buf[17] = '0' + (p / 1000U) % 10U;
	buf[18] = '0' + (p / 100U) % 10U;
	buf[19] = '0' + (p / 10U) % 10U;
	buf[20] = '0' + p % 10U;
	buf[21] = '\0';
}

#endif /* __PROCESS_NEW_BPF_COMMON_H */
