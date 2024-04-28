#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h> // rand()
#include <stdbool.h>
#include <string.h>
#include <unistd.h> // usleep()
#include <limits.h>
#include <stdatomic.h>
#include <pthread.h>

#include "scan.h"
#include "output.h"
#include "target.h"
#include "util.h"
#include "rawsock.h"
#include "tcp.h"
#include "udp.h"
#include "icmp.h"
#include "banner.h"

enum {
	SEND_FINISHED 	  = (1 << 0),
	ERROR_SEND_THREAD = (1 << 1),
	ERROR_RECV_THREAD = (1 << 2),
};

static uint8_t source_addr[16];
static int source_port;
//
static struct ports ports;
static unsigned int max_rate;
static int show_closed, banners;
static uint8_t ip_type;
//
static FILE *outfile;
static struct outputdef outdef;

static atomic_uint pkts_sent, pkts_recv;
static atomic_uchar status_bits;

static inline int source_port_rand(void);
static void *send_thread_tcp(void *unused);
static void *send_thread_udp(void *unused);
static void *send_thread_icmp(void *unused);

static void *recv_thread(void *unused);
static void recv_handler(uint64_t ts, int len, const uint8_t *packet);
static void recv_handler_tcp(uint64_t ts, int len, const uint8_t *packet, const uint8_t *csrcaddr);
static void recv_handler_udp(uint64_t ts, int len, const uint8_t *packet, const uint8_t *csrcaddr);
static void recv_handler_icmp(uint64_t ts, int len, const uint8_t *packet, const uint8_t *csrcaddr);

#if ATOMIC_INT_LOCK_FREE != 2
#warning Non lock-free atomic types will severely affect performance.
#endif

/****/

void scan_set_general(const struct ports *_ports, int _max_rate, int _show_closed, int _banners)
{
	memcpy(&ports, _ports, sizeof(struct ports));
	max_rate = _max_rate < 0 ? UINT_MAX : _max_rate - 1;
	show_closed = _show_closed;
	banners = _banners;
}

void scan_set_network(const uint8_t *_source_addr, int _source_port, uint8_t _ip_type)
{
	memcpy(source_addr, _source_addr, 16);
	source_port = _source_port;
	ip_type = _ip_type;
}

void scan_set_output(FILE *_outfile, const struct outputdef *_outdef)
{
	outfile = _outfile;
	memcpy(&outdef, _outdef, sizeof(struct outputdef));
}

int scan_main(const char *interface, int quiet)
{
	if(rawsock_open(interface, 65535) < 0)
		return -1;
	atomic_store(&pkts_sent, 0);
	atomic_store(&pkts_recv, 0);
	atomic_store(&status_bits, 0);
	if(banners && ip_type == IP_TYPE_TCP) {
		if(scan_responder_init(outfile, &outdef, source_port) < 0)
			goto err;
	}
	if(!banners && ip_type == IP_TYPE_UDP)
		fprintf(stderr, "Warning: UDP scans don't make sense without banners enabled.\n");
	if(banners && ip_type == IP_TYPE_ICMPV6)
		fprintf(stderr, "Warning: Enabling banners is a no-op for ICMP scans.\n");

	// Set capture filters
	int fflags = RAWSOCK_FILTER_IPTYPE | RAWSOCK_FILTER_DSTADDR;
	if(source_port != -1 && ip_type != IP_TYPE_ICMPV6)
		fflags |= RAWSOCK_FILTER_DSTPORT;
	if(rawsock_setfilter(fflags, ip_type, source_addr, source_port) < 0)
		goto err;

	// Write output file header
	outdef.begin(outfile);

	// Start threads
	pthread_t tr, ts;
	if(pthread_create(&tr, NULL, recv_thread, NULL) < 0)
		goto err;
	pthread_detach(tr);
	do {
		int r;
		if(ip_type == IP_TYPE_TCP)
			r = pthread_create(&ts, NULL, send_thread_tcp, NULL);
		else if(ip_type == IP_TYPE_UDP)
			r = pthread_create(&ts, NULL, send_thread_udp, NULL);
		else // IP_TYPE_ICMPV6
			r = pthread_create(&ts, NULL, send_thread_icmp, NULL);
		if(r < 0)
			goto err;
	} while(0);
	pthread_detach(ts);

	// Stats & progress watching
	unsigned char cur_status = 0;
	while(1) {
		unsigned int cur_sent, cur_recv;
		cur_sent = atomic_exchange(&pkts_sent, 0);
		cur_recv = atomic_exchange(&pkts_recv, 0);
		if(!quiet) {
			float progress = target_gen_progress();
			if(progress < 0.0f)
				fprintf(stderr, "snt:%5u rcv:%5u p:???%%\r", cur_sent, cur_recv);
			else
				fprintf(stderr, "snt:%5u rcv:%5u p:%3d%%\r", cur_sent, cur_recv, (int) (progress*100));
		}
		cur_status = atomic_load(&status_bits);
		if(cur_status)
			break;

		usleep(STATS_INTERVAL * 1000);
	}
	cur_status &= ~SEND_FINISHED; // leave only error bits

	// Wait for the last packets to arrive
	fputs("", stderr);
	if(!cur_status) {
		fprintf(stderr, "Waiting %d more seconds...\n", FINISH_WAIT_TIME);
		usleep(FINISH_WAIT_TIME * 1000 * 1000);
	} else {
		fprintf(stderr, "Errors were encountered.\n");
		// FIXME: missing a way to abort the scan thread
	}
	rawsock_breakloop();
	if(banners && ip_type == IP_TYPE_TCP)
		scan_responder_finish();
	if(!quiet && !cur_status)
		fprintf(stderr, "rcv:%5u\n", atomic_exchange(&pkts_recv, 0));

	// Write output file footer
	outdef.end(outfile);

	int r = 0;
	ret:
	rawsock_close();
	return r;
	err:
	r = 1;
	goto ret;
}

