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

#include <stdint.h>

#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_malloc.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_memcpy.h>
#include <rte_byteorder.h>
#include <rte_branch_prediction.h>
#include <rte_sched.h>
#include <rte_ip.h>
#include <rte_tcp.h>

#include "main.h"
#include "pcap.h"

/*
 * QoS parameters are encoded as follows:
 *		Outer VLAN ID defines subport
 *		Inner VLAN ID defines pipe
 *		Destination IP 0.0.XXX.0 defines traffic class
 *		Destination IP host (0.0.0.XXX) defines queue
 * Values below define offset to each field from start of frame
 */
#define SUBPORT_OFFSET	7
#define PIPE_OFFSET		9
#define TC_OFFSET		20
#define QUEUE_OFFSET	20
#define COLOR_OFFSET	19

// static inline int
// get_pkt_sched(struct rte_mbuf *m, uint32_t *subport, uint32_t *pipe,
// 			uint32_t *traffic_class, uint32_t *queue, uint32_t *color)
// {
// 	uint16_t *pdata = rte_pktmbuf_mtod(m, uint16_t *);

// 	*subport = (rte_be_to_cpu_16(pdata[SUBPORT_OFFSET]) & 0x0FFF) &
// 			(port_params.n_subports_per_port - 1); /* Outer VLAN ID*/
// 	*pipe = (rte_be_to_cpu_16(pdata[PIPE_OFFSET]) & 0x0FFF) &
// 			(port_params.n_pipes_per_subport - 1); /* Inner VLAN ID */
// 	*traffic_class = (pdata[QUEUE_OFFSET] & 0x0F) &
// 			(RTE_SCHED_TRAFFIC_CLASSES_PER_PIPE - 1); /* Destination IP */
// 	*queue = ((pdata[QUEUE_OFFSET] >> 8) & 0x0F) &
// 			(RTE_SCHED_QUEUES_PER_TRAFFIC_CLASS - 1) ; /* Destination IP */
// 	*color = pdata[COLOR_OFFSET] & 0x03; 	/* Destination IP */

// 	return 0;
// }

// void
// app_rx_thread(struct thread_conf **confs)
// {
	// uint32_t i, nb_rx;
	// struct rte_mbuf *rx_mbufs[burst_conf.rx_burst] __rte_cache_aligned;
	// struct thread_conf *conf;
	// int conf_idx = 0;
// 
	// uint32_t subport;
	// uint32_t pipe;
	// uint32_t traffic_class;
	// uint32_t queue;
	// uint32_t color;
// 
	// while ((conf = confs[conf_idx])) {
		// nb_rx = rte_eth_rx_burst(conf->rx_port, conf->rx_queue, rx_mbufs,
				// burst_conf.rx_burst);
// 
		// if (likely(nb_rx != 0)) {
			// APP_STATS_ADD(conf->stat.nb_rx, nb_rx);
// 
			// for(i = 0; i < nb_rx; i++) {
				// get_pkt_sched(rx_mbufs[i],
						// &subport, &pipe, &traffic_class, &queue, &color);
				// rte_sched_port_pkt_write(rx_mbufs[i], subport, pipe,
						// traffic_class, queue, (enum rte_meter_color) color);
			// }
// 
			// if (unlikely(rte_ring_sp_enqueue_bulk(conf->rx_ring,
					// (void **)rx_mbufs, nb_rx, NULL) == 0)) {
				// for(i = 0; i < nb_rx; i++) {
					// rte_pktmbuf_free(rx_mbufs[i]);
// 
					// APP_STATS_ADD(conf->stat.nb_drop, 1);
				// }
			// }
		// }
		// conf_idx++;
		// if (confs[conf_idx] == NULL)
			// conf_idx = 0;
	// }
// }

// define your own classifier
static inline int
get_pkt_sched(struct rte_mbuf *m, uint32_t *subport, uint32_t *pipe,
			uint32_t *traffic_class, uint32_t *queue, uint32_t *color)
{
	uint16_t *pdata = rte_pktmbuf_mtod(m, uint16_t *);

	*subport = 0;
	*pipe = 0;
	*traffic_class = (pdata[QUEUE_OFFSET] & 0x0F) &
			(RTE_SCHED_TRAFFIC_CLASSES_PER_PIPE - 1); /* Destination IP */
	*queue = ((pdata[QUEUE_OFFSET] >> 8) & 0x0F) &
			(RTE_SCHED_QUEUES_PER_TRAFFIC_CLASS - 1) ; /* Destination IP */
	*color = pdata[COLOR_OFFSET] & 0x03; 	/* Destination IP */

	return 0;
}

