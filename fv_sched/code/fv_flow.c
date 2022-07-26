#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include "pcap.h"

#define MAX_ETHPORTS 16
#define MIN_PKT_SIZE 64
#define MAX_PKT_SIZE 1518
#define MBUF_INVALID_PORT UINT8_MAX
#define BUF_SIZE 2048
#define NB_MBUF 1024
#define MEMPOOL_CACHE_SIZE 256
#define BURST_SIZE 32
#define MAX_MBUFS_PER_PORT 16384
#define MBUF_SIZE (BUF_SIZE + RTE_PKTMBUF_HEADROOM)
#define TIMEVAL_TO_MSEC(t)  ((t.tv_sec * 1000) + (t.tv_usec / 1000))
#define PRT_NUM 12
#define OFF_MF 0x2000
#define OFF_MASK 0x1fff

#define PCAP_MAGIC_NUMBER   0xa1b2c3d4
#define PCAP_MAJOR_VERSION  2
#define PCAP_MINOR_VERSION  4
#ifndef BIG_ENDIAN
#define BIG_ENDIAN      0x4321
#endif
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN   0x1234
#endif

/*
 * RX and TX Prefetch, Host, and Write-back threshold values should be
 * carefully set for optimal performance. Consult the network
 * controller's datasheet and supporting DPDK documentation for guidance
 * on how these parameters should be set.
 */
#define RX_PTHRESH 			8 /**< Default values of RX prefetch threshold reg. */
#define RX_HTHRESH 			8 /**< Default values of RX host threshold reg. */
#define RX_WTHRESH 			4 /**< Default values of RX write-back threshold reg. */

/*
 * These default values are optimized for use with the Intel(R) 82599 10 GbE
 * Controller and the DPDK ixgbe PMD. Consider using other values for other
 * network controllers and/or network drivers.
 */
#define TX_PTHRESH 			36 /**< Default values of TX prefetch threshold reg. */
#define TX_HTHRESH			0  /**< Default values of TX host threshold reg. */
#define TX_WTHRESH			0  /**< Default values of TX write-back threshold reg. */

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RTE_TEST_RX_DESC_DEFAULT	128
#define RTE_TEST_TX_DESC_DEFAULT	128

#define TSTAMP_MAGIC 't'|'s'

/* device info */
static struct rte_eth_dev_info dev_info[MAX_ETHPORTS];
/* port configure */
static struct rte_eth_conf port_conf = {
	// .rxmode = {
		// .mq_mode	= 	ETH_MQ_RX_RSS,
		// .max_rx_pkt_len = 	ETHER_MAX_LEN,
		// .split_hdr_size = 	0,
		// .header_split   = 	0, /**< Header Split disabled */
		// .hw_ip_checksum = 	1, /**< IP checksum offload enabled */
		// .hw_vlan_filter = 	0, /**< VLAN filtering disabled */
		// .jumbo_frame    = 	0, /**< Jumbo Frame Support disabled */
		// .hw_strip_crc   = 	1, /**< CRC stripped by hardware */
	// },
	// .rx_adv_conf = {
		// .rss_conf = {
			// .rss_key = 	NULL,
			// .rss_hf = 	ETH_RSS_TCP | ETH_RSS_UDP |
					// ETH_RSS_IP | ETH_RSS_L2_PAYLOAD
		// },
	// },	
	.rxmode = {
		.mq_mode	= 	ETH_MQ_RX_NONE,
		.max_rx_pkt_len = 	ETHER_MAX_LEN,
	},
	.txmode = {
		.mq_mode = 		ETH_MQ_TX_NONE,
	},
};
/* rx configure */
static const struct rte_eth_rxconf rx_conf = {
	.rx_thresh = {
		.pthresh = 		RX_PTHRESH, /* RX prefetch threshold reg */
		.hthresh = 		RX_HTHRESH, /* RX host threshold reg */
		.wthresh = 		RX_WTHRESH, /* RX write-back threshold reg */
	},
	.rx_free_thresh = 	    32,
};
/* tx configure */
static const struct rte_eth_txconf tx_conf = {
	.tx_thresh = {
		.pthresh = 		TX_PTHRESH, /* TX prefetch threshold reg */
		.hthresh = 		TX_HTHRESH, /* TX host threshold reg */
		.wthresh = 		TX_WTHRESH, /* TX write-back threshold reg */
	},
	.tx_free_thresh = 		0, /* Use PMD default values */
	.tx_rs_thresh = 		0, /* Use PMD default values */
	/*
	 * As the example won't handle mult-segments and offload cases,
	 * set the flag by default.
	 */
	.txq_flags = 			0x0,
};

