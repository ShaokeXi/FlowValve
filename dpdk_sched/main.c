/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <unistd.h>
#include <stdint.h>

#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_malloc.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_memcpy.h>
#include <rte_byteorder.h>
#include <rte_branch_prediction.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_sched.h>

#include "main.h"

#define APP_MODE_NONE 0
#define APP_RX_MODE   1
#define APP_WT_MODE   2
#define APP_TX_MODE   4
#define TIMEVAL_TO_MSEC(t)  ((t.tv_sec * 1000) + (t.tv_usec / 1000))

uint8_t interactive = APP_INTERACTIVE_DEFAULT;
uint32_t qavg_period = APP_QAVG_PERIOD;
uint32_t qavg_ntimes = APP_QAVG_NTIMES;

static int
send_main_loop()
{
	uint32_t lcore_id;
	uint32_t i, mode;
	uint32_t rx_idx = 0;
	uint32_t wt_idx = 0;
	uint32_t tx_idx = 0;
	struct thread_conf *rx_confs[MAX_DATA_STREAMS];
	struct thread_conf *wt_confs[MAX_DATA_STREAMS];
	struct thread_conf *tx_confs[MAX_DATA_STREAMS];

	memset(rx_confs, 0, sizeof(rx_confs));
	memset(wt_confs, 0, sizeof(wt_confs));
	memset(tx_confs, 0, sizeof(tx_confs));


	mode = APP_MODE_NONE;
	lcore_id = rte_lcore_id();

	for (i = 0; i < nb_pfc; i++) {
		struct flow_conf *flow = &qos_conf[i];

		if (flow->rx_core == lcore_id) {
			flow->rx_thread.rx_port = flow->rx_port;
			flow->rx_thread.rx_ring =  flow->rx_ring;
			flow->rx_thread.rx_queue = flow->rx_queue;

			rx_confs[rx_idx++] = &flow->rx_thread;

			mode |= APP_RX_MODE;
		}
		if (flow->tx_core == lcore_id) {
			flow->tx_thread.tx_port = flow->tx_port;
			flow->tx_thread.tx_ring =  flow->tx_ring;
			flow->tx_thread.tx_queue = flow->tx_queue;

			tx_confs[tx_idx++] = &flow->tx_thread;

			mode |= APP_TX_MODE;
		}
		if (flow->wt_core == lcore_id) {
			flow->wt_thread.rx_ring =  flow->rx_ring;
			flow->wt_thread.tx_ring =  flow->tx_ring;
			flow->wt_thread.tx_port =  flow->tx_port;
			flow->wt_thread.sched_port =  flow->sched_port;

			wt_confs[wt_idx++] = &flow->wt_thread;

			mode |= APP_WT_MODE;
		}
	}

	if (mode == APP_MODE_NONE) {
		RTE_LOG(INFO, APP, "lcore %u has nothing to do\n", lcore_id);
		return -1;
	}

	if (mode == (APP_RX_MODE | APP_WT_MODE)) {
		RTE_LOG(INFO, APP, "lcore %u was configured for both RX and WT !!!\n",
				 lcore_id);
		return -1;
	}

	RTE_LOG(INFO, APP, "entering main loop on lcore %u\n", lcore_id);
	/* initialize mbuf memory */
	if (mode == APP_RX_MODE) {
		for (i = 0; i < rx_idx; i++) {
			RTE_LOG(INFO, APP, "flow %u lcoreid %u "
					"reading port %"PRIu8"\n",
					i, lcore_id, rx_confs[i]->rx_port);
		}

		app_rx_thread(rx_confs);
	}
	else if (mode == (APP_TX_MODE | APP_WT_MODE)) {
		for (i = 0; i < wt_idx; i++) {
			wt_confs[i]->m_table = rte_malloc("table_wt", sizeof(struct rte_mbuf *)
					* burst_conf.tx_burst, RTE_CACHE_LINE_SIZE);

			if (wt_confs[i]->m_table == NULL)
				rte_panic("flow %u unable to allocate memory buffer\n", i);

			RTE_LOG(INFO, APP, "flow %u lcoreid %u sched+write "
					"port %"PRIu8"\n",
					i, lcore_id, wt_confs[i]->tx_port);
		}

		app_mixed_thread(wt_confs);
	}
	else if (mode == APP_TX_MODE) {
		for (i = 0; i < tx_idx; i++) {
			tx_confs[i]->m_table = rte_malloc("table_tx", sizeof(struct rte_mbuf *)
					* burst_conf.tx_burst, RTE_CACHE_LINE_SIZE);

			if (tx_confs[i]->m_table == NULL)
				rte_panic("flow %u unable to allocate memory buffer\n", i);

			RTE_LOG(INFO, APP, "flow %u lcoreid %u "
					"writing port %"PRIu8"\n",
					i, lcore_id, tx_confs[i]->tx_port);
		}

		app_tx_thread(tx_confs);
	}
	else if (mode == APP_WT_MODE){
		for (i = 0; i < wt_idx; i++) {
			RTE_LOG(INFO, APP, "flow %u lcoreid %u scheduling \n", i, lcore_id);
		}

		app_worker_thread(wt_confs);
	}

	return 0;
}