/****/

static void *send_thread_tcp(void *unused)
{
	uint8_t _Alignas(uint32_t) packet[FRAME_ETH_SIZE + FRAME_IP_SIZE + TCP_HEADER_SIZE];
	uint8_t dstaddr[16];
	struct ports_iter it;

	(void) unused;
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	set_thread_name("send");

	rawsock_eth_prepare(ETH_FRAME(packet), ETH_TYPE_IPV6);
	rawsock_ip_prepare(IP_FRAME(packet), IP_TYPE_TCP);
	if(target_gen_next(dstaddr) < 0)
		goto err;
	rawsock_ip_modify(IP_FRAME(packet), TCP_HEADER_SIZE, dstaddr);
	tcp_prepare(TCP_HEADER(packet));
	tcp_make_syn(TCP_HEADER(packet), FIRST_SEQNUM);
	ports_iter_begin(&ports, &it);

	while(1) {
		// Next port number (or target if ports exhausted)
		if(ports_iter_next(&it) == 0) {
			if(target_gen_next(dstaddr) < 0)
				break; // no more targets
			rawsock_ip_modify(IP_FRAME(packet), TCP_HEADER_SIZE, dstaddr);
			ports_iter_begin(NULL, &it);
			continue;
		}

		tcp_modify(TCP_HEADER(packet), source_port==-1?source_port_rand():source_port, it.val);
		tcp_checksum(IP_FRAME(packet), TCP_HEADER(packet), 0);
		rawsock_send(packet, sizeof(packet));

		// Rate control
		if(atomic_fetch_add(&pkts_sent, 1) >= max_rate) {
			do
				usleep(1000);
			while(atomic_load(&pkts_sent) != 0);
		}
	}

	atomic_fetch_or(&status_bits, SEND_FINISHED);
	return NULL;
err:
	atomic_fetch_or(&status_bits, ERROR_SEND_THREAD);
	return NULL;
}

