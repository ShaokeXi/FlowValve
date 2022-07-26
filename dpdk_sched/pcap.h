#ifndef _PCAP_H_
#define _PCAP_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
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

#define BUF_SIZE 2048
#define MIN_PKT_SIZE 64
#define MAX_PKT_SIZE 1518
#define MAX_MBUFS_PER_PORT 16384
#define MBUF_INVALID_PORT UINT8_MAX
#define PCAP_MAGIC_NUMBER 0xa1b2c3d4
#define MBUF_SIZE (BUF_SIZE + RTE_PKTMBUF_HEADROOM)

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

__inline__ void _pcap_convert(pcap_info_t *pcap, pcaprec_hdr_t *pHdr);
void _pcap_info(pcap_info_t *pcap, int flag);
void _pcap_close(pcap_info_t *pcap);
pcap_info_t *_pcap_open(char *filename);
void _pcap_rewind(pcap_info_t *pcap);
size_t _pcap_read(pcap_info_t *pcap, pcaprec_hdr_t *pHdr, char *pktBuff, uint32_t bufLen);
void pcap_mbuf_ctor(struct rte_mempool *mp, void *opaque_arg, void *_m, unsigned i);
struct rte_mempool *pcap_parse(pcap_info_t *pcap, int port);

#endif /* _PCAP_H_ */