#include "pcap.h"

__inline__ void
_pcap_convert(pcap_info_t *pcap, pcaprec_hdr_t *pHdr)
{
	if (pcap->endian == BIG_ENDIAN) {
		pHdr->incl_len  = ntohl(pHdr->incl_len);
		pHdr->orig_len  = ntohl(pHdr->orig_len);
		pHdr->ts_sec    = ntohl(pHdr->ts_sec);
		pHdr->ts_usec   = ntohl(pHdr->ts_usec);
	}
}

void _pcap_info(pcap_info_t *pcap, int flag)
{
	printf("\nPCAP file: %s\n", pcap->filename);
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

void _pcap_close(pcap_info_t *pcap)
{
	if (pcap == NULL)
		return;

	if (pcap->fd)
		fclose(pcap->fd);
	if (pcap->filename)
		free(pcap->filename);
	rte_free(pcap);
}

pcap_info_t *_pcap_open(char *filename)
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
	_pcap_info(pcap, 0);

	return pcap;

leave:
	_pcap_close(pcap);
	fflush(stdout);

	return NULL;
}

void _pcap_rewind(pcap_info_t *pcap)
{
	if (pcap == NULL)
		return;

	/* Rewind to the beginning */
	rewind(pcap->fd);

	/* Seek past the pcap header */
	(void)fseek(pcap->fd, sizeof(pcap_hdr_t), SEEK_SET);
}

size_t _pcap_read(pcap_info_t *pcap,
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
void pcap_mbuf_ctor(struct rte_mempool *mp, void *opaque_arg, void *_m, unsigned i)
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

struct rte_mempool *pcap_parse(pcap_info_t *pcap, int port)
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