typedef struct pcap_hdr_s {
	uint32_t magic_number;	/**< magic number */
	uint16_t version_major;	/**< major version number */
	uint16_t version_minor;	/**< minor version number */
	int32_t thiszone;	/**< GMT to local correction */
	uint32_t sigfigs;	/**< accuracy of timestamps */
	uint32_t snaplen;	/**< max length of captured packets, in octets */
	uint32_t network;	/**< data link type */
} pcap_hdr_t;

typedef struct pcaprec_hdr_s {
	uint32_t ts_sec;	/**< timestamp seconds */
	uint32_t ts_usec;	/**< timestamp microseconds */
	uint32_t incl_len;	/**< number of octets of packet saved in file */
	uint32_t orig_len;	/**< actual length of packet */
} pcaprec_hdr_t;

typedef struct pcap_info_s {
	FILE      *fd;		/**< File descriptor */
	char      *filename;	/**< allocated string for filename of pcap */
	uint32_t endian;	/**< Endian flag value */
	uint32_t pkt_size;	/**< Average packet size */
	uint32_t pkt_count;	/**< pcap count of packets */
	uint32_t pkt_idx;	/**< Index into the current PCAP file */
	pcap_hdr_t info;	/**< information on the PCAP file */
} pcap_info_t;

typedef struct pcap_pkt_data_s {	/**< Keep these in this order as pkt_seq_t mirrors the first three objects */
	uint8_t           *buffAddr;	/**< Start of buffer virtual address */
	uint8_t           *virtualAddr;	/**< Pointer to the start of the packet data */
	phys_addr_t physAddr;		/**< Packet physical address */
	uint32_t size;			/**< Real packet size (hdr.incl_len) */
	pcaprec_hdr_t hdr;		/**< Packet header from the .pcap file for each packet */
} pcap_pkt_data_t;

union _data {
	uint64_t udata;
	struct {
		uint16_t data_len;
		uint16_t buf_len;
		uint32_t pkt_len;
	};
};

typedef struct {
	uint64_t timestamp;
	uint8_t magic;
} tstamp_t;

static volatile bool force_quit[5];
static int pf = 4;
static int vf = 4;
static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;
static int exec_time = 10;
static uint32_t interval = 1000;
static char pfile[32];

/* packet memory pools for storing packet bufs */
static struct rte_mempool *pktmbuf_pool[PRT_NUM] = {NULL};

static __inline__ void
_pcap_convert(pcap_info_t *pcap, pcaprec_hdr_t *pHdr)
{
	if (pcap->endian == BIG_ENDIAN) {
		pHdr->incl_len  = ntohl(pHdr->incl_len);
		pHdr->orig_len  = ntohl(pHdr->orig_len);
		pHdr->ts_sec    = ntohl(pHdr->ts_sec);
		pHdr->ts_usec   = ntohl(pHdr->ts_usec);
	}
}

static void _pcap_info(pcap_info_t *pcap, uint16_t port, int flag)
{
	printf("\nPCAP file for port %d: %s\n", port, pcap->filename);
	printf("  magic: %08x,", pcap->info.magic_number);
	printf(" Version: %d.%d,",
	       pcap->info.version_major,
	       pcap->info.version_minor);
	printf(" Zone: %d,", pcap->info.thiszone);
	printf(" snaplen: %d,", pcap->info.snaplen);
	printf(" sigfigs: %d,", pcap->info.sigfigs);
	printf(" network: %d", pcap->info.network);
	printf(" Endian: %s\n", pcap->endian == BIG_ENDIAN ? "Big" : "Little");
	if (flag)
		printf("  Packet count: %d\n", pcap->pkt_count);
	printf("\n");
	fflush(stdout);
}