// read from pcap file not actually receive from ports
void app_rx_thread(struct thread_conf **confs)
{
	uint32_t i, nb_rx;
	struct rte_mempool *mp[MAX_DATA_STREAMS] = { NULL };
	struct rte_mbuf *rx_mbufs[burst_conf.rx_burst] __rte_cache_aligned, *m;
	struct thread_conf *conf;
	int conf_idx = 0, core;

	uint32_t subport;
	uint32_t pipe;
	uint32_t traffic_class;
	uint32_t queue;
	uint32_t color;

	pcap_info_t *pcap;
	char pfile[32];
	union _data *d;

	core = rte_lcore_id();

	while ((conf = confs[conf_idx]))
	{
		snprintf(pfile, sizeof(pfile), "%s_%d.pcap", pcap_file, conf->rx_port-vf_num);
		pcap = _pcap_open(pfile);
    	if (pcap == NULL) {
    	    RTE_LOG(ERR, USER1, "[CPU %d] Cannot parse pcap file %s.\n", core, pfile);
    	}
    	mp[conf_idx] = pcap_parse(pcap, conf->rx_port);
		conf_idx++;
		if (confs[conf_idx] == NULL) {
			conf_idx = 0;
			break;
		}
	}

	while ((conf = confs[conf_idx])) {
		for (i = 0; i < burst_conf.rx_burst; i++) {
			if ((rx_mbufs[i] = rte_pktmbuf_alloc(mp[conf_idx])) == NULL) {
				RTE_LOG(ERR, USER1, "[CPU %d] Cannot alloc mbuf.\n", core);
                exit(EXIT_FAILURE);
			}
			m = rx_mbufs[i];	
			d = (union _data *)&m->udata64;
			m->data_len = d->data_len;
			m->pkt_len = d->pkt_len;
		}
		nb_rx = burst_conf.rx_burst;

		if (likely(nb_rx != 0)) {
			APP_STATS_ADD(conf->stat.nb_rx, nb_rx);

			for(i = 0; i < nb_rx; i++) {
				get_pkt_sched(rx_mbufs[i],
						&subport, &pipe, &traffic_class, &queue, &color);
				rte_sched_port_pkt_write(rx_mbufs[i], subport, pipe,
						traffic_class, queue, (enum rte_meter_color) color);
			}

			if (unlikely(rte_ring_sp_enqueue_bulk(conf->rx_ring,
					(void **)rx_mbufs, nb_rx, NULL) == 0)) {
				for(i = 0; i < nb_rx; i++) {
					rte_pktmbuf_free(rx_mbufs[i]);

					APP_STATS_ADD(conf->stat.nb_drop, 1);
				}
			}
		}

		for (i = 0; i < nb_rx; i++) {
			rte_pktmbuf_free(rx_mbufs[i]);
		}

		conf_idx++;
		if (confs[conf_idx] == NULL)
			conf_idx = 0;
	}
}


/* Send the packet to an output interface
 * For performance reason function returns number of packets dropped, not sent,
 * so 0 means that all packets were sent successfully
 */

static inline void
app_send_burst(struct thread_conf *qconf)
{
	struct rte_mbuf **mbufs;
	uint32_t n, ret;

	mbufs = (struct rte_mbuf **)qconf->m_table;
	n = qconf->n_mbufs;

	do {
		ret = rte_eth_tx_burst(qconf->tx_port, qconf->tx_queue, mbufs, (uint16_t)n);
		/* we cannot drop the packets, so re-send */
		/* update number of packets to be sent */
		n -= ret;
		mbufs = (struct rte_mbuf **)&mbufs[ret];
	} while (n);
}

static inline void
app_tstamp_apply(struct rte_mbuf *m)
{
	tstamp_t *tstamp;
	char *p;

	p = rte_pktmbuf_mtod(m, char *);
	p += sizeof(struct ether_hdr);
	p += sizeof(struct ipv4_hdr);
	p += sizeof(struct tcp_hdr);

	/* Force pointer to be aligned correctly */
	p = RTE_PTR_ALIGN_CEIL(p, sizeof(uint64_t));
	tstamp = (tstamp_t *)p;
	tstamp->timestamp  = rte_rdtsc_precise();
	tstamp->magic      = TSTAMP_MAGIC;
}

