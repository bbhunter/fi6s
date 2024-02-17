#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h> // getpid()
#include <time.h> // time()
#include <getopt.h>
#include <netinet/in.h> // struct sockaddr_in6, AF_INET6
#include <sys/socket.h>

#include "util.h"
#include "target.h"
#include "rawsock.h"
#include "scan.h"
#include "output.h"
#include "banner.h"

static int read_targets_from_file(const char *filename, int stream_targets);
static void usage(void);
static inline bool is_all_ff(const uint8_t *buf, int len);

enum operating_mode {
	M_SCAN, M_PRINT_HOSTS,
	M_READSCAN, M_PRINT_SUMMARY,
	M_PRINT_NETWORK,
};

int main(int argc, char *argv[])
{
	static const struct option long_options[] = {
		{"readscan", required_argument, 0, 1000},
		{"print-hosts", no_argument, 0, 1001},
		{"print-summary", no_argument, 0, 1002},
		{"list-protocols", no_argument, 0, 1003},
		{"print-network-settings", no_argument, 0, 1004},

		{"interface", required_argument, 0, 2002},
		{"source-mac", required_argument, 0, 2003},
		{"router-mac", required_argument, 0, 2004},
		{"source-ip", required_argument, 0, 2005},
		{"ttl", required_argument, 0, 2007},

		{"randomize-hosts", required_argument, 0, 2000},
		{"max-rate", required_argument, 0, 2001},
		{"source-port", required_argument, 0, 2006},
		{"stream-targets", no_argument, 0, 2008},
		{"icmp", no_argument, 0, 2009},

		{"output-format", required_argument, 0, 3000},
		{"show-closed", no_argument, 0, 3001},

		{"help", no_argument, 0, 'h'},
		{"ports", required_argument, 0, 'p'},
		{"output-file", required_argument, 0, 'o'},
		{"quiet", no_argument, 0, 'q'},
		{"banners", no_argument, 0, 'b'},
		{"udp", no_argument, 0, 'u'},
		{0,0,0,0},
	};

	int randomize_hosts = 1,
		ttl = 64, max_rate = -1,
		source_port = -1, quiet = 0,
		show_closed = 0, banners = 0,
		stream_targets = 0;
	enum operating_mode mode;
	uint8_t ip_type, source_mac[6], router_mac[6], source_addr[16];
	char *interface;
	struct ports ports;
	FILE *outfile, *readscan;
	const struct outputdef *outdef;

	mode = M_SCAN;
	ip_type = IP_TYPE_TCP;
	interface = NULL; // automatically picked
	outfile = stdout;
	outdef = &output_list;

	memset(source_mac, 0xff, 6);
	memset(router_mac, 0xff, 6);
	memset(source_addr, 0xff, 16);
	init_ports(&ports);
	readscan = NULL;

	while(1) {
		int c = getopt_long(argc, argv, "hp:o:qbu", long_options, NULL);
		if(c == -1) // no more options
			break;
		else if(c == '?') // signals error
			return 1;
		switch(c) {
			case 1000: {
				FILE *f = strcmp(optarg, "-") == 0 ? stdin : fopen(optarg, "rb");
				if(!f) {
					printf("Failed to open scan file for reading\n");
					return 1;
				}
				readscan = f;
				mode = M_READSCAN;
				break;
			}
			case 1001:
				mode = M_PRINT_HOSTS;
				break;
			case 1002:
				mode = M_PRINT_SUMMARY;
				break;
			case 1003:
				banner_print_service_types();
				return 0;
			case 1004:
				mode = M_PRINT_NETWORK;
				break;

			case 2002:
				interface = optarg;
				break;
			case 2003:
				if(parse_mac(optarg, source_mac) < 0) {
					printf("Argument to --source-mac is not a valid MAC address\n");
					return 1;
				}
				break;
			case 2004:
				if(parse_mac(optarg, router_mac) < 0) {
					printf("Argument to --router-mac is not a valid MAC address\n");
					return 1;
				}
				break;
			case 2005:
				if(parse_ipv6(optarg, source_addr) < 0) {
					printf("Argument to --source-ip is not a valid IPv6 address\n");
					return 1;
				}
				break;
			case 2007: {
				int val = strtol_simple(optarg, 10);
				if(val < 1 || val > 255) {
					printf("Argument to --ttl must be a number in range 1-255\n");
					return 1;
				}
				ttl = val;
				break;
			}

			case 2000:
				if(strlen(optarg) > 1 || (*optarg != '0' && *optarg != '1')) {
					printf("Argument to --randomize-hosts must be 0 or 1\n");
					return 1;
				}
				randomize_hosts = (*optarg == '1');
				break;
			case 2001: {
				int val = strtol_suffix(optarg);
				if(val <= 0) {
					printf("Argument to --max-rate must be a positive number\n");
					return 1;
				}
				max_rate = val;
				break;
			}
			case 2006: {
				int val = strtol_simple(optarg, 10);
				if(val < 1 || val > 65535) {
					printf("Argument to --source-port must be a number in range 1-65535\n");
					return 1;
				}
				source_port = val;
				break;
			}
			case 2008:
				stream_targets = 1;
				break;
			case 2009:
				ip_type = IP_TYPE_ICMPV6;
				break;

			case 3000:
				if(strcmp(optarg, "list") == 0) {
					outdef = &output_list;
				} else if(strcmp(optarg, "json") == 0) {
					outdef = &output_json;
				} else if(strcmp(optarg, "binary") == 0) {
					outdef = &output_binary;
				} else {
					printf("Argument to --output-format must be one of list, json or binary\n");
					return 1;
				}
				break;
			case 3001:
				show_closed = 1;
				break;

			case 'h':
				usage();
				return 0;
			case 'p':
				if(parse_ports(optarg, &ports) < 0) {
					printf("Argument to -p must be valid port range(s)\n");
					return 1;
				}
				break;
			case 'o': {
				FILE *f = strcmp(optarg, "-") == 0 ? stdout : fopen(optarg, "wb");
				if(!f) {
					printf("Failed to open output file for writing\n");
					return 1;
				}
				outfile = f;
				break;
			}
			case 'q':
				quiet = 1;
				break;
			case 'b':
				banners = 1;
				break;
			case 'u':
				ip_type = IP_TYPE_UDP;
				break;

			default:
				break;
		}
	}

	int max_args = 1;
	if(mode == M_READSCAN) {
		max_args = 0;
	} else {
		if(mode == M_PRINT_NETWORK && argc - optind == 0) {
			// permitted for convenience
		} else if(argc - optind < 1) {
			printf("No target specification(s) given.\n");
			return 1;
		}
	}
	if(argc - optind > max_args) {
		printf("Too many arguments.\n");
		return 1;
	}

	// attempt to auto-detect network settings
	if(mode == M_SCAN || mode == M_PRINT_NETWORK) {
		if(!interface) {
			if(rawsock_getdev(&interface) < 0)
				return 1;
			if(!interface) { // didn't find one
				fprintf(stderr, "No default interface found, "
					"provide one using the --interface option.\n");
				return 1;
			}
			if(mode != M_PRINT_NETWORK)
				fprintf(stderr, "Using default interface '%s'\n", interface);
		}
		if(is_all_ff(source_mac, 6))
			rawsock_getmac(interface, source_mac);
		if(is_all_ff(router_mac, 6))
			rawsock_getgw(interface, router_mac);
		if(is_all_ff(source_addr, 16)) {
			struct sockaddr_in6 globaddr = {0};
			globaddr.sin6_family = AF_INET6;
			globaddr.sin6_addr.s6_addr[0] = 0x20; // 2000::
			rawsock_getsrcip(&globaddr, interface, source_addr);
		}
	}

	srand(time(NULL) ^ getpid());
	if(target_gen_init() < 0)
		return 1;
	target_gen_set_randomized(randomize_hosts);
	rawsock_eth_settings(source_mac, router_mac);
	rawsock_ip_settings(source_addr, ttl);

	const char *tspec = argv[optind];
	if(mode == M_READSCAN || mode == M_PRINT_NETWORK) {
		// no targets in this mode
	} else {
		if(*tspec == '@') { // load from file
			if(read_targets_from_file(&tspec[1], stream_targets) < 0)
				return 1;
		} else { // single target spec
			struct targetspec t;
			if(target_parse(tspec, &t) < 0) {
				printf("Failed to parse target specification.\n");
				return 1;
			}
			target_gen_add(&t);
		}
		if(target_gen_finish_add() < 0) {
			printf("No target specification(s) given.\n");
			return 1;
		}
	}

	int r;
	if(mode == M_READSCAN) {
		scan_reader_set_general(show_closed, banners);
		scan_reader_set_output(outfile, outdef);
		r = scan_reader_main(readscan) < 0 ? 1 : 0;
	} else if(mode == M_PRINT_HOSTS) {
		uint8_t addr[16];
		char buf[IPV6_STRING_MAX];
		while(target_gen_next(addr) == 0) {
			ipv6_string(buf, addr);
			puts(buf);
		}

		r = 0;
	} else if(mode == M_PRINT_SUMMARY) {
		int nports = 0;
		if(validate_ports(&ports)) {
			struct ports_iter it;
			for(ports_iter_begin(&ports, &it); ports_iter_next(&it); )
				nports++;
		} else {
			nports = 1;
		}
		target_gen_print_summary(max_rate, nports);

		r = 0;
	} else if(mode == M_PRINT_NETWORK) {
		char buf[IPV6_STRING_MAX];
		printf("Interface: %s\n", interface);
		mac_string(buf, source_mac);
		printf("Source MAC: %s\n", is_all_ff(source_mac, 6) ? "(missing)" : buf);
		mac_string(buf, router_mac);
		printf("Router MAC: %s\n", is_all_ff(router_mac, 6) ? "(missing)" : buf);
		printf("Time-To-Live: %d\n", ttl);
		ipv6_string(buf, source_addr);
		printf("Source IP: %s\n", is_all_ff(source_addr, 16) ? "(missing)" : buf);

		r = 0;
	} else {
		r = target_gen_sanity_check() < 0 ? 1 : 0;

		if (r == 0) {
			const char* missing = NULL;
			if(is_all_ff(source_mac, 6))
				missing = "--source-mac";
			else if(is_all_ff(router_mac, 6))
				missing = "--router-mac";
			else if(is_all_ff(source_addr, 16))
				missing = "--source-ip";
			else if(ip_type != IP_TYPE_ICMPV6 && !validate_ports(&ports))
				missing = "-p";
			else if(banners && ip_type == IP_TYPE_TCP && source_port == -1)
				missing = "--source-port";

			if(missing) {
				printf("Option %s is required but was not given.\n", missing);
				r = 1;
			}
		}

		if (r == 0) {
			scan_set_general(&ports, max_rate, show_closed, banners);
			scan_set_network(source_addr, source_port, ip_type);
			scan_set_output(outfile, outdef);
			r = scan_main(interface, quiet) < 0 ? 1 : 0;
		}
	}

	target_gen_fini();
	fclose(outfile);
	if(mode == M_READSCAN)
		fclose(readscan);
	return r;
}