static void _pcap_close(pcap_info_t *pcap)
{
	if (pcap == NULL)
		return;

	if (pcap->fd)
		fclose(pcap->fd);
	if (pcap->filename)
		free(pcap->filename);
	rte_free(pcap);
}

static pcap_info_t *_pcap_open(char *filename, uint16_t port)
{
	pcap_info_t   *pcap = NULL;

	if (filename == NULL) {
		printf("%s: filename is NULL\n", __func__);
		goto leave;
	}

	pcap = (pcap_info_t *)rte_malloc_socket("PCAP info",
					 sizeof(pcap_info_t),
					 RTE_CACHE_LINE_SIZE,
					 rte_socket_id());
	if (pcap == NULL) {
		printf("%s: malloc failed for pcap_info_t structure\n",
		       __func__);
		goto leave;
	}
	memset((char *)pcap, 0, sizeof(pcap_info_t));

	pcap->fd = fopen((const char *)filename, "r");
	if (pcap->fd == NULL) {
		printf("%s: failed for (%s)\n", __func__, filename);
		goto leave;
	}

	if (fread(&pcap->info, 1, sizeof(pcap_hdr_t),
		  pcap->fd) != sizeof(pcap_hdr_t) ) {
		printf("%s: failed to read the file header\n", __func__);
		goto leave;
	}

	/* Default to little endian format. */
	pcap->endian    = LITTLE_ENDIAN;
	pcap->filename  = strdup(filename);

	/* Make sure we have a valid PCAP file for Big or Little Endian formats. */
	if ( (pcap->info.magic_number != PCAP_MAGIC_NUMBER) &&
	     (pcap->info.magic_number != ntohl(PCAP_MAGIC_NUMBER)) ) {
		printf("%s: Magic Number does not match!\n", __func__);
		fflush(stdout);
		goto leave;
	}

	/* Convert from big-endian to little-endian. */
	if (pcap->info.magic_number == ntohl(PCAP_MAGIC_NUMBER) ) {
		printf(
			"PCAP: Big Endian file format found, converting to little endian\n");
		pcap->endian                = BIG_ENDIAN;
		pcap->info.magic_number     = ntohl(pcap->info.magic_number);
		pcap->info.network          = ntohl(pcap->info.network);
		pcap->info.sigfigs          = ntohl(pcap->info.sigfigs);
		pcap->info.snaplen          = ntohl(pcap->info.snaplen);
		pcap->info.thiszone         = ntohl(pcap->info.thiszone);
		pcap->info.version_major    = ntohs(pcap->info.version_major);
		pcap->info.version_minor    = ntohs(pcap->info.version_minor);
	}
	_pcap_info(pcap, port, 0);

	return pcap;

leave:
	_pcap_close(pcap);
	fflush(stdout);

	return NULL;
}

static void _pcap_rewind(pcap_info_t *pcap)
{
	if (pcap == NULL)
		return;

	/* Rewind to the beginning */
	rewind(pcap->fd);

	/* Seek past the pcap header */
	(void)fseek(pcap->fd, sizeof(pcap_hdr_t), SEEK_SET);
}

static size_t _pcap_read(pcap_info_t *pcap,
	   pcaprec_hdr_t *pHdr,
	   char *pktBuff,
	   uint32_t bufLen)
{
	do {
		if (fread(pHdr, 1, sizeof(pcaprec_hdr_t),
			  pcap->fd) != sizeof(pcaprec_hdr_t) )
			return 0;

		/* Convert the packet header to the correct format. */
		_pcap_convert(pcap, pHdr);

		/* Skip packets larger then the buffer size. */
		if (pHdr->incl_len > bufLen) {
			(void)fseek(pcap->fd, pHdr->incl_len, SEEK_CUR);
			return pHdr->incl_len;
		}

		return fread(pktBuff, 1, pHdr->incl_len, pcap->fd);
	} while (1);
}