/* Send the packet to an output interface */
static void
app_send_packets(struct thread_conf *qconf, struct rte_mbuf **mbufs, uint32_t nb_pkt)
{
	uint32_t i, len;

	len = qconf->n_mbufs;
	for(i = 0; i < nb_pkt; i++) {
		qconf->m_table[len] = mbufs[i];
		len++;
		/* enough pkts to be sent */
		if (unlikely(len == burst_conf.tx_burst)) {
			qconf->n_mbufs = len;
			app_send_burst(qconf);
			len = 0;
		}
	}

	qconf->n_mbufs = len;
}

void
app_tx_thread(struct thread_conf **confs)
{
	struct rte_mbuf *mbufs[burst_conf.qos_dequeue];
	struct thread_conf *conf;
	int conf_idx = 0;
	int retval;
	const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * BURST_TX_DRAIN_US;

	while ((conf = confs[conf_idx])) {
		retval = rte_ring_sc_dequeue_bulk(conf->tx_ring, (void **)mbufs,
					burst_conf.qos_dequeue, NULL);
		if (likely(retval != 0)) {
			app_send_packets(conf, mbufs, burst_conf.qos_dequeue);

			conf->counter = 0; /* reset empty read loop counter */
		}

		conf->counter++;

		/* drain ring and TX queues */
		if (unlikely(conf->counter > drain_tsc)) {
			/* now check is there any packets left to be transmitted */
			if (conf->n_mbufs != 0) {
				app_send_burst(conf);

				conf->n_mbufs = 0;
			}
			conf->counter = 0;
		}

		conf_idx++;
		if (confs[conf_idx] == NULL)
			conf_idx = 0;
	}
}


void
app_worker_thread(struct thread_conf **confs)
{
	struct rte_mbuf *mbufs[burst_conf.ring_burst];
	struct thread_conf *conf;
	int conf_idx = 0;

	while ((conf = confs[conf_idx])) {
		uint32_t i, nb_pkt;

		/* Read packet from the ring */
		nb_pkt = rte_ring_sc_dequeue_burst(conf->rx_ring, (void **)mbufs,
					burst_conf.ring_burst, NULL);
		if (likely(nb_pkt)) {
			for (i = 0; i < nb_pkt; i++) {
				/* set tstamp */
				app_tstamp_apply(mbufs[i]);
			}
			int nb_sent = rte_sched_port_enqueue(conf->sched_port, mbufs,
					nb_pkt);

			APP_STATS_ADD(conf->stat.nb_drop, nb_pkt - nb_sent);
			APP_STATS_ADD(conf->stat.nb_rx, nb_pkt);
		}

		nb_pkt = rte_sched_port_dequeue(conf->sched_port, mbufs,
					burst_conf.qos_dequeue);
		if (likely(nb_pkt > 0))
			while (rte_ring_sp_enqueue_bulk(conf->tx_ring,
					(void **)mbufs, nb_pkt, NULL) == 0)
				; /* empty body */

		conf_idx++;
		if (confs[conf_idx] == NULL)
			conf_idx = 0;
	}
}


void
app_mixed_thread(struct thread_conf **confs)
{
	struct rte_mbuf *mbufs[burst_conf.ring_burst];
	struct thread_conf *conf;
	int conf_idx = 0;
	const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * BURST_TX_DRAIN_US;

	while ((conf = confs[conf_idx])) {
		uint32_t nb_pkt;

		/* Read packet from the ring */
		nb_pkt = rte_ring_sc_dequeue_burst(conf->rx_ring, (void **)mbufs,
					burst_conf.ring_burst, NULL);
		if (likely(nb_pkt)) {
			int nb_sent = rte_sched_port_enqueue(conf->sched_port, mbufs,
					nb_pkt);

			APP_STATS_ADD(conf->stat.nb_drop, nb_pkt - nb_sent);
			APP_STATS_ADD(conf->stat.nb_rx, nb_pkt);
		}


		nb_pkt = rte_sched_port_dequeue(conf->sched_port, mbufs,
					burst_conf.qos_dequeue);
		if (likely(nb_pkt > 0)) {
			app_send_packets(conf, mbufs, nb_pkt);

			conf->counter = 0; /* reset empty read loop counter */
		}

		conf->counter++;

		/* drain ring and TX queues */
		if (unlikely(conf->counter > drain_tsc)) {

			/* now check is there any packets left to be transmitted */
			if (conf->n_mbufs != 0) {
				app_send_burst(conf);

				conf->n_mbufs = 0;
			}
			conf->counter = 0;
		}

		conf_idx++;
		if (confs[conf_idx] == NULL)
			conf_idx = 0;
	}
}