static void *send_thread_udp(void *unused)
{
	uint8_t _Alignas(uint32_t) packet[FRAME_ETH_SIZE + FRAME_IP_SIZE + UDP_HEADER_SIZE + BANNER_QUERY_MAX_LENGTH];
	uint8_t dstaddr[16];
	struct ports_iter it;

	(void) unused;
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	set_thread_name("send");

	rawsock_eth_prepare(ETH_FRAME(packet), ETH_TYPE_IPV6);
	rawsock_ip_prepare(IP_FRAME(packet), IP_TYPE_UDP);
	if(target_gen_next(dstaddr) < 0)
		goto err;
	if(!banners) {
		rawsock_ip_modify(IP_FRAME(packet), UDP_HEADER_SIZE, dstaddr);
		udp_modify2(UDP_HEADER(packet), 0); // we send empty packets
	}
	ports_iter_begin(&ports, &it);

	while(1) {
		// Next port number (or target if ports exhausted)
		if(ports_iter_next(&it) == 0) {
			if(target_gen_next(dstaddr) < 0)
				break; // no more targets
			if(!banners)
				rawsock_ip_modify(IP_FRAME(packet), UDP_HEADER_SIZE, dstaddr);
			ports_iter_begin(NULL, &it);
			continue;
		}

		uint16_t dstport = it.val;
		udp_modify(UDP_HEADER(packet), source_port==-1?source_port_rand():source_port, dstport);
		unsigned int dlen = 0;
		if(banners) {
			const char *payload = banner_get_query(IP_TYPE_UDP, dstport, &dlen);
			if(payload && dlen > 0)
				memcpy(UDP_DATA(packet), payload, dlen);
			rawsock_ip_modify(IP_FRAME(packet), UDP_HEADER_SIZE + dlen, dstaddr);
			udp_modify2(UDP_HEADER(packet), dlen);
		}

		udp_checksum(IP_FRAME(packet), UDP_HEADER(packet), dlen);
		rawsock_send(packet, FRAME_ETH_SIZE + FRAME_IP_SIZE + UDP_HEADER_SIZE + dlen);

		// Rate control
		if(atomic_fetch_add(&pkts_sent, 1) >= max_rate) {
			do
				usleep(1000);
			while(atomic_load(&pkts_sent) != 0);
		}
	}

	atomic_fetch_or(&status_bits, SEND_FINISHED);
	return NULL;
err:
	atomic_fetch_or(&status_bits, ERROR_SEND_THREAD);
	return NULL;
}

static void *send_thread_icmp(void *unused)
{
	uint8_t _Alignas(uint32_t) packet[FRAME_ETH_SIZE + FRAME_IP_SIZE + ICMP_HEADER_SIZE];
	uint8_t dstaddr[16];

	(void) unused;
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	set_thread_name("send");

	rawsock_eth_prepare(ETH_FRAME(packet), ETH_TYPE_IPV6);
	rawsock_ip_prepare(IP_FRAME(packet), IP_TYPE_ICMPV6);
	if(target_gen_next(dstaddr) < 0)
		goto err;
	rawsock_ip_modify(IP_FRAME(packet), ICMP_HEADER_SIZE, dstaddr);
	ICMP_HEADER(packet)->type = 128; // Echo Request
	ICMP_HEADER(packet)->code = 0;
	ICMP_HEADER(packet)->body32 = ICMP_BODY;

	while(1) {
		icmp_checksum(IP_FRAME(packet), ICMP_HEADER(packet), 0);
		rawsock_send(packet, sizeof(packet));

		// Rate control
		if(atomic_fetch_add(&pkts_sent, 1) >= max_rate) {
			do
				usleep(1000);
			while(atomic_load(&pkts_sent) != 0);
		}

		// Next target
		if(target_gen_next(dstaddr) < 0)
			break;
		rawsock_ip_modify(IP_FRAME(packet), ICMP_HEADER_SIZE, dstaddr);
	}

	atomic_fetch_or(&status_bits, SEND_FINISHED);
	return NULL;
err:
	atomic_fetch_or(&status_bits, ERROR_SEND_THREAD);
	return NULL;
}

/****/

static void *recv_thread(void *unused)
{
	(void) unused;
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	set_thread_name("recv");

	int r = rawsock_loop(recv_handler);
	if(r < 0)
		atomic_fetch_or(&status_bits, ERROR_RECV_THREAD);
	return NULL;
}

static void recv_handler(uint64_t ts, int len, const uint8_t *packet)
{
	int v;
	const uint8_t *csrcaddr;

	atomic_fetch_add(&pkts_recv, 1);
	//printf("<< @%lu -- %d bytes\n", ts, len);

	// decode
	if(rawsock_has_ethernet_headers()) {
		if(len < FRAME_ETH_SIZE)
			goto perr;
		rawsock_eth_decode(ETH_FRAME(packet), &v);
	} else {
		v = ETH_TYPE_IPV6;
		packet -= FRAME_ETH_SIZE; // FIXME: convenient but horrible hack
		len += FRAME_ETH_SIZE;
	}
	if(v != ETH_TYPE_IPV6 || len < FRAME_ETH_SIZE + FRAME_IP_SIZE)
		goto perr;
	rawsock_ip_decode(IP_FRAME(packet), &v, NULL, NULL, &csrcaddr, NULL);
	if(v != ip_type) // is this the ip type we expect?
		goto perr;

	// handle
	if(ip_type == IP_TYPE_TCP)
		recv_handler_tcp(ts, len, packet, csrcaddr);
	else if(ip_type == IP_TYPE_UDP)
		recv_handler_udp(ts, len, packet, csrcaddr);
	else // IP_TYPE_ICMPV6
		recv_handler_icmp(ts, len, packet, csrcaddr);

	return;
	perr: ;
#ifndef NDEBUG
	fprintf(stderr, "Failed to decode a packet of length %d\n", len);
#endif
}

static void recv_handler_tcp(uint64_t ts, int len, const uint8_t *packet, const uint8_t *csrcaddr)
{
	if(len < FRAME_ETH_SIZE + FRAME_IP_SIZE + TCP_HEADER_SIZE)
		goto perr;

	// Output stuff
	if(TCP_HEADER(packet)->f_ack && (TCP_HEADER(packet)->f_syn || TCP_HEADER(packet)->f_rst)) {
		int v, v2;
		tcp_decode(TCP_HEADER(packet), &v, NULL);
		rawsock_ip_decode(IP_FRAME(packet), NULL, NULL, &v2, NULL, NULL);
		int st = TCP_HEADER(packet)->f_syn ? OUTPUT_STATUS_OPEN : OUTPUT_STATUS_CLOSED;
		if(outdef.raw || show_closed || TCP_HEADER(packet)->f_syn)
			outdef.output_status(outfile, ts, csrcaddr, OUTPUT_PROTO_TCP, v, v2, st);
	}
	// Pass packet to responder
	if(banners)
		scan_responder_process(ts, len, packet);

	return;
	perr: ;
#ifndef NDEBUG
	fprintf(stderr, "Failed to decode TCP packet of length %d\n", len);
#endif
}

static void recv_handler_udp(uint64_t ts, int len, const uint8_t *packet, const uint8_t *csrcaddr)
{
	if(len < FRAME_ETH_SIZE + FRAME_IP_SIZE + UDP_HEADER_SIZE)
		goto perr;

	int v;
	udp_decode(UDP_HEADER(packet), &v, NULL);
	if(!banners) {
		// We got an answer, that's already noteworthy enough
		int v2;
		rawsock_ip_decode(IP_FRAME(packet), NULL, NULL, &v2, NULL, NULL);
		outdef.output_status(outfile, ts, csrcaddr, OUTPUT_PROTO_UDP, v, v2, OUTPUT_STATUS_OPEN);
		return;
	}

	uint32_t plen = len - (FRAME_ETH_SIZE + FRAME_IP_SIZE + UDP_HEADER_SIZE);
	if(plen == 0)
		return;
	else if(plen > BANNER_MAX_LENGTH)
		plen = BANNER_MAX_LENGTH;
	char temp[BANNER_MAX_LENGTH];
	memcpy(temp, UDP_DATA(packet), plen);
	if(!outdef.raw)
		banner_postprocess(IP_TYPE_UDP, v, temp, &plen);
	outdef.output_banner(outfile, ts, csrcaddr, OUTPUT_PROTO_UDP, v, temp, plen);

	return;
	perr: ;
#ifndef NDEBUG
	fprintf(stderr, "Failed to decode UDP packet of length %d\n", len);
#endif
}

static void recv_handler_icmp(uint64_t ts, int len, const uint8_t *packet, const uint8_t *csrcaddr)
{
	if(len < FRAME_ETH_SIZE + FRAME_IP_SIZE + ICMP_HEADER_SIZE)
		goto perr;

	if(ICMP_HEADER(packet)->type != 129) // Echo Reply
		return;
	if(ICMP_HEADER(packet)->body32 != ICMP_BODY)
		return;

	int v2;
	rawsock_ip_decode(IP_FRAME(packet), NULL, NULL, &v2, NULL, NULL);
	outdef.output_status(outfile, ts, csrcaddr, OUTPUT_PROTO_ICMP, 0, v2, OUTPUT_STATUS_UP);

	return;
	perr: ;
#ifndef NDEBUG
	fprintf(stderr, "Failed to decode ICMPv6 packet of length %d\n", len);
#endif
}

/****/

static inline int source_port_rand(void)
{
	int v;
	v = rand() & 0xffff; // random 16-bit number
	v |= 16384; // ensure that 1) it's not zero 2) it's >= 16384
	return v;
}