/* Callback routine to construct PCAP packet buffers. */
static void pcap_mbuf_ctor(struct rte_mempool *mp, void *opaque_arg, void *_m, unsigned i)
{
    struct rte_mbuf *m = _m;
	uint32_t mbuf_size, buf_len, priv_size = 0;
	pcaprec_hdr_t hdr;
	ssize_t len = -1;
	char buffer[MBUF_SIZE];
	pcap_info_t *pcap = (pcap_info_t *)opaque_arg;

    priv_size = rte_pktmbuf_priv_size(mp);
    buf_len = rte_pktmbuf_data_room_size(mp);
    mbuf_size = sizeof(struct rte_mbuf) + priv_size;
    memset(m, 0, mbuf_size);

	/* start of buffer is after mbuf structure and priv data */
	m->priv_size = priv_size;
	m->buf_addr = (char *)m + mbuf_size;
	m->buf_physaddr = rte_mempool_virt2phy(mp, m) + mbuf_size;
	m->buf_len = (uint16_t)buf_len;

	/* keep some headroom between start of buffer and data */
	m->data_off = RTE_MIN(RTE_PKTMBUF_HEADROOM, m->buf_len);

	/* init some constant fields */
	m->pool         = mp;
	m->nb_segs      = 1;
	m->port         = MBUF_INVALID_PORT;
	rte_mbuf_refcnt_set(m, 1);
	m->next		= NULL;

	for (;; ) {
		union _data *d = (union _data *)&m->udata64;

		if (unlikely(_pcap_read(pcap, &hdr, buffer,
					sizeof(buffer)) <= 0) ) {
			_pcap_rewind(pcap);
			continue;
		}

		len = hdr.incl_len;

		/* Adjust the packet length if not a valid size. */
		if (len < MIN_PKT_SIZE)
			len = MIN_PKT_SIZE;
		else if (len > MAX_PKT_SIZE)
			len = MAX_PKT_SIZE;

		m->data_len = len;
		m->pkt_len  = len;

        d->pkt_len = len;
		d->data_len = len;
		d->buf_len = m->buf_len;

		rte_memcpy((uint8_t *)m->buf_addr + m->data_off, buffer, len);
		break;
	}
}

static struct rte_mempool *pcap_parse(pcap_info_t *pcap, int port)
{
    pcaprec_hdr_t hdr;
    struct rte_mempool *mp = NULL;
	uint32_t elt_count, len, i;
	uint64_t pkt_sizes = 0;
	char buffer[MBUF_SIZE];
	char name[RTE_MEMZONE_NAMESIZE];

	if (pcap == NULL)
		return NULL;

	_pcap_rewind(pcap);		/* Rewind the file is needed */

    snprintf(name, sizeof(name), "%s%d", "PCAP TX", port);
	pkt_sizes = elt_count = i = 0;

	/* The pcap_open left the file pointer to the first packet. */
	while (_pcap_read(pcap, &hdr, buffer, sizeof(buffer)) > 0) {
		/* Skip any jumbo packets or packets that are too small */
		len = hdr.incl_len;

		if (len < (uint32_t)MIN_PKT_SIZE)
			len = MIN_PKT_SIZE;
		else if (len > (uint32_t)MAX_PKT_SIZE)
			len = MAX_PKT_SIZE;

		elt_count++;
		pkt_sizes += len;
	}

