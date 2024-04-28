#ifndef _SCAN_H
#define _SCAN_H

#include <stdio.h>
#include <stdint.h>

struct outputdef;
struct ports;

#define STATS_INTERVAL   1000 // ms
#define FINISH_WAIT_TIME 5    // s
#define BANNER_TIMEOUT   2500 // ms

#define FIRST_SEQNUM 0xf0000000

void scan_set_general(const struct ports *ports, int max_rate, int show_closed, int banners);
void scan_set_network(const uint8_t *source_addr, int source_port, uint8_t ip_type);
void scan_set_output(FILE *outfile, const struct outputdef *outdef);
int scan_main(const char *interface, int quiet);

void scan_reader_set_general(int show_closed, int banners);
void scan_reader_set_output(FILE *outfile, const struct outputdef *outdef);
int scan_reader_main(FILE *infile);

/*** INTERNAL ***/

#define ETH_FRAME(buf) ( (struct frame_eth*) &(buf)[0] )
#define IP_FRAME(buf) ( (struct frame_ip*) &(buf)[FRAME_ETH_SIZE] )
#define TCP_HEADER(buf) ( (struct tcp_header*) &(buf)[FRAME_ETH_SIZE + FRAME_IP_SIZE] )
#define UDP_HEADER(buf) ( (struct udp_header*) &(buf)[FRAME_ETH_SIZE + FRAME_IP_SIZE] )
#define ICMP_HEADER(buf) ( (struct icmp_header*) &(buf)[FRAME_ETH_SIZE + FRAME_IP_SIZE] )
#define TCP_DATA(buf, data_offset) ( (uint8_t*) &(buf)[FRAME_ETH_SIZE + FRAME_IP_SIZE + data_offset] )
#define UDP_DATA(buf) TCP_DATA(buf, UDP_HEADER_SIZE)

int scan_responder_init(FILE *outfile, const struct outputdef *outdef, uint16_t source_port);
void scan_responder_process(uint64_t ts, int len, const uint8_t *rpacket);
void scan_responder_finish();

#endif // _SCAN_H