static int read_targets_from_file(const char *filename, int stream_targets)
{
	FILE *f;
	char buf[256];
	f = fopen(filename, "r");
	if(!f) {
		printf("Failed to open target list for reading.\n");
		return -1;
	}

	if(stream_targets) {
		target_gen_set_streaming(f);
		return 0;
	}

	while(fgets(buf, sizeof(buf), f) != NULL) {
		struct targetspec t;

		trim_string(buf, " \t\r\n");
		if(buf[0] == '#' || buf[0] == '\0')
			continue; // skip comments and empty lines

		if(target_parse(buf, &t) < 0) {
			printf("Failed to parse target \"%s\".\n", buf);
			fclose(f);
			return -1;
		}
		if(target_gen_add(&t) < 0) {
			fclose(f);
			return -1;
		}
	}
	fclose(f);
	return 0;
}

static void usage(void)
{
	printf("fi6s is a IPv6 network scanner capable of scanning lots of targets in little time.\n");
	printf("Usage: fi6s [options] <target specification>\n");
	printf("\n");
	static const struct { const char *l, *r; } lines[] = {
		{"General options:", NULL},
		{"--help", "Show this text"},
		{"--list-protocols", "List TCP/UDP protocols supported by fi6s for banner grabbing"},
		{"--readscan <file>", "Read specified binary scan from <file> instead of performing a scan"},
		{"--print-network-settings", "Print (auto-detected) network settings and exit"},
		{"--print-hosts", "Print all hosts to be scanned and exit (don't scan)"},
		{"--print-summary", "Print summary of hosts to be scanned and exit"},
		{"Network settings:", NULL},
		{"--interface <iface>", "Use <iface> for capturing and sending packets"},
		{"--source-mac <mac>", "Set Ethernet layer source to <mac>"},
		{"--router-mac <mac>", "Set Ethernet layer destination to <mac>"},
		{"--ttl <n>", "Set Time-To-Live of sent packets to <n> (default: 64)"},
		{"--source-ip <ip>", "Use specified source IP address"},
		{"Scan options:", NULL},
		{"--stream-targets", "Read target IPs from file on demand instead of ahead-of-time"},
		{"--randomize-hosts <0|1>", "Randomize scan order of hosts (default: 1)"},
		{"--max-rate <n>", "Send no more than <n> packets per second (default: unlimited)"},
		{"--source-port <port>", "Use specified source port"},
		{"-p/--ports <ranges>", "Specify port range(s) to scan"},
		{"-b/--banners", "Capture banners on open TCP ports / UDP responses"},
		{"-u/--udp", "UDP scan"},
		{"--icmp", "ICMPv6 Echo scan"},
		{"-q/--quiet", "Do not output status message during scan"},
		{"Output options:", NULL},
		{"-o <file>", "Write results to <file>"},
		{"--output-format <fmt>", "Set output format to one of list,json,binary (default: list)"},
		{"--show-closed", "Show closed ports (TCP)"},
		{NULL},
	};
	for(int i = 0; lines[i].l != NULL; i++) {
		if(lines[i].r)
			printf("  %-25s %s\n", lines[i].l, lines[i].r);
		else
			printf("%s\n", lines[i].l);
	}
	printf("\n");
	printf("Target specification:\n");
	printf("  A target specification is essentially just a network address and mask.\n");
	printf("  They come in three shapes:\n");
	printf("    2001:db8::/64 (classic subnet notation)\n");
	printf("      This one should be obvious, you can even omit the number (it defaults to 128).\n");
	printf("    2001:db8::1/32-48 (subnet range notation)\n");
	printf("      The resulting netmask is ffff:ffff:0000:ffff:ffff:ffff:ffff:ffff.\n");
	printf("      It refers to addresses 2001:db8:0::1, 2001:db8:1::1 ... 2001:db8:ffff::1\n");
	printf("    2001:db8::x (wildcard nibble notation)\n");
	printf("      The resulting netmask is ::000f.\n");
	printf("      It refers to addresses 2001:db8::0, 2001:db8::1 ... 2001:db8::f\n");
	printf("  Only one target specification can be specified on the command line,\n");
	printf("  if you want to scan multiple targets pass @/path/to/list_of_targets.txt to fi6s.\n");
	printf("\n");
	printf("The \"binary\" output format:\n");
	printf("  When saving as binary output, banners will not be decoded during scanning and are saved verbatim.\n");
	printf("  Binary scan files can be read again afterwards and converted to any desired output format.\n");
	printf("  When reading binary scans, the --banners and --show-closed options are also applied\n");
	printf("  and can be used to select which data is shown.\n");
	printf("  For example, you could perform a scan that captures banners but only extract open/closed ports:\n");
	printf("    $ fi6s -o scan.bin --output-format binary -b 2001:db8::xx\n");
	printf("    $ fi6s --readscan scan.bin --show-closed\n");
}

static inline bool is_all_ff(const uint8_t *buf, int len)
{
	while(len--) {
		if(*(buf++) != 0xff)
			return false;
	}
	return true;
}