	/* If count is greater then zero then we allocate and create the PCAP mbuf pool. */
	if (elt_count > 0) {
		/* Create the average size packet */
		pcap->pkt_size    = (pkt_sizes / elt_count);
		pcap->pkt_count   = elt_count;
		pcap->pkt_idx     = 0;

		_pcap_rewind(pcap);

		/* Round up the count and size to allow for TX ring size. */
		if (elt_count < MAX_MBUFS_PER_PORT)
			elt_count = MAX_MBUFS_PER_PORT;
		elt_count = rte_align32pow2(elt_count);

		mp = rte_mempool_create(
            name,
            elt_count,
            MBUF_SIZE,
            0,
            sizeof(struct rte_pktmbuf_pool_private),
            rte_pktmbuf_pool_init,
            NULL,
            pcap_mbuf_ctor,
            (void *)pcap,
            rte_lcore_to_socket_id(0), MEMPOOL_F_SP_PUT|MEMPOOL_F_SC_GET);

		if (mp == NULL)
			RTE_LOG(ERR, USER1, "Cannot init port %d for %d PCAP packets", port, pcap->pkt_count);
	}

	return mp;
}

static void check_all_ports_link_status(uint8_t port_num, uint32_t port_mask)
{
#define CHECK_INTERVAL 			100 /* 100ms */
#define MAX_CHECK_TIME 			90 /* 9s (90 * 100ms) in total */

	uint8_t portid, count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;

	printf("\nChecking link status");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		all_ports_up = 1;
		for (portid = 0; portid < port_num; portid++) {
			if ((port_mask & (1 << portid)) == 0)
				continue;
			memset(&link, 0, sizeof(link));
			rte_eth_link_get_nowait(portid, &link);
			/* print link status if flag set */
			if (print_flag == 1) {
				if (link.link_status)
					printf("Port %d Link Up - speed %u "
						"Mbps - %s\n", (uint8_t)portid,
						(unsigned)link.link_speed,
				(link.link_duplex == ETH_LINK_FULL_DUPLEX) ?
					("full-duplex") : ("half-duplex\n"));
				else
					printf("Port %d Link Down\n",
						(uint8_t)portid);
				continue;
			}
			/* clear all_ports_up flag if any link down */
			if (link.link_status == 0) {
				all_ports_up = 0;
				break;
			}
		}
		/* after finally printing all link status, get out */
		if (print_flag == 1)
			break;

		if (all_ports_up == 0) {
			printf(".");
			fflush(stdout);
			rte_delay_ms(CHECK_INTERVAL);
		}

		/* set the print_flag if all ports up or timeout */
		if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1)) {
			print_flag = 1;
			printf("done\n");
		}
	}
}

static void nic_init(int nb_ports)
{
    int portid, ret;
    /* set up mempool for each port */
    for (portid = 0; portid < nb_ports; portid++) {
        char name[RTE_MEMPOOL_NAMESIZE];
        uint32_t nb_mbuf = NB_MBUF, rx_q = 0, tx_q = 0;
        sprintf(name, "mbuf_pool-%d", portid);
        /* create the mbuf pool */
        pktmbuf_pool[portid] =
            rte_mempool_create(name, nb_mbuf,
            MBUF_SIZE, MEMPOOL_CACHE_SIZE,
            sizeof(struct rte_pktmbuf_pool_private),
            rte_pktmbuf_pool_init, NULL,
            rte_pktmbuf_init, NULL,
            rte_socket_id(), MEMPOOL_F_SP_PUT |
            MEMPOOL_F_SC_GET);

        if (pktmbuf_pool[portid] == NULL)
            rte_exit(EXIT_FAILURE, "Cannot init mbuf pool, errno: %d\n",
                    rte_errno);

        /* init port */
        printf("Initializing port %u... ", (unsigned) portid);
        fflush(stdout);
        /* hard-coded one tx queue and one rx queue port */
        ret = rte_eth_dev_configure(portid, 1, 1, &port_conf);
        if (ret < 0)
            rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
                    ret, (unsigned) portid);

        fflush(stdout);

        /* check port capabilities */
        rte_eth_dev_info_get(portid, &dev_info[portid]);

        ret = rte_eth_rx_queue_setup(portid, rx_q, nb_rxd,
                            rte_eth_dev_socket_id(portid), &rx_conf,
                            pktmbuf_pool[portid]);
        if (ret < 0)
            rte_exit(EXIT_FAILURE,
                    "rte_eth_rx_queue_setup:err=%d, port=%u, queueid: %d\n",
                    ret, (unsigned) portid, rx_q);

        fflush(stdout);
        ret = rte_eth_tx_queue_setup(portid, tx_q, nb_txd,
                            rte_eth_dev_socket_id(portid), &tx_conf);
        if (ret < 0)
            rte_exit(EXIT_FAILURE,
                    "rte_eth_tx_queue_setup:err=%d, port=%u, queueid: %d\n",
                    ret, (unsigned) portid, tx_q);

        /* Start device */
        ret = rte_eth_dev_start(portid);
        if (ret < 0)
            rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
                    ret, (unsigned) portid);

        printf("done: \n");
        rte_eth_promiscuous_enable(portid);

    }
    check_all_ports_link_status(nb_ports, 0xFFFFFFFF);
}

void signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM) {
		printf("\n\nSignal %d received, preparing to exit...\n",
				signum);
		force_quit[0] = force_quit[1] = force_quit[2] = force_quit[3] = force_quit[4] = true;
	}
}

static int core2port(int core) {
	// modify this function accordingly
	return core-1;
}

static inline void set_tstamp(struct rte_mbuf *m)
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

static int send_main_loop()
{
    int core, nb_tx, tx_q = 0, i, prtid, time_cnt = 0;
    struct rte_mempool *mp = NULL;
    struct rte_mbuf *pkts_burst[BURST_SIZE], *m;
	struct rte_mbuf **pp;
    struct timeval cur_tv, prev_tv;
    uint64_t diff_ts, send_pkts = 0, send_bytes = 0;
	union _data *d;
    pcap_info_t *pcap;
	char pcapf[32];


    core = rte_lcore_id();
    prtid = core2port(core);
	snprintf(pcapf, sizeof(pcapf), "%s_%d.pcap", pfile, prtid-vf);
	pcap = _pcap_open(pcapf, prtid);
    if (pcap == NULL) {
        RTE_LOG(ERR, USER1, "[CPU %d] Cannot parse pcap file %s.\n", core, pcapf);
    }
    mp = pcap_parse(pcap, prtid);

    /* timer */
    gettimeofday(&cur_tv, NULL);
    prev_tv = cur_tv;

    while (!force_quit[prtid]) {
        for (i = 0; i < BURST_SIZE; i++) {
            if ((pkts_burst[i] = rte_pktmbuf_alloc(mp)) == NULL) {
                RTE_LOG(ERR, USER1, "[CPU %d] Cannot alloc mbuf.\n", core);
                exit(EXIT_FAILURE);
            }
			m = pkts_burst[i];	
			/* set timestamp */
			set_tstamp(m);
			d = (union _data *)&m->udata64;
			m->data_len = d->data_len;
			m->pkt_len = d->pkt_len;
            send_bytes += m->pkt_len;
        }
        i = BURST_SIZE;
		pp = pkts_burst;
        while (i)
        {
            nb_tx = rte_eth_tx_burst(prtid, tx_q, pp, i);
            i -= nb_tx;
			pp += nb_tx;
            send_pkts += nb_tx;
        }
        for (i = 0; i < BURST_SIZE; i++){
            rte_pktmbuf_free(pkts_burst[i]);
        }
		gettimeofday(&cur_tv, NULL);
        diff_ts = TIMEVAL_TO_MSEC(cur_tv) - TIMEVAL_TO_MSEC(prev_tv);
        if (diff_ts > interval) {
			RTE_LOG(INFO, USER1, "[CPU %d] %d TX %ld pps, %.2f Gbps\n", core, time_cnt++,
                send_pkts*1000/diff_ts, send_bytes*8.0/diff_ts/1000000);
            prev_tv = cur_tv;
			send_pkts = 0;
            send_bytes = 0;
        }
	}
    return 0;
}