/* main processing loop */
static void recv_main_loop()
{
	uint32_t core, nb_rx, rx_q = 0, i, time_cnt = 0, dport, prtid, fid;
	uint32_t ip_cnt[4] = { 0 };
    struct tcp_hdr *tcp;
	struct rte_mbuf *pkts_burst[burst_conf.rx_burst];
	struct timeval cur_tv, prev_tv;
    uint64_t diff_ts, recv_pkts = 0, recv_bytes = 0;
	uint64_t lat[4] = { 0 }, avg_lat[4] = { 0 }, lat_nb[4] = { 0 }, ticks;
	uint32_t interval = 1000;
	tstamp_t *tstamp;

    core = rte_lcore_id();
	prtid = core - 1;
	ticks = rte_get_timer_hz() / 1000000;
	// timer
	gettimeofday(&cur_tv, NULL);
    prev_tv = cur_tv;

	while (1) {
		while ((nb_rx = rte_eth_rx_burst(prtid, rx_q, pkts_burst, burst_conf.rx_burst)) > 0) {
			for (i = 0; i < nb_rx; i++) {
				// per-packet processing
				uint8_t *ptr = (uint8_t *)pkts_burst[i]->buf_addr + pkts_burst[i]->data_off;
				ptr += (sizeof(struct ether_hdr) + sizeof(struct ipv4_hdr));
				tcp = (struct tcp_hdr *)ptr;
				dport = ntohs(tcp->dst_port);
				if (dport >= 5001 && dport <= 5004) {
					fid = dport - 5001;
					ip_cnt[fid]++;
					// calculate latency
					ptr += (sizeof(struct tcp_hdr));
					ptr = RTE_PTR_ALIGN_CEIL(ptr, sizeof(uint64_t));
					tstamp = (tstamp_t *)ptr;
					if (tstamp->magic == TSTAMP_MAGIC) {
						lat[fid] = (rte_rdtsc_precise() - tstamp->timestamp);
						avg_lat[fid] += lat[fid];
						lat_nb[fid]++;
					}
				}

				recv_bytes += pkts_burst[i]->pkt_len;
				rte_pktmbuf_free(pkts_burst[i]);
			}
			recv_pkts += nb_rx;
		}
		gettimeofday(&cur_tv, NULL);
		diff_ts = TIMEVAL_TO_MSEC(cur_tv) - TIMEVAL_TO_MSEC(prev_tv);
		if (diff_ts > interval) {
			for (i = 0; i < 4; i++) {
				lat[i] = lat_nb[i] > 0 ? avg_lat[i]/lat_nb[i]/ticks : 0;
			}
			RTE_LOG(INFO, APP, "[CPU %d] %d latency flow1 %d us, flow2 %d us, flow3 %d us, flow4 %d us\n", 
				core, time_cnt, lat[0], lat[1], lat[2], lat[3]);
			RTE_LOG(INFO, APP, "[CPU %d] %d RX %ld pps, %.2f Gbps, flow1 %ld pps, flow2 %ld pps, flow3 %ld pps, flow4 %ld pps\n", 
				core, time_cnt, recv_pkts*1000/diff_ts, recv_bytes*8.0/diff_ts/1000000, ip_cnt[0]*1000/diff_ts, ip_cnt[1]*1000/diff_ts, ip_cnt[2]*1000/diff_ts, ip_cnt[3]*1000/diff_ts);
			time_cnt++;
			prev_tv = cur_tv;
			recv_pkts = 0;
			recv_bytes = 0;
			memset(ip_cnt, 0, sizeof(ip_cnt));
			memset(avg_lat, 0, sizeof(avg_lat));
			memset(lat_nb, 0, sizeof(lat_nb));
		}
	}	
	
}

/* main processing loop */
static int
app_main_loop(__attribute__((unused))void *dummy)
{
	// specify one core to receive traffic from the Netronome virtual port
	uint32_t core = rte_lcore_id();
	if (core <= 4) {
		recv_main_loop();
	}
	else {
		send_main_loop();
	}
	return 0;
}

void
app_stat(int cnt)
{
	uint32_t i;
	struct rte_eth_stats r_stats, t_stats;
	static struct rte_eth_stats rx_stats[MAX_DATA_STREAMS];
	static struct rte_eth_stats tx_stats[MAX_DATA_STREAMS];
	static struct thread_stat wt_stats[MAX_DATA_STREAMS];	

	/* print statistics */
	for(i = 0; i < nb_pfc; i++) {
		struct flow_conf *flow = &qos_conf[i];

		rte_eth_stats_get(flow->rx_port, &r_stats);
		// printf("\nRX port %"PRIu8": rx: %"PRIu64 " err: %"PRIu64
				// " no_mbuf: %"PRIu64 "\n",
				// flow->rx_port,
				// stats.ipackets - rx_stats[i].ipackets,
				// stats.ierrors - rx_stats[i].ierrors,
				// stats.rx_nombuf - rx_stats[i].rx_nombuf);
		memcpy(&rx_stats[i], &r_stats, sizeof(r_stats));

		rte_eth_stats_get(flow->tx_port, &t_stats);
		// printf("RX port %"PRIu8": rx: %"PRIu64 " err: %"PRIu64" no_mbuf: %"PRIu64" TX port %"PRIu8": tx: %" PRIu64 " err: %" PRIu64 "\n",
				// flow->rx_port,
				// r_stats.ipackets - rx_stats[i].ipackets,
				// r_stats.ierrors - rx_stats[i].ierrors,
				// r_stats.rx_nombuf - rx_stats[i].rx_nombuf,
		printf("QoS rx: %d %"PRIu64 " drop: %"PRIu64" TX port %"PRIu8": tx: %" PRIu64 " err: %" PRIu64 "\n", cnt,
				flow->wt_thread.stat.nb_rx - wt_stats[i].nb_rx,
				flow->wt_thread.stat.nb_drop - wt_stats[i].nb_drop,
				flow->tx_port,
				t_stats.opackets - tx_stats[i].opackets,
				t_stats.oerrors - tx_stats[i].oerrors);
		memcpy(&tx_stats[i], &t_stats, sizeof(t_stats));
		memcpy(&(flow->wt_thread.stat), &wt_stats[i], sizeof(struct thread_stat));

#if APP_COLLECT_STAT
		// printf("-------+------------+------------+\n");
		// printf("       |  received  |   dropped  |\n");
		// printf("-------+------------+------------+\n");
		// printf("  RX   | %10" PRIu64 " | %10" PRIu64 " |\n",
			// flow->rx_thread.stat.nb_rx,
			// flow->rx_thread.stat.nb_drop);
		// printf("QOS+TX | %10" PRIu64 " | %10" PRIu64 " |   pps: %"PRIu64 " \n",
			// flow->wt_thread.stat.nb_rx,
			// flow->wt_thread.stat.nb_drop,
			// flow->wt_thread.stat.nb_rx - flow->wt_thread.stat.nb_drop);
		// printf("-------+------------+------------+\n");
		// 
		// memset(&flow->rx_thread.stat, 0, sizeof(struct thread_stat));
		// memset(&flow->wt_thread.stat, 0, sizeof(struct thread_stat));
#endif
	}
}

int
main(int argc, char **argv)
{
	int ret, cnt = 0;

	ret = app_parse_args(argc, argv);
	if (ret < 0)
		return -1;

	ret = app_init();
	if (ret < 0)
		return -1;

	/* launch per-lcore init on every lcore */
	rte_eal_mp_remote_launch(app_main_loop, NULL, SKIP_MASTER);

	if (interactive) {
		sleep(1);
		prompt();
	}
	else {
		/* print statistics every second */
		while(1) {
			sleep(1);
			app_stat(cnt++);
		}
		// sleep(10);

	}

	return 0;
}