/* main processing loop */
static int recv_main_loop()
{
	uint32_t core, nb_rx, rx_q = 0, i, time_cnt = 0, dport, prtid, fid;
	uint32_t ip_cnt[4] = { 0 };
    struct tcp_hdr *tcp;
	struct rte_mbuf *pkts_burst[BURST_SIZE];
	struct timeval cur_tv, prev_tv;
    uint64_t diff_ts, recv_pkts = 0, recv_bytes = 0;
	uint64_t lat[4] = { 0 }, avg_lat[4] = { 0 }, lat_nb[4] = { 0 }, ticks;
	tstamp_t *tstamp;

    core = rte_lcore_id();
	prtid = core2port(core);
	ticks = rte_get_timer_hz() / 1000000;

	// timer
	gettimeofday(&cur_tv, NULL);
    prev_tv = cur_tv;

	while (!force_quit[prtid]) {
		while ((nb_rx = rte_eth_rx_burst(prtid, rx_q, pkts_burst, BURST_SIZE)) > 0) {
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
			RTE_LOG(INFO, USER1, "[CPU %d] %d latency flow1 %d us, flow2 %d us, flow3 %d us, flow4 %d us\n", 
				core, time_cnt, lat[0], lat[1], lat[2], lat[3]);
			RTE_LOG(INFO, USER1, "[CPU %d] %d RX %ld pps, %.2f Gbps, flow1 %ld pps, flow2 %ld pps, flow3 %ld pps, flow4 %ld pps\n", 
				core, time_cnt++, recv_pkts*1000/diff_ts, recv_bytes*8.0/diff_ts/1000000, ip_cnt[0]*1000/diff_ts, ip_cnt[1]*1000/diff_ts, ip_cnt[2]*1000/diff_ts, ip_cnt[3]*1000/diff_ts);
			prev_tv = cur_tv;
			recv_pkts = 0;
			recv_bytes = 0;
			memset(ip_cnt, 0, sizeof(ip_cnt));
			memset(avg_lat, 0, sizeof(avg_lat));
			memset(lat_nb, 0, sizeof(lat_nb));
		}
	}
    return 0;
}

static int launch_one_lcore(void *arg __rte_unused)
{
    uint8_t core = rte_lcore_id();
    if (core <= vf)
    {
        recv_main_loop();
    }
    else {
        send_main_loop();
    }
    return 0;
}

int main(int argc, char **argv)
{
	int i, ret;
	uint8_t nb_ports, portid;
	char corebuf[8];

    for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0) {
		    vf = atoi(argv[i+1]);
		}
		if (strcmp(argv[i], "-p") == 0) {
			pf = atoi(argv[i+1]);
		}
		if (strcmp(argv[i], "-t") == 0) {
			exec_time = atoi(argv[i+1]);
		}
		if (strcmp(argv[i], "-f") == 0) {
			strncpy(pfile, argv[i+1], sizeof(pfile));
		}
        RTE_LOG(INFO, USER1, "%s\n", argv[i+1]);
    }

	snprintf(corebuf, 8, "0-%d", vf+pf);

	/* init EAL */
    char *v[] = {"",
        "-l",
        corebuf,
        "-m",
        "4096",
        ""
    };
    const int c = 5;
	ret = rte_eal_init(c, v);

	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");

	force_quit[0] = force_quit[1] = force_quit[2] = force_quit[3] = force_quit[4] = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	nb_ports = rte_eth_dev_count();
	if (nb_ports == 0)
		rte_exit(EXIT_FAILURE, "No Ethernet ports - bye\n");

    // run init processure
    nic_init(nb_ports);

	/* launch per-lcore init on every lcore */
    rte_eal_mp_remote_launch(launch_one_lcore, NULL, SKIP_MASTER);

    sleep(exec_time);

	for (portid = 0; portid < nb_ports; portid++) {
		force_quit[portid] = true;
		printf("Closing port %d...", portid);
		rte_eth_dev_stop(portid);
		rte_eth_dev_close(portid);
		printf(" Done\n");
	}

	return ret;
}
