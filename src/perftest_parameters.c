#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <limits.h>
#include <arpa/inet.h>
#if defined(__FreeBSD__)
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#endif
#include "perftest_parameters.h"
#include "mlx5_devx.h"
#include "raw_ethernet_resources.h"
#include "host_memory.h"
#include "mmap_memory.h"
#include "cuda_memory.h"
#include "rocm_memory.h"
#include "neuron_memory.h"
#include "hl_memory.h"
#include<math.h>
#ifdef HAVE_RO
#include <stdbool.h>
#include <pci/pci.h>
#endif
#define MAC_LEN (17)
#define ETHERTYPE_LEN (6)
#define MAC_ARR_LEN (6)
#define HEX_BASE (16)
#define DEFAULT_JSON_FILE_NAME "perftest_out.json"
static const char *connStr[] = {"RC","UC","UD","RawEth","XRC","DC","SRD"};
static const char *testsStr[] = {"Send","RDMA_Write","RDMA_Write_imm","RDMA_Read","Atomic"};
static const char *portStates[] = {"Nop","Down","Init","Armed","","Active Defer"};
static const char *qp_state[] = {"OFF","ON"};
static const char *exchange_state[] = {"Ethernet","rdma_cm"};
static const char *atomicTypesStr[] = {"CMP_AND_SWAP","FETCH_AND_ADD"};
#ifdef HAVE_HNSDV
static const char *congestStr[] = {"DCQCN","LDCP","HC3","DIP"};
#endif

/******************************************************************************
 * parse_mac_from_str.
 *
 * Description : parse string by format of"XX:XX:XX:XX:XX:XX" to uint8_t array in size 6 for MAC adderes
 *
 *  Parameters :
 *		mac - char*.
 *		*addr - pointer to output array
 *
 * Return Value : SUCCESS, FAILURE.
 ******************************************************************************/
#if defined(__FreeBSD__)
#define strdupa(_s)                                             \
({                                                              \
        char *_d;                                               \
        int _len;                                               \
                                                                \
        _len = strlen(_s) + 1;                                  \
        _d = alloca(_len);                                      \
        if (_d)                                                 \
                memcpy(_d, _s, _len);                           \
        _d;                                                     \
})
#endif

static int parse_mac_from_str(char *mac, u_int8_t *addr)
{
	char tmpMac[MAC_LEN+1];
	char *tmpField;
	int fieldNum = 0;

	if (strlen(mac) != MAC_LEN) {
		fprintf(stderr, "invalid MAC length\n");
		return FAILURE;
	}
	if (addr == NULL) {
		fprintf(stderr, "invalid  output addr array\n");
		return FAILURE;
	}

	strcpy(tmpMac, mac);
	tmpField = strtok(tmpMac, ":");
	while (tmpField != NULL && fieldNum < MAC_ARR_LEN) {
		char *chk;
		int tmpVal;
		tmpVal = strtoul(tmpField, &chk, HEX_BASE);
		if (tmpVal > 0xff) {
			fprintf(stderr, "field %d value %X out of range\n", fieldNum, tmpVal);
			return FAILURE;
		}
		if (*chk != 0) {
			fprintf(stderr, "Non-digit character %c (%0x) detected in field %d\n", *chk, *chk, fieldNum);
			return FAILURE;
		}
		addr[fieldNum++] = (u_int8_t) tmpVal;
		tmpField = strtok(NULL, ":");
	}
	if (tmpField != NULL || fieldNum != MAC_ARR_LEN) {
		fprintf(stderr, "MAC address longer than six fields\n");
		return FAILURE;
	}
	return SUCCESS;
}
static int parse_ethertype_from_str(char *ether_str, uint16_t *ethertype_val)
{
	if (strlen(ether_str) != ETHERTYPE_LEN) {
		fprintf(stderr, "invalid ethertype length\n");
		return FAILURE;
	}
	*ethertype_val = strtoul(ether_str, NULL, HEX_BASE);
	if (!*ethertype_val)
		return FAILURE;
	return SUCCESS;
}

/******************************************************************************
  parse_ip_from_str.
 *
 * Description : Convert from presentation format of an Internet number in nuffer
 starting at CP to the binary network format and store result for
 interface type AF in buffer starting at BUF.
 *
 *  Parameters :
 *		*ip - char* ip string.
 *		*addr - pointer to output array
 *
 * Return Value : SUCCESS, FAILURE.
 *
 ******************************************************************************/
int parse_ip_from_str(char *ip, u_int32_t *addr)
{
	return inet_pton(AF_INET, ip, addr);
}

/******************************************************************************/
int parse_ip6_from_str(char *ip6, struct in6_addr *addr)
{
	return inet_pton(AF_INET6, ip6, addr);
}

/******************************************************************************
  check_valid_udp_port.
 ******************************************************************************/
int check_if_valid_udp_port(int udp_port)
{
	return ON;
}
/******************************************************************************
  get cache line size from system
 ******************************************************************************/
static int get_cache_line_size()
{
	int size = 0;
 #if !defined(__FreeBSD__)
	size = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
	if (size == 0) {
		#if defined(__sparc__) && defined(__arch64__)
		char* file_name =
			"/sys/devices/system/cpu/cpu0/l2_cache_line_size";
		#else
		char* file_name =
			"/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size";
		#endif

		FILE *fp;
		char line[10];
		fp = fopen(file_name, "r");
		if (fp == NULL) {
			return DEF_CACHE_LINE_SIZE;
		}
		if(fgets(line,10,fp) != NULL) {
			size = atoi(line);
		}
		fclose(fp);
	}
#endif
	// cppcheck-suppress knownConditionTrueFalse
	if (size <= 0)
		size = DEF_CACHE_LINE_SIZE;

	return size;
}
#ifdef HAVE_RO
/******************************************************************************
  Check PCIe Relaxed Ordering

  Stolen from https://github.com/pciutils/pciutils/blob/master/example.c
 ******************************************************************************/
static bool check_pcie_relaxed_ordering_compliant(void) {
	struct pci_access *pacc;
	struct pci_dev *dev;
	bool cpu_is_RO_compliant = true;

	pacc = pci_alloc();
	pci_init(pacc);
	pci_scan_bus(pacc);
	for (dev = pacc->devices; dev && cpu_is_RO_compliant;
	     dev = dev->next) {
		pci_fill_info(dev,
			      PCI_FILL_IDENT | PCI_FILL_BASES | PCI_FILL_CLASS);
		/* https://lore.kernel.org/patchwork/patch/820922/ */
		if ((dev->vendor_id == 0x8086) &&
		    (((dev->device_id >= 0x6f01 && dev->device_id <= 0x6f0e) ||
		      (dev->device_id >= 0x2f01 && dev->device_id <= 0x2f01))))
			cpu_is_RO_compliant = false;
	}
	pci_cleanup(pacc);
	return cpu_is_RO_compliant;
}
#endif
/******************************************************************************
 *
 ******************************************************************************/
static void usage(const char *argv0, VerbType verb, TestType tst, int connection_type)
{
	printf("Usage:\n");

	if (tst != FS_RATE) {
		printf("  %s            start a server and wait for connection\n", argv0);
		printf("  %s <host>     connect to server at <host>\n", argv0);
	} else
		printf("  %s             run a server to measure FS rate \n", argv0);

	printf("\n");
	printf("Options:\n");

	if (verb != ATOMIC && connection_type != RawEth) {
		printf("  -a, --all ");
		printf(" Run sizes from 2 till 2^23\n");
	}

	if (verb == ATOMIC) {
		printf("  -A, --atomic_type=<type> ");
		printf(" type of atomic operation from {CMP_AND_SWAP,FETCH_AND_ADD} (default FETCH_AND_ADD)\n");
	}

	if (tst == BW) {
		printf("  -b, --bidirectional ");
		printf(" Measure bidirectional bandwidth (default unidirectional)\n");
	}

	if (connection_type != RawEth) {
		if (verb == SEND) {
			printf("  -c, --connection=<RC/XRC/UC/UD/DC/SRD> ");
			printf(" Connection type RC/XRC/UC/UD/DC/SRD (default RC)\n");
		} else 	if (verb == WRITE || verb == WRITE_IMM) {
			printf("  -c, --connection=<RC/XRC/UC/DC> ");
			printf(" Connection type RC/XRC/UC/DC (default RC)\n");
		} else if (verb == READ || verb == ATOMIC) {
			printf("  -c, --connection=<RC/XRC/DC> ");
			printf(" Connection type RC/XRC/DC (default RC)\n");
		}
		#ifdef HAVE_DCS
		printf("      --log_dci_streams=<log_num_dci_stream_channels> (default 0) ");
		printf(" Run DC initiator as DCS instead of DCI with <log_num dci_stream_channels>\n");
		printf("      --log_active_dci_streams=<log_num_active_dci_stream_channels> (default log_num_dci_stream_channels)\n");
		#endif
		#ifdef HAVE_AES_XTS
		printf("	  --aes_xts Runs traffic with AES_XTS feature (encryption)\n");
		printf("	  --encrypt_on_tx Runs traffic with encryption on tx (default decryption on tx)\n");
		printf("	  --sig_before Puts signature on data before encrypting it (default after)\n");
		printf("      --aes_block_size=<512,520,4048,4096,4160> (default 512)\n");
		printf("	  --data_enc_keys_number=<number of data encryption keys> (default 1)\n");
		printf("	  --kek_path path to the key encryption key file\n");
		printf("	  --credentials_path path to the credentials file\n");
		printf("	  --data_enc_key_app_path path to the data encryption key app\n");
		#endif
	}

	if (tst == LAT) {
		printf("  -C, --report-cycles ");
		printf(" report times in cpu cycle units (default microseconds)\n");
	}

	printf("  -d, --ib-dev=<dev> ");
	printf(" Use IB device <dev> (default first device found)\n");

	printf("  -D, --duration ");
	printf(" Run test for a customized period of seconds.\n");

	if (verb != WRITE && verb != WRITE_IMM && connection_type != RawEth) {
		printf("  -e, --events ");
		printf(" Sleep on CQ events (default poll)\n");

		printf("  -X, --vector=<completion vector> ");
		printf(" Set <completion vector> used for events\n");
	}

	printf("  -f, --margin ");
	printf(" measure results within margins. (default=2sec)\n");

	printf("  -F, --CPU-freq ");
	printf(" Do not show a warning even if cpufreq_ondemand module is loaded, and cpu-freq is not on max.\n");

	if (verb == SEND && tst != FS_RATE) {
		printf("  -g, --mcg ");
		printf(" Send messages to multicast group with 1 QP attached to it.\n");
		printf("         When there is no multicast gid specified, a default IPv6 typed gid will be used.\n");
	}

	printf("  -h, --help ");
	printf(" Show this help screen.\n");

	if (tst == LAT || tst == LAT_BY_BW || tst == FS_RATE) {
		printf("  -H, --report-histogram ");
		printf(" Print out all results (default print summary only)\n");
	}

	printf("  -i, --ib-port=<port> ");
	printf(" Use port <port> of IB device (default %d)\n",DEF_IB_PORT);

	if (verb != READ && verb != ATOMIC) {
		printf("  -I, --inline_size=<size> ");
		printf(" Max size of message to be sent in inline\n");
	}

	if (tst == BW || tst == LAT_BY_BW) {
		printf("  -l, --post_list=<list size>\n");
		printf(" Post list of send WQEs of <list size> size (instead of single post)\n");
		printf("      --recv_post_list=<list size>");
		printf(" Post list of receive WQEs of <list size> size (instead of single post)\n");
	}

	if (tst != FS_RATE) {
		printf("  -L, --hop_limit=<hop_limit> ");
		printf(" Set hop limit value (ttl for IPv4 RawEth QP). Values 0-255 (default %d)\n", DEF_HOP_LIMIT);

		if (connection_type == RawEth) {
			printf("  -m, --mtu=<mtu> ");
			printf(" MTU size : 64 - 9600 (default port mtu)\n");
		} else {
			printf("  -m, --mtu=<mtu> ");
			printf(" MTU size : 256 - 4096 (default port mtu)\n");
		}

		if (verb == SEND) {
			printf("  -M, --MGID=<multicast_gid> ");
			printf(" In multicast, uses <multicast_gid> as the group MGID.\n");
		}
	}

	printf("  -n, --iters=<iters> ");
	printf(" Number of exchanges (at least %d, default %d)\n", MIN_ITER, ((verb == WRITE || verb == WRITE_IMM) && (tst == BW)) ? DEF_ITERS_WB : DEF_ITERS);

	if (tst == BW) {
		printf("  -N, --noPeak");
		printf(" Cancel peak-bw calculation (default with peak up to iters=20000)\n");
	}

	if (verb == READ || verb == ATOMIC) {
		printf("  -o, --outs=<num> ");
		printf(" num of outstanding read/atom(default max of device)\n");
	}

	if (tst == BW && connection_type != RawEth) {
		printf("  -O, --dualport ");
		printf(" Run test in dual-port mode.\n");
	}

	printf("  -p, --port=<port> ");
	printf(" Listen on/connect to port <port> (default %d)\n",DEF_PORT);

	if (tst == BW ) {
		printf("  -q, --qp=<num of qp's>  Num of qp's(default %d)\n", DEF_NUM_QPS);
		printf("  -Q, --cq-mod ");
		printf(" Generate Cqe only after <--cq-mod> completion\n");
	}

	if ((verb == SEND || verb == WRITE_IMM) && tst != FS_RATE) {
		printf("  -r, --rx-depth=<dep> ");
		printf(" Rx queue size (default %d).",DEF_RX_SEND);
		printf(" If using srq, rx-depth controls max-wr size of the srq\n");
	}

	if (connection_type != RawEth) {
		printf("  -R, --rdma_cm ");
		printf(" Connect QPs with rdma_cm and run test on those QPs\n");
	}

	if (verb != ATOMIC) {
		printf("  -s, --size=<size> ");
		printf(" Size of message to exchange (default %d)\n", tst == LAT ? DEF_SIZE_LAT : DEF_SIZE_BW);
	}

	if (tst != FS_RATE) {
		printf("  -S, --sl=<sl> ");
		printf(" SL (default %d)\n",DEF_SL);

		if (tst == BW || tst == LAT_BY_BW) {
			printf("  -t, --tx-depth=<dep> ");
			printf(" Size of tx queue (default %d)\n", tst == LAT ? DEF_TX_LAT : DEF_TX_BW);
		}

		printf("  -T, --tos=<tos value> ");
		printf(" Set <tos_value> to RDMA-CM QPs. available only with -R flag. values 0-256 (default off)\n");
	}

	printf("  -u, --qp-timeout=<timeout> ");
	printf(" QP timeout, timeout value is 4 usec * 2 ^(timeout), default %d\n",DEF_QP_TIME);

	if (tst == LAT || tst == LAT_BY_BW || tst == FS_RATE) {
		printf("  -U, --report-unsorted ");
		printf(" (implies -H) print out unsorted results (default sorted)\n");
	}

	printf("  -V, --version ");
	printf(" Display version number\n");

	if (tst == BW) {
		printf("  -w, --limit_bw=<value> ");
		printf(" Set verifier limit for bandwidth\n");
	}

	printf("  -W, --report-counters=<list of counter names> ");
	printf(" Report performance counter change (example: \"counters/port_xmit_data,hw_counters/out_of_buffer\")\n");

	if (connection_type != RawEth) {
		printf("  -x, --gid-index=<index> ");
		printf(" Test uses GID with GID index\n");
	}

	if (tst == BW) {
		printf("  -y, --limit_msgrate=<value> ");
		printf(" Set verifier limit for Msg Rate\n");
	}

	if (connection_type != RawEth) {
		printf("  -z, --comm_rdma_cm ");
		printf(" Communicate with rdma_cm module to exchange data - use regular QPs\n");
	}

	/*Long flags*/
	putchar('\n');

	printf("      --out_json ");
	printf(" Save the report in a json file\n");

	printf("      --out_json_file=<file> ");
	printf(" Name of the report json file. (Default: %s in the working directory) \n",DEFAULT_JSON_FILE_NAME);

	printf("      --cpu_util ");
	printf(" Show CPU Utilization in report, valid only in Duration mode \n");

	#ifdef HAVE_HNSDV
	printf("      --congest_type=<DCQCN, LDCP, HC3, DIP> ");
	printf(" Use the hnsdv interface to set congestion control algorithm.\n");
	#endif

	if (tst != FS_RATE) {
		printf("      --dlid ");
		printf(" Set a Destination LID instead of getting it from the other side.\n");
	}

	if (connection_type != RawEth) {
		printf("      --dont_xchg_versions ");
		printf(" Do not exchange versions and MTU with other side \n");
	}

	if (tst != FS_RATE) {
		printf("      --force-link=<value> ");
		printf(" Force the link(s) to a specific type: IB or Ethernet.\n");
	}

	if (verb == SEND) {
		printf("      --use-srq ");
		printf(" Use a Shared Receive Queue. --rx-depth controls max-wr size of the SRQ \n");
	}

	if (connection_type != RawEth) {
		printf("      --ipv6 ");
		printf(" Use IPv6 GID. Default is IPv4\n");
		printf("      --ipv6-addr=<IPv6> ");
		printf(" Use IPv6 address for parameters negotiation. Default is IPv4\n");
	}

	// please note it is a different source_ip from raw_ethernet case
	if (connection_type != RawEth) {
		printf("      --bind_source_ip ");
		printf(" Source IP of the interface used for connection establishment. By default taken from routing table.\n");
	}


	if (tst == LAT) {
		printf("      --latency_gap=<delay_time> ");
		printf(" delay time between each post send\n");
	}

	if (connection_type != RawEth) {
		printf("      --mmap=file ");
		printf(" Use an mmap'd file as the buffer for testing P2P transfers.\n");
		printf("      --mmap-offset=<offset> ");
		printf(" Use an mmap'd file as the buffer for testing P2P transfers.\n");
	}

	if (tst == BW) {
		printf("      --mr_per_qp ");
		printf(" Create memory region for each qp.\n");
	}

	#if defined HAVE_EX_ODP
	printf("      --odp ");
	printf(" Use On Demand Paging instead of Memory Registration.\n");
	#endif

	printf("      --output=<units>");
	printf(" Set verbosity output level: bandwidth , message_rate, latency \n");

	if (connection_type != RawEth && !((verb == WRITE || verb == WRITE_IMM) && tst == LAT)) {
		printf("      --payload_file_path=<payload_txt_file_path>");
		printf(" Set the payload by passing a txt file containing a pattern in the next form(little endian): '0xaaaaaaaa, 0xbbbbbbbb, ...' .\n");
	}

	printf(" Latency measurement is Average calculation \n");

	printf("      --use_old_post_send");
	printf(" Use old post send flow (ibv_post_send).\n");

	if (tst != FS_RATE) {
		printf("      --perform_warm_up");
		printf(" Perform some iterations before start measuring in order to warming-up memory cache, valid in Atomic, Read and Write BW tests\n");

		printf("      --pkey_index=<pkey index> PKey index to use for QP\n");
	}

	if ( tst == BW ) {
		printf("      --report-both ");
		printf(" Report RX & TX results separately on Bidirectional BW tests\n");

		printf("      --report_gbits ");
		printf(" Report Max/Average BW of test in Gbit/sec (instead of MiB/sec)\n");
		printf("        Note: MiB=2^20 byte, while Gb=10^9 bits. Use these formulas for conversion:\n");
		printf("        Factor=10^9/(2^20*8)=119.2; MiB=Gb_result * factor; Gb=MiB_result / factor\n");

		if (connection_type != RawEth) {
			printf("      --report-per-port ");
			printf(" Report BW data on both ports when running Dualport and Duration mode\n");
		}

		printf("      --reversed ");
		printf(" Reverse traffic direction - Server send to client\n");

		printf("      --run_infinitely ");
		printf(" Run test forever, print results every <duration> seconds\n");
	}

	if (connection_type != RawEth) {
		printf("      --retry_count=<value> ");
		printf(" Set retry count value in rdma_cm mode\n");
	}

	if (tst != FS_RATE) {
		printf("      --tclass=<value> ");
		printf(" Set the Traffic Class in GRH (if GRH is in use)\n");

		if (connection_type != RawEth) {
			printf("      --flow_label=<value> ");
			printf(" Set the flow_label in GRH (if GRH is in use)\n");
		}

		if (cuda_memory_supported()) {
			printf("      --use_cuda=<cuda device id>");
			printf(" Use CUDA specific device for GPUDirect RDMA testing\n");

			printf("      --use_cuda_bus_id=<cuda full BUS id>");
			printf(" Use CUDA specific device, based on its full PCIe address, for GPUDirect RDMA testing\n");

			if (cuda_memory_dmabuf_supported()) {
				printf("      --use_cuda_dmabuf");
				printf(" Use CUDA DMA-BUF for GPUDirect RDMA testing\n");
			}
		}

		if (rocm_memory_supported()) {
			printf("      --use_rocm=<rocm device id>");
			printf(" Use selected ROCm device for GPUDirect RDMA testing\n");
		}

		if (neuron_memory_supported()) {
			printf("      --use_neuron=<logical neuron core id>");
			printf(" Use selected logical neuron core for NeuronDirect RDMA testing\n");

			if (neuron_memory_dmabuf_supported()) {
				printf("      --use_neuron_dmabuf");
				printf(" Use DMA-BUF for HW accelerator direct RDMA testing\n");
			}
		}

		if (hl_memory_supported()) {
			printf("      --use_hl=<hl device id>");
			printf(" Use selected Habana Labs device for RDMA testing\n");
		}

		printf("      --use_hugepages ");
		printf(" Use Hugepages instead of contig, memalign allocations.\n");
	}

	if (verb == WRITE || verb == WRITE_IMM || verb == READ) {
		printf("      --use-null-mr ");
		printf(" Allocate a null memory region for the client with ibv_alloc_null_mr.\n");
	}

	if (tst == BW || tst == LAT_BY_BW) {
		printf("      --wait_destroy=<seconds> ");
		printf(" Wait <seconds> before destroying allocated resources (QP/CQ/PD/MR..)\n");

		#if defined HAVE_RO
		printf("      --disable_pcie_relaxed");
		printf(" Disable PCIe relaxed ordering\n");
		#endif
		printf("\n Rate Limiter:\n");
		printf("      --burst_size=<size>");
		printf(" Set the amount of messages to send in a burst when using rate limiter\n");

		printf("      --typical_pkt_size=<bytes>");
		printf(" Set the size of packet to send in a burst. Only supports PP rate limiter\n");

		printf("      --rate_limit=<rate>");
		printf(" Set the maximum rate of sent packages. default unit is [Gbps]. use --rate_units to change that.\n");

		printf("      --rate_units=<units>");
		printf(" [Mgp] Set the units for rate limit to MiBps (M), Gbps (g) or pps (p). default is Gbps (g).\n");
		printf("        Note (1): pps not supported with HW limit.\n");
		printf("        Note (2): When using PP rate_units is forced to Kbps.\n");

		printf("      --rate_limit_type=<type>");
		printf(" [HW/SW/PP] Limit the QP's by HW, PP or by SW. Disabled by default. When rate_limit is not specified HW limit is Default.\n");
		printf("        Note: in Latency under load test SW rate limit is forced\n");

	}
	#if defined HAVE_OOO_ATTR
	printf("      --use_ooo ");
	printf(" Use out of order data placement\n");
	#endif

	if ((tst == LAT || tst == BW) && verb == WRITE) {
		printf("      --write_with_imm ");
		printf(" use write-with-immediate verb instead of write\n");
	}

	putchar('\n');
}
/******************************************************************************
  usage
 ******************************************************************************/
void usage_raw_ethernet(TestType tst)
{
	printf("  Raw Ethernet options :\n");
	printf("  -B, --source_mac ");
	printf(" source MAC address by this format XX:XX:XX:XX:XX:XX **MUST** be entered \n");

	printf("  -E, --dest_mac ");
	printf(" destination MAC address by this format XX:XX:XX:XX:XX:XX **MUST** be entered \n");

	printf("  -G, --use_rss ");
	printf(" use RSS on server side. need to open 2^x qps (using -q flag. default is -q 2). open 2^x clients that transmit to this server\n");

	printf("  -J, --dest_ip ");
	#ifdef HAVE_IPV6
	printf(" destination ip address by this format X.X.X.X for IPv4 or X:X:X:X:X:X for IPv6 (using to send packets with IP header)\n");
	#else
	printf(" destination ip address by this format X.X.X.X (using to send packets with IP header)\n");
	#endif

	printf("  -j, --source_ip ");
	#ifdef HAVE_IPV6
	printf(" source ip address by this format X.X.X.X for IPv4 or X:X:X:X:X:X for IPv6 (using to send packets with IP header)\n");
	#else
	printf(" source ip address by this format X.X.X.X (using to send packets with IP header)\n");
	#endif

	printf("  -K, --dest_port ");
	printf(" destination port number (using to send packets with UDP header as default, or you can use --tcp flag to send TCP Header)\n");

	printf("  -k, --source_port ");
	printf(" source port number (using to send packets with UDP header as default, or you can use --tcp flag to send TCP Header)\n");

	printf("  -Y, --ethertype ");
	printf(" ethertype value in the ethernet frame by this format 0xXXXX\n");

	printf("  -Z, --server ");
	printf(" choose server side for the current machine (--server/--client must be selected )\n");

	printf("      --vlan_en ");
	printf(" insert vlan tag in ethernet header.\n");

	printf("      --vlan_pcp ");
	printf(" specify vlan_pcp value for vlan tag, 0~7. 8 means different vlan_pcp for each packet\n");

	if (tst != FS_RATE) {
		printf("  -P, --client ");
		printf(" choose client side for the current machine (--server/--client must be selected)\n");

		printf("  -v, --mac_fwd ");
		printf(" run mac forwarding test \n");

		printf("      --flows");
		printf(" set number of TCP/UDP flows, starting from <src_port, dst_port>. \n");

		printf("      --flows_burst");
		printf(" set number of burst size per TCP/UDP flow. \n");

		printf("      --promiscuous");
		printf(" run promiscuous mode.\n");

		printf("      --reply_every ");
		printf(" in latency test, receiver pong after number of received pings\n");

		#if defined HAVE_SNIFFER
		printf("      --sniffer");
		printf(" run sniffer mode.\n");
		#endif

		printf("      --flow_label ");
		printf(" IPv6 flow label\n");

	}

	printf("      --tcp ");
	printf(" send TCP Packets. must include IP and Ports information.\n");

	#ifdef HAVE_IPV6
	printf("      --raw_ipv6 ");
	printf(" send IPv6 Packets.\n");
	#endif

	if (tst == BW) {
		printf("      --raw_mcast ");
		printf(" send raw ethernet multicast traffic. No need to specify dest MAC address\n");
	}

	printf("\n");

}
/******************************************************************************
 *
 ******************************************************************************/
static void init_perftest_params(struct perftest_parameters *user_param)
{
	user_param->port		= DEF_PORT;
	user_param->ib_port		= DEF_IB_PORT;
	user_param->ib_port2		= DEF_IB_PORT2;
	user_param->link_type		= LINK_UNSPEC;
	user_param->link_type2		= LINK_UNSPEC;
	user_param->size		= (user_param->tst == BW ) ? DEF_SIZE_BW : DEF_SIZE_LAT;
	user_param->tx_depth		= (user_param->tst == BW || user_param->tst == LAT_BY_BW ) ? DEF_TX_BW : DEF_TX_LAT;
	user_param->qp_timeout		= DEF_QP_TIME;
	user_param->test_method		= RUN_REGULAR;
	user_param->cpu_freq_f		= OFF;
	user_param->connection_type	= (user_param->connection_type == RawEth) ? RawEth : RC;
	user_param->use_null_mr		= 0;
	user_param->use_event		= OFF;
	user_param->eq_num		= 0;
	user_param->use_eq_num		= OFF;
	user_param->num_of_qps		= DEF_NUM_QPS;
	user_param->gid_index		= DEF_GID_INDEX;
	user_param->gid_index2		= DEF_GID_INDEX;
	user_param->use_gid_user	= 0;
	user_param->inline_size		= DEF_INLINE;
	user_param->use_mcg		= OFF;
	user_param->use_rdma_cm		= OFF;
	user_param->work_rdma_cm	= OFF;
	user_param->rx_depth		= (user_param->verb == SEND || user_param->verb == WRITE || user_param->verb == WRITE_IMM)
						? DEF_RX_SEND : DEF_RX_RDMA;
	user_param->duplex		= OFF;
	user_param->noPeak		= OFF;
	user_param->req_cq_mod		= 0;
	user_param->req_size 		= 0;
	user_param->cq_mod		= DEF_CQ_MOD;
	user_param->iters		= (user_param->tst == BW && (user_param->verb == WRITE || user_param->verb == WRITE_IMM))
						? DEF_ITERS_WB : DEF_ITERS;
	user_param->dualport		= OFF;
	user_param->post_list		= 1;
	user_param->recv_post_list	= 1;
	user_param->use_srq		= OFF;
	user_param->use_xrc		= OFF;
	user_param->use_rss		= OFF;
	user_param->srq_exists		= OFF;
	user_param->duration		= DEF_DURATION;
	user_param->margin		= DEF_INIT_MARGIN;
	user_param->test_type		= ITERATIONS;
	user_param->state		= START_STATE;
	user_param->tos			= DEF_TOS;
	user_param->hop_limit		= DEF_HOP_LIMIT;
	user_param->mac_fwd		= OFF;
	user_param->report_fmt		= MBS;
	user_param->report_both		= OFF;
	user_param->is_reversed		= OFF;
	user_param->is_limit_bw		= OFF;
	user_param->limit_bw		= 0;
	user_param->is_limit_msgrate	= OFF;
	user_param->limit_msgrate	= 0;
	user_param->pkey_index		= 0;
	user_param->raw_qos		= 0;
	user_param->tcp			= 0;
	user_param->burst_size		= 0;
	user_param->typical_pkt_size	= 0;
	user_param->rate_limit		= 0;
	user_param->valid_hw_rate_limit_index = 0;
	user_param->rate_units		= GIGA_BIT_PS;
	user_param->rate_limit_type	= DISABLE_RATE_LIMIT;
	user_param->is_rate_limit_type  = 0;
	user_param->data_enc_keys_number = 1;
	user_param->log_dci_streams = 0;
	user_param->log_active_dci_streams = 0;
	user_param->output		= -1;
	user_param->memory_type		= MEMORY_HOST;
	user_param->memory_create	= host_memory_create;
	user_param->cuda_device_id	= 0;
	user_param->cuda_device_bus_id	= NULL;
	user_param->use_cuda_dmabuf	= 0;
	user_param->rocm_device_id	= 0;
	user_param->neuron_core_id	= 0;
	user_param->mmap_file		= NULL;
	user_param->mmap_offset		= 0;
	user_param->iters_per_port[0]	= 0;
	user_param->iters_per_port[1]	= 0;
	user_param->wait_destroy	= 0;
	user_param->is_old_raw_eth_param = 0;
	user_param->is_new_raw_eth_param = 0;
	user_param->reply_every		= 1;
	user_param->vlan_en             = OFF;
	user_param->vlan_pcp		= 1;
	user_param->print_eth_func 	= &print_ethernet_header;

	if (user_param->tst == LAT) {
		user_param->r_flag->unsorted	= OFF;
		user_param->r_flag->histogram	= OFF;
		user_param->r_flag->cycles	= OFF;
	}

	if (user_param->tst == FS_RATE) {
		user_param->r_flag->cycles	= OFF;
	}

	if (user_param->verb == ATOMIC) {
		user_param->atomicType	= FETCH_AND_ADD;
		user_param->size	= DEF_SIZE_ATOMIC;
	}

	user_param->cpu_util			= 0;
	user_param->out_json			= 0;
	user_param->out_json_file_name = strdup(DEFAULT_JSON_FILE_NAME);
	user_param->cpu_util_data.enable	= 0;
	user_param->retry_count			= DEF_RETRY_COUNT;
	user_param->dont_xchg_versions		= 0;
	user_param->ipv6			= 0;
	user_param->ai_family			= AF_INET;
	user_param->report_per_port		= 0;
	user_param->use_odp			= 0;
	user_param->use_hugepages		= 0;
	user_param->use_old_post_send		= 0;
	user_param->use_promiscuous		= 0;
	user_param->use_sniffer			= 0;
	user_param->check_alive_exited		= 0;
	user_param->raw_mcast			= 0;
	user_param->cache_line_size		= get_cache_line_size();
	user_param->cycle_buffer		= sysconf(_SC_PAGESIZE);

	if (user_param->cycle_buffer <= 0) {
		user_param->cycle_buffer = DEF_PAGE_SIZE;
	}
	user_param->mr_per_qp			= 0;
	user_param->dlid			= 0;
	user_param->traffic_class		= 0;
	user_param->flow_label			= 0;
	user_param->flows			= DEF_FLOWS;
	user_param->flows_burst			= 1;
	user_param->perform_warm_up		= 0;
	user_param->use_ooo			= 0;
	user_param->disable_pcir		= 0;
	user_param->source_ip		= NULL;
	user_param->has_source_ip	= 0;
	user_param->use_write_with_imm	= 0;
	user_param->congest_type	= OFF;
}

static int open_file_write(const char* file_path)
{
	int fd;

	fd = open(file_path, O_CREAT|O_RDWR|O_TRUNC|O_CLOEXEC,S_IRUSR|S_IWUSR);

	if (fd < 0)
		fprintf(stderr, "failed to open %s\n", file_path);

	return fd;
}

/******************************************************************************
 *
 ******************************************************************************/
static int ctx_chk_pkey_index(struct ibv_context *context,int pkey_idx)
{
	int idx = 0;
	struct ibv_device_attr attr;

	if (!ibv_query_device(context,&attr)) {
		//coverity[uninit_use]
		if (pkey_idx > attr.max_pkeys - 1) {
			printf(RESULT_LINE);
			fprintf(stderr," Specified PKey Index, %i, greater than allowed max, %i\n",pkey_idx,attr.max_pkeys - 1);
			fprintf(stderr," Changing to 0\n");
			idx = 0;
		} else
			idx = pkey_idx;
	} else {
		fprintf(stderr," Unable to validata PKey Index, changing to 0\n");
		idx = 0;
	}

	return idx;

}


/******************************************************************************
 *
 ******************************************************************************/
static void change_conn_type(int *cptr, VerbType verb, const char *optarg)
{
	if (*cptr == RawEth)
		return;

	if (strcmp(connStr[0], optarg)==0)
		*cptr = RC;

	else if (strcmp(connStr[1], optarg)==0) {
		*cptr = UC;
		if (verb == READ || verb == ATOMIC) {
			fprintf(stderr," UC connection not possible in READ/ATOMIC verbs\n");
			exit(1);
		}

	} else if (strcmp(connStr[2], optarg)==0)  {
		*cptr = UD;
		if (verb != SEND) {
			fprintf(stderr," UD connection only possible in SEND verb\n");
			exit(1);
		}
	} else if(strcmp(connStr[3], optarg)==0) {
		*cptr = RawEth;

	} else if(strcmp(connStr[4], optarg)==0) {
		#ifdef HAVE_XRCD
		*cptr = XRC;
		#else
		fprintf(stderr," XRC not detected in libibverbs\n");
		exit(1);
		#endif
	} else if (strcmp(connStr[5], optarg)==0) {
		#ifdef HAVE_MLX5DV
		*cptr = DC;
		#else
		fprintf(stderr," DC not supported, mlx5dv.h is needed\n");
		exit(1);
		#endif
	} else if (strcmp(connStr[6], optarg) == 0) {
		#ifdef HAVE_SRD
		if (verb != SEND && verb != READ && verb != WRITE && verb != WRITE_IMM) {
			fprintf(stderr, " SRD connection only possible in SEND/READ/WRITE/WRITE_IMM verbs\n");
			exit(1);
		}
		*cptr = SRD;
		#else
		fprintf(stderr, " SRD not detected in libibverbs\n");
		exit(1);
		#endif
	} else {
		fprintf(stderr, " Invalid Connection type. Please choose from {RC,UC,UD,XRC,DC,SRD}\n");
		exit(1);
	}
}

#ifdef HAVE_HNSDV
static void set_congest_type(int *cgtr, const char *optarg)
{
	if (strcmp(congestStr[0], optarg) == 0) {
		*cgtr = HNSDV_QP_CREATE_ENABLE_DCQCN;
	} else if (strcmp(congestStr[1], optarg) == 0) {
		*cgtr = HNSDV_QP_CREATE_ENABLE_LDCP;
	} else if (strcmp(congestStr[2], optarg) == 0) {
		*cgtr = HNSDV_QP_CREATE_ENABLE_HC3;
	} else if (strcmp(congestStr[3], optarg) == 0) {
		*cgtr = HNSDV_QP_CREATE_ENABLE_DIP;
	} else {
		fprintf(stderr, " Invalid congest type. Please choose from {DCQCN,LDCP,HC3,DIP}\n");
		exit(1);
	}
}
#endif

/******************************************************************************
 *
 ******************************************************************************/
int set_eth_mtu(struct perftest_parameters *user_param)
{
	if (user_param->mtu == 0) {
		user_param->mtu = 1518;
	}

	if(user_param->mtu >= MIN_MTU_RAW_ETERNET && user_param->mtu <= MAX_MTU_RAW_ETERNET) {
		user_param->curr_mtu = user_param->mtu;

	} else {

		fprintf(stderr," Invalid MTU - %d \n",user_param->mtu);
		fprintf(stderr," Please choose mtu form {64, 9600}\n");
		return -1;
	}

	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
void print_supported_ibv_rate_values()
{
	int i;
	for (i = 0; i < RATE_VALUES_COUNT; i++)
		printf("\t\t\t %s Gbps \t\t\n", RATE_VALUES[i].rate_gbps_str);
}

/******************************************************************************
 *
 ******************************************************************************/
void  get_gbps_str_by_ibv_rate(char *rate_input_value, int *rate)
{
	int i;
	for (i = 0; i < RATE_VALUES_COUNT; i++) {
		if (strcmp(rate_input_value, RATE_VALUES[i].rate_gbps_str) == 0) {
			*rate = (int)RATE_VALUES[i].rate_gbps_enum;
			return;
		}
	}
	printf("\x1b[31mThe input value for hw rate limit is not supported\x1b[0m\n");
	print_supported_ibv_rate_values();
}

/******************************************************************************
 *
 ******************************************************************************/
void flow_rules_force_dependecies(struct perftest_parameters *user_param)
{
	if (user_param->flows != DEF_FLOWS) {
		if (user_param->is_server_port == OFF) {
			fprintf(stderr, " Flows feature works with UDP/TCP packets only for now\n");
			exit(1);
		}
		if (user_param->test_type == ITERATIONS) {
			int min_iter_req = user_param->flows * user_param->flows_burst;
			if (user_param->iters / min_iter_req < 1) {
				fprintf(stderr, " Current iteration number will not complete full cycle on all flows, it need to be multiple of the product between flows and flows_burst\n");
				fprintf(stderr, " Set  N*%d Iterations \n", user_param->flows * user_param->flows_burst);
				exit(FAILURE);
			}
		}
		if (user_param->tst == FS_RATE) {
			fprintf(stderr, "FS rate test not requiring flows parameter\n");
			exit(FAILURE);
		}
		if (user_param->duplex) {
			fprintf(stderr, " Flows is currently designed to work with unidir tests only\n");
			exit(FAILURE);
		}
	} else {
		if (user_param->flows_burst  > 1) {
			fprintf(stderr, " Flows burst is designed to work with more then single flow\n");
			exit(FAILURE);
		}
	}
	return;
}

/******************************************************************************
 *
 ******************************************************************************/
static void force_dependecies(struct perftest_parameters *user_param)
{
	/*Additional configuration and assignments.*/
	if (user_param->verb == WRITE) {
		user_param->rx_depth = DEF_RX_RDMA;
	}

	if (user_param->test_method != RUN_INFINITELY && user_param->test_type == ITERATIONS) {
		if (user_param->tx_depth > user_param->iters) {
			user_param->tx_depth = user_param->iters;
		}

		if ((user_param->verb == SEND || user_param->verb == WRITE_IMM) &&
				user_param->rx_depth > user_param->iters) {
			user_param->rx_depth = user_param->iters;
		}

		if (user_param->connection_type == UD || user_param->connection_type == UC) {
			if (user_param->use_srq && user_param->iters <= MIN_SRQ_UD_RX_DEPTH) {
			        user_param->rx_depth = MIN_SRQ_UD_RX_DEPTH;
			}

			if (user_param->rx_depth == DEF_RX_SEND) {
				user_param->rx_depth = (user_param->iters < UC_MAX_RX) ? user_param->iters : UC_MAX_RX;
			}
		}
	}

	/* we disable cq_mod for large message size to prevent from incorrect BW calculation
	 *    (and also because it is not needed)
	 * we don't disable cq_mod for UD because it doesn't support large enough messages
	 * we don't disable cq_mod for RUN_ALL mode because we cannot change it in accordance to
	 *    message size during RUN_ALL run
	 * we don't disable cq_mod for use_event, because having a lot of processes with use_event leads
	 *     to bugs (probably due to issues with events processing, thus we have less events)
	 */
	if (user_param->size > MSG_SIZE_CQ_MOD_LIMIT &&
		user_param->connection_type != UD &&
		user_param->test_method != RUN_ALL &&
		!user_param->use_event)
	{
		if (!user_param->req_cq_mod) // user didn't request any cq_mod
		{
			user_param->cq_mod = DISABLED_CQ_MOD_VALUE;
		}
		else if (user_param->cq_mod > DISABLED_CQ_MOD_VALUE)
		{
			printf(RESULT_LINE);
			printf("Warning: Large message requested and CQ moderation enabled\n");
			printf("Warning: It can lead to inaccurate results\n");
		}
	}

	if (user_param->tst == LAT_BY_BW && user_param->rate_limit_type == DISABLE_RATE_LIMIT) {
		if (user_param->output == FULL_VERBOSITY)
			printf("rate_limit type is forced to SW.\n");
		user_param->rate_limit_type = SW_RATE_LIMIT;
	}

	if (user_param->cq_mod > user_param->tx_depth) {
		user_param->cq_mod = user_param->tx_depth;
	}

	if (user_param->verb == READ || user_param->verb == ATOMIC)
		user_param->inline_size = 0;

	if (user_param->test_method == RUN_ALL)
		user_param->size = MAX_SIZE;

	if (user_param->verb == ATOMIC && user_param->size != DEF_SIZE_ATOMIC) {
		printf(RESULT_LINE);
		printf("Message size cannot be changed for Atomic tests \n");
		exit (1);
	}

	if (user_param->use_srq && user_param->verb != SEND) {
		printf(RESULT_LINE);
		printf(" Using SRQ only avavilible in SEND tests.\n");
		exit (1);
	}

	/* XRC Part */
	if (user_param->connection_type == XRC) {
		if (user_param->work_rdma_cm == ON) {
			printf(RESULT_LINE);
			fprintf(stderr," XRC does not support RDMA_CM\n");
			exit(1);
		}
		user_param->use_xrc = ON;
		user_param->use_srq = ON;
	}

	if (user_param->use_srq && user_param->verb == SEND &&
	    user_param->num_of_qps > user_param->rx_depth) {
		printf(RESULT_LINE);
		printf(" Using SRQ depth should be greater than number of QPs.\n");
		exit (1);
	}

#ifdef HAVE_AES_XTS
	switch (user_param->aes_block_size)
	{
		case AES_XTS_BLOCK_SIZE_520:
			user_param->aes_block_size = MLX5DV_BLOCK_SIZE_520;
			break;

		case AES_XTS_BLOCK_SIZE_4048:
			user_param->aes_block_size = MLX5DV_BLOCK_SIZE_4048;
			break;

		case AES_XTS_BLOCK_SIZE_4096:
			user_param->aes_block_size = MLX5DV_BLOCK_SIZE_4096;
			break;

		case AES_XTS_BLOCK_SIZE_4160:
			user_param->aes_block_size = MLX5DV_BLOCK_SIZE_4160;
			break;

		default:
			user_param->aes_block_size = MLX5DV_BLOCK_SIZE_512;
			break;
	}
#endif

	if (user_param->dualport == ON) {

		user_param->num_of_qps *= 2;
		if (user_param->tst != BW) {
			printf(" Dual-port mode only supports BW tests.\n");
			exit (1);
		}

		if (user_param->use_mcg){
			printf(" Dual-port mode not supported in multicast feature\n");
			exit (1);
		}
		if (user_param->link_type != LINK_UNSPEC)
			user_param->link_type2 = user_param->link_type;
	}

	if (user_param->post_list > 1) {
		if (user_param->tst == BW || user_param->tst == LAT_BY_BW)
		{
			if (user_param->test_type == ITERATIONS && (user_param->iters % user_param->post_list) != 0) {
				printf(RESULT_LINE);
				fprintf(stderr, "Number of iterations must be a multiple of post list size\n");
				exit(1);
			}
			if (!user_param->req_cq_mod)
			{
				user_param->cq_mod = user_param->post_list;
				printf(RESULT_LINE);
				printf("Post List requested - CQ moderation will be the size of the post list\n");
			}
			else if ((user_param->post_list % user_param->cq_mod) != 0)
			{
				printf(RESULT_LINE);
				fprintf(stderr, "Post list size must be a multiple of CQ moderation\n");
				exit(1);
			}
		}
		else
		{
			printf(RESULT_LINE);
			fprintf(stderr, "Post list is supported in BW tests only\n");
			exit(1);
		}
	}
	if (user_param->recv_post_list > 1) {
		if (user_param->tst == BW || user_param->tst == LAT_BY_BW) {
			if (user_param->test_type == ITERATIONS && (user_param->iters % user_param->recv_post_list) != 0) {
				printf(RESULT_LINE);
				fprintf(stderr, "Number of iterations must be a multiple of receive post list size\n");
				exit(1);
			}
		}
		else
		{
			printf(RESULT_LINE);
			fprintf(stderr, "Receive post list is supported in BW tests only\n");
			exit(1);
		}
	}

	if (user_param->test_type==DURATION) {

		/* When working with Duration, iters=0 helps us to satisfy loop cond. in run_iter_bw.
		We also use it for "global" counter of packets.
		*/
		user_param->iters = 0;
		if (user_param->noPeak == OFF && user_param->tst == BW)
			printf(" WARNING: BW peak won't be measured in this run.\n");
		user_param->noPeak = ON;

		if (user_param->use_event) {
			printf(RESULT_LINE);
			fprintf(stderr,"Duration mode doesn't work with events.\n");
			exit(1);
		}

		if (user_param->test_method == RUN_ALL) {
			printf(RESULT_LINE);
			fprintf(stderr, "Duration mode currently doesn't support running on all sizes.\n");
			exit(1);
		}
		if (user_param->cpu_util) {
			user_param->cpu_util_data.enable = 1;
		}
	}

	if ( (user_param->test_type != DURATION) && user_param->cpu_util ) {
		printf(RESULT_LINE);
		fprintf(stderr, " CPU Utilization works only with Duration mode.\n");
	}

	if (user_param->connection_type == RawEth) {
		user_param->use_old_post_send = 1;
		if (user_param->test_method == RUN_ALL)
		{
			fprintf(stderr, "Raw Ethernet tests do not support -a / --all flag.\n");
			exit(1);
		}

		if (user_param->use_rdma_cm == ON || user_param->work_rdma_cm == ON) {
			fprintf(stderr," RDMA CM isn't supported for Raw Ethernet tests\n");
			exit(1);
		}

		if (user_param->use_gid_user) {
			fprintf(stderr," GID index isn't supported for Raw Ethernet tests\n");
			exit(1);
		}

		if (user_param->memory_type == MEMORY_MMAP) {
			fprintf(stderr," mmaped files aren't supported for Raw Ethernet tests\n");
			exit(1);
		}

		if(user_param->machine == UNCHOSEN) {
			printf(RESULT_LINE);
			fprintf(stderr," Invalid Command line.\n you must choose test side --client or --server\n");
			exit(1);
		}

		/* Verify the packet */
		if(user_param->is_source_mac == OFF) {
			printf(RESULT_LINE);
			fprintf(stderr," Invalid Command line.\n you must enter source mac by this format -B XX:XX:XX:XX:XX:XX\n");
			exit(1);
		}

		if(user_param->is_dest_mac == OFF && (user_param->tst == LAT || (user_param->machine == CLIENT && !user_param->raw_mcast))) {
			printf(RESULT_LINE);
			fprintf(stderr," Invalid Command line.\n you must enter dest mac by this format -E XX:XX:XX:XX:XX:XX\n");
			exit(1);
		}

		if((user_param->is_server_port == ON && user_param->is_client_port == OFF) || (user_param->is_server_port == OFF && user_param->is_client_port == ON)) {
			printf(RESULT_LINE);
			fprintf(stderr," Invalid Command line.\n if you would like to send UDP header,\n you must enter server&client port --server_port X --client_port X\n");
			exit(1);
		}

		if ((user_param->is_server_port == ON) && (user_param->is_server_ip == OFF || user_param->is_client_ip == OFF)) {
			printf(RESULT_LINE);
			fprintf(stderr," Invalid Command line.\nPlease provide source_ip and/or dest_ip when using UDP\n");
			exit(1);
		}
		/* UDP packet is ok by now. check tcp flag */
		if (user_param->tcp == ON && user_param->is_server_port == OFF) {
			printf(RESULT_LINE);
			fprintf(stderr,"Invalid Command line.\nPlease provide UDP information (IP & UDP Port src/dest) in order to use TCP\n");
			exit(1);
		}

		/* Mac forwarding dependencies */
		if (user_param->duplex == OFF && user_param->mac_fwd == ON) {
			printf("mac_fwd should run in duplex mode only. changing to duplex mode.\n");
			user_param->duplex = ON;
		}
		if (user_param->mac_fwd == ON && user_param->cq_mod >= user_param->rx_depth) {
			fprintf(stderr," CQ moderation can't be grater than rx depth.\n");
			user_param->cq_mod = user_param->rx_depth < user_param->tx_depth ? user_param->rx_depth : user_param->tx_depth;
			fprintf(stderr," Changing CQ moderation to min( rx depth , tx depth) = %d.\n",user_param->cq_mod);
		}

		if (user_param->raw_mcast && user_param->duplex) {
			fprintf(stderr, " Multicast feature works on unidirectional traffic only\n");
			exit(1);
		}

		flow_rules_force_dependecies(user_param);
	}

	if (user_param->use_mcg &&  user_param->gid_index == -1) {
		user_param->gid_index = 0;
	}

	if (user_param->work_rdma_cm) {

		if (user_param->connection_type == UC) {
			printf(RESULT_LINE);
			printf(" UC is not supported in librdmacm\n");
			exit(1);
		}

		if (user_param->use_mcg) {
			printf(RESULT_LINE);
			printf(" Perftest still doesn't support Multicast with rdma_cm\n");
			exit(1);
		}

		if (user_param->dualport) {
			printf(RESULT_LINE);
			printf(" Perftest still doesn't support Dual Port with rdma_cm\n");
			exit(1);
		}

		user_param->use_rdma_cm = ON;

	} else if (user_param->tos != DEF_TOS && user_param->connection_type != RawEth) {
		fprintf(stdout," TOS only valid for rdma_cm based QP and RawEth QP \n");
		exit(1);
	}

	if (user_param->hop_limit != DEF_HOP_LIMIT && user_param->connection_type != RawEth) {
		fprintf(stdout," Hop limit only valid for RawEth QP \n");
		exit(1);
	}

	if (user_param->use_mcg) {

		if (user_param->connection_type != UD)
			user_param->connection_type = UD;

		if (user_param->duplex) {
			fprintf(stdout,"Bidirectional mode not supported in multicast\n");
			exit (1);
		}

		if (user_param->num_of_qps > 1) {
			fprintf(stdout,"Only 1 QP supported in multicast\n");
			exit(1);
		}
	}

	if ((user_param->verb == SEND || user_param->verb == WRITE_IMM) && user_param->tst == BW
			&& user_param->machine == SERVER && !user_param->duplex ) {
		if (user_param->noPeak == OFF)
			printf(" WARNING: BW peak won't be measured in this run.\n");
		user_param->noPeak = ON;
	}

	/* Run infinitely dependencies */
	if (user_param->test_method == RUN_INFINITELY) {
		if (user_param->noPeak == OFF && user_param->tst == BW)
			printf(" WARNING: BW peak won't be measured in this run.\n");
		user_param->noPeak = ON;
		user_param->test_type = DURATION;
		if (user_param->use_event) {
			printf(RESULT_LINE);
			fprintf(stderr," run_infinitely does not support events feature yet.\n");
			exit(1);
		}

		if (user_param->tst == LAT) {
			printf(RESULT_LINE);
			fprintf(stderr," run_infinitely exists only in BW tests for now.\n");
			exit(1);

		}

		if (user_param->duplex && (user_param->verb == SEND || user_param->verb == WRITE_IMM)) {
			printf(RESULT_LINE);
			fprintf(stderr," run_infinitely mode is not supported in SEND or WRITE_IMM "
					"Bidirectional BW test\n");
			exit(1);
		}
		if (user_param->rate_limit_type != DISABLE_RATE_LIMIT) {
			printf(RESULT_LINE);
			fprintf(stderr," run_infinitely does not support rate limit feature yet\n");
			exit(1);
		}
	}

	if (user_param->connection_type == DC && !user_param->use_srq)
		user_param->use_srq = ON;

	if (!user_param->use_old_post_send)
	{
		#ifndef HAVE_IBV_WR_API
		printf(RESULT_LINE);
		fprintf(stderr, " new post send flow is not supported, falling back to ibv_post_send\n");
		user_param->use_old_post_send = 1;
		#endif
	}

	if (user_param->connection_type == DC)
	{
		if (user_param->use_old_post_send)
		{
			printf(RESULT_LINE);
			fprintf(stderr, " DC does not support old post send flow\n");
			exit(1);
		}
		if (user_param->work_rdma_cm == ON)
		{
			printf(RESULT_LINE);
			fprintf(stderr, " DC does not support RDMA_CM\n");
			exit(1);
		}
	}

	if(user_param->log_dci_streams) {
		#ifdef HAVE_DCS
		if(user_param->connection_type != DC) {
			printf(RESULT_LINE);
			fprintf(stderr," log_dci_streams supported only on DC\n");
			exit(1);
		}
		#else
		printf(RESULT_LINE);
		fprintf(stderr," log_dci_streams not supported\n");
		exit(1);
		#endif
	}
	if(user_param->log_active_dci_streams > 0){
		#ifdef HAVE_DCS
		if(user_param->connection_type != DC){
			printf(RESULT_LINE);
			fprintf(stderr," log_active_dci_streams supported only on DC\n");
			exit(1);
		}
		if(user_param->log_dci_streams == 0){
			printf(RESULT_LINE);
			fprintf(stderr," log_dci_streams must be greater than 0\n");
			exit(1);
		}
		#else
		printf(RESULT_LINE);
		fprintf(stderr," log_active_dci_streams not supported\n");
		exit(1);
		#endif
	}
	if(user_param->aes_xts) {
		#ifdef HAVE_AES_XTS
		if(user_param->connection_type != RC) {
			printf(RESULT_LINE);
			fprintf(stderr," aes_xts supported only on RC\n");
			exit(1);
		}
		if(user_param->kek_path == NULL) {
			printf(RESULT_LINE);
			fprintf(stderr," Please provide key encryption key file path\n");
			exit(1);
		}
		if(user_param->data_enc_key_app_path == NULL) {
			printf(RESULT_LINE);
			fprintf(stderr," Please provide data encryption key app path\n");
			exit(1);
		}
		if(user_param->credentials_path == NULL) {
			printf(RESULT_LINE);
			fprintf(stderr," Please provide credentials file path\n");
			exit(1);
		}
		if(user_param->use_old_post_send) {
			printf(RESULT_LINE);
			fprintf(stderr," aes_xts doesn't support old post send\n");
			exit(1);
		}
		if(user_param->work_rdma_cm) {
			printf(RESULT_LINE);
			fprintf(stderr," rdma_cm doesn't support aes_xts\n");
			exit(1);
		}
		if(user_param->tst == LAT && (user_param->verb == WRITE || user_param->verb == WRITE_IMM)) {
			printf(RESULT_LINE);
			fprintf(stderr," aes_xts isn't supported on write_lat\n");
			exit(1);
		}
		if((int)user_param->size <= user_param->inline_size) {
			printf(RESULT_LINE);
			fprintf(stderr," aes_xts doesn't support Inline messages\n");
			exit(1);
		}
		#else
		printf(RESULT_LINE);
		fprintf(stderr," aes_xts not supported\n");
		exit(1);
		#endif
	}
	else {
		if(user_param->encrypt_on_tx) {
			printf(RESULT_LINE);
			fprintf(stderr," encrypt_on_tx supported only when enc/dec traffic\n");
			exit(1);
		}
		if(user_param->sig_before) {
			printf(RESULT_LINE);
			fprintf(stderr," sig_before supported only when enc/dec traffic\n");
			exit(1);
		}
		if(user_param->credentials_path){
			printf(RESULT_LINE);
			fprintf(stderr," giving credentials path supported only with enc/dec traffic\n");
			exit(1);
		}
		if(user_param->kek_path){
			printf(RESULT_LINE);
			fprintf(stderr," giving kek path supported only with enc/dec traffic\n");
			exit(1);
		}
		if(user_param->data_enc_key_app_path){
			printf(RESULT_LINE);
			fprintf(stderr," giving kek path supported only with enc/dec traffic\n");
			exit(1);
		}
	}

	if (user_param->connection_type == SRD) {
		if (user_param->work_rdma_cm == ON) {
			printf(RESULT_LINE);
			fprintf(stderr, " SRD does not support RDMA_CM\n");
			exit(1);
		}
		user_param->cq_mod = 1;
	}

	if ((user_param->use_srq && (user_param->tst == LAT || user_param->machine == SERVER || user_param->duplex == ON)) || user_param->use_xrc)
		user_param->srq_exists = 1;

	if (user_param->burst_size > 0) {
		if (user_param->rate_limit_type == DISABLE_RATE_LIMIT && user_param->tst != LAT_BY_BW ) {
			printf(RESULT_LINE);
			fprintf(stderr," Can't enable burst mode when rate limiter is off\n");
			exit(1);
		}
	}

	if (user_param->burst_size <= 0) {
		if (user_param->rate_limit_type == SW_RATE_LIMIT)
			fprintf(stderr," Setting burst size to tx depth = %d\n", user_param->tx_depth);

		if (user_param->rate_limit_type != PP_RATE_LIMIT)
			user_param->burst_size = user_param->tx_depth;
	}

	if (user_param->typical_pkt_size &&
	    user_param->rate_limit_type != PP_RATE_LIMIT){
		printf(RESULT_LINE);
		fprintf(stderr," Typical packet size only supports PP rate limiter\n");
		exit(1);
	}

	if (user_param->rate_limit_type == SW_RATE_LIMIT) {
		if (user_param->tst != BW || user_param->verb == ATOMIC || (user_param->verb == SEND && user_param->duplex)) {
			printf(RESULT_LINE);
			fprintf(stderr,"SW Rate limiter cann't be executed on non-BW, ATOMIC or bidirectional SEND tests\n");
			exit(1);
		}
	} else if (user_param->rate_limit_type == HW_RATE_LIMIT) {
		if (user_param->use_rdma_cm == ON || user_param->work_rdma_cm == ON) {
			fprintf(stderr," HW rate limit isn't supported yet with rdma_cm scenarios\n");
			exit(1);
		}
		double rate_limit_gbps = 0;
		switch (user_param->rate_units) {
			case MEGA_BYTE_PS:
				rate_limit_gbps =((double)(((user_param->rate_limit)*8*1024*1024) / 1000000000));
				break;
			case GIGA_BIT_PS:
				rate_limit_gbps = user_param->rate_limit;
				break;
			case PACKET_PS:
				printf(RESULT_LINE);
				fprintf(stderr, " Failed: pps rate limit units is not supported when setting HW rate limit\n");
				exit(1);
			default:
				printf(RESULT_LINE);
				fprintf(stderr, " Failed: Unknown rate limit units\n");
				exit(1);
		}
		if (rate_limit_gbps > 0) {
			int rate_index_to_set = -1;
			get_gbps_str_by_ibv_rate(user_param->rate_limit_str, &rate_index_to_set);
			if (rate_index_to_set == -1) {
				printf(RESULT_LINE);
				fprintf(stderr, " Failed: Unknown rate limit value\n");
				exit(1);
			}
			user_param->valid_hw_rate_limit_index = rate_index_to_set;
		}
	} else if (user_param->rate_limit_type == PP_RATE_LIMIT) {
		if (user_param->rate_limit < 0) {
			fprintf(stderr," Must specify a rate limit when using Packet Pacing.\n Please add --rate_limit=<limit>.\n");
			exit(1);
		}
		if (user_param->connection_type != RawEth) {
			fprintf(stderr,"Packet Pacing is only supported for Raw Ethernet.\n");
			exit(1);
		}

		if (user_param->rate_units != MEGA_BYTE_PS) {
			fprintf(stderr,"Packet Pacing only supports MEGA_BYTE_PS.\n");
			exit(1);
		}

		user_param->rate_limit = user_param->rate_limit * 8 * 1024;
	}

	if (user_param->tst == LAT_BY_BW) {
		if ( user_param->test_type == DURATION) {
			fprintf(stderr, "Latency under load test is currently support iteration mode only.\n");
			exit(1);
		}
		if (user_param->num_of_qps > 1) {
			fprintf(stderr, "Multi QP is not supported in LAT under load test\n");
			exit(1);
		}
		if (user_param->duplex) {
			fprintf(stderr, "Bi-Dir is not supported in LAT under load test\n");
			exit(1);
		}
		if(user_param->output != FULL_VERBOSITY && user_param->output != OUTPUT_LAT) {
			printf(RESULT_LINE);
			fprintf(stderr," Output verbosity level for BW can be latency\n");
			exit(1);
		}
	}

	if (user_param->output != FULL_VERBOSITY) {
		if (user_param->tst == BW && !(user_param->output == OUTPUT_BW || user_param->output == OUTPUT_MR)) {
			printf(RESULT_LINE);
			fprintf(stderr," Output verbosity level for BW can be: bandwidth, message_rate\n");
			exit(1);
		}

		if (user_param->tst == LAT && !(user_param->output == OUTPUT_LAT)) {
			printf(RESULT_LINE);
			fprintf(stderr," Output verbosity level for LAT can be: latency\n");
			exit(1);
		}
	}

	if ( (user_param->latency_gap > 0) && user_param->tst != LAT ) {
		printf(RESULT_LINE);
		fprintf(stderr," Latency gap feature is only for latency tests\n");
		exit(1);
	}

	if ( user_param->test_type == DURATION && user_param->margin == DEF_INIT_MARGIN) {
		user_param->margin = user_param->duration / 4;
	}

	if (user_param->use_null_mr && !(user_param->verb == WRITE || user_param->verb == WRITE_IMM || user_param->verb == READ)) {
		printf(RESULT_LINE);
		fprintf(stderr, "Perftest supports using a null memory region with write/read verbs only\n");
		exit(1);
	}

	if (user_param->memory_type == MEMORY_CUDA && user_param->tst == LAT && (user_param->verb == WRITE || user_param->verb == WRITE_IMM)) {
		printf(RESULT_LINE);
		fprintf(stderr,"Perftest supports CUDA latency tests with read/send verbs only\n");
		exit(1);
	}

	if (user_param->memory_type == MEMORY_CUDA && (int)user_param->size <= user_param->inline_size) {
		printf(RESULT_LINE);
		fprintf(stderr,"Perftest doesn't support CUDA tests with inline messages\n");
		exit(1);
	}

	if ( (user_param->connection_type == UD) && (user_param->inline_size > MAX_INLINE_UD) ) {
		printf(RESULT_LINE);
		fprintf(stderr, "Setting inline size to %d (Max inline size in UD)\n",MAX_INLINE_UD);
		user_param->inline_size = MAX_INLINE_UD;
	}

	if (user_param->report_per_port && (user_param->test_type != DURATION || !user_param->dualport)) {
		printf(RESULT_LINE);
		fprintf(stderr, "report per port feature work only with Duration and Dualport\n");
		exit(1);
	}

	/* WA for a bug when rx_depth is odd in SEND */
	if ((user_param->verb == SEND || user_param->verb == WRITE_IMM) && (user_param->rx_depth % 2 == 1) && user_param->test_method == RUN_REGULAR)
		user_param->rx_depth += 1;

	if (user_param->test_type == ITERATIONS && user_param->iters > 20000 && user_param->noPeak == OFF && user_param->tst == BW) {
		printf(" WARNING: BW peak won't be measured in this run.\n");
		user_param->noPeak = ON;
	}

	if (!(user_param->duration > 2*user_param->margin)) {
		printf(RESULT_LINE);
		fprintf(stderr, "please check that DURATION > 2*MARGIN\n");
		exit(1);
	}

	if((user_param->use_event == OFF) && user_param->use_eq_num == ON) {
		fprintf(stderr, " Events must be enabled to select a completion vector\n");
		exit(1);
	}

	user_param->fill_count = 0;
	if (user_param->test_type == ITERATIONS) {
		if (user_param->cq_mod >= user_param->tx_depth && user_param->iters % user_param->tx_depth) {
			user_param->fill_count = 1;
		} else if (user_param->cq_mod < user_param->tx_depth && user_param->iters % user_param->cq_mod) {
			user_param->fill_count = 1;
		}
	}

	#ifdef HAVE_HNSDV
	if (user_param->congest_type) {
		if (user_param->work_rdma_cm == ON)
		{
			printf(RESULT_LINE);
			fprintf(stderr, "rdma_cm does not support setting congest type.\n");
			exit(1);
		}

		if (user_param->connection_type == XRC || user_param->connection_type == UD) {
			printf(RESULT_LINE);
			fprintf(stdout, "XRC/UD does not support setting congest type.\n");
			exit(1);
		}
	}
	#endif

	return;
}
/******************************************************************************
 *
 ******************************************************************************/
const char *transport_str(enum ibv_transport_type type)
{
	switch (type) {
		case IBV_TRANSPORT_IB:
			return "IB";
			break;
		case IBV_TRANSPORT_IWARP:
			return "IW";
			break;
		default:
			return "Unknown";
	}
}

/******************************************************************************
 * Try to map verbs' link layer types to a descriptive string or "Unknown"
 ******************************************************************************/
const char *link_layer_str(int8_t link_layer)
{
	switch (link_layer) {

		case IBV_LINK_LAYER_UNSPECIFIED:
		case IBV_LINK_LAYER_INFINIBAND:
			return "IB";
		case IBV_LINK_LAYER_ETHERNET:
			return "Ethernet";
		default:
			return "Unknown";
	}
}

/******************************************************************************
 * Try to parse a string to a verbs link layer or LINK_FAILURE
 ******************************************************************************/
const int str_link_layer(const char *str)
{
	if (strncasecmp("IB", str, 2) == 0)
		return IBV_LINK_LAYER_INFINIBAND;
	else if (strncasecmp("Ethernet", str, 8) == 0)
		return IBV_LINK_LAYER_ETHERNET;
	else
		return LINK_FAILURE;
}

/******************************************************************************
 *
 ******************************************************************************/
enum ctx_device ib_dev_name(struct ibv_context *context)
{
	enum ctx_device dev_fname = UNKNOWN;
	struct ibv_device_attr attr;

	if (ibv_query_device(context,&attr)) {
		dev_fname = DEVICE_ERROR;
	}

	else if (attr.vendor_id == 5157) {

		switch (attr.vendor_part_id >> 12) {
			case 10 :
			case 4  : dev_fname = CHELSIO_T4; break;
			case 11 :
			case 5  : dev_fname = CHELSIO_T5; break;
			case 6  : dev_fname = CHELSIO_T6; break;
			default : dev_fname = UNKNOWN; break;
		}

		/* Assuming it's Mellanox HCA or unknown.
		If you want Inline support in other vendor devices, please send patch to gilr@dev.mellanox.co.il
		*/
	} else if (attr.vendor_id == 0x8086) {
		switch (attr.vendor_part_id) {
			case 14289 : dev_fname = INTEL_GEN1; break;
			case 5522  : dev_fname = INTEL_GEN2; break;
			default    : dev_fname = INTEL_GEN2; break;
		}
	} else {

		//coverity[uninit_use]
		switch (attr.vendor_part_id) {
			case 4099  : dev_fname = CONNECTX3; break;
			case 4100  : dev_fname = CONNECTX3; break;
			case 4103  : dev_fname = CONNECTX3_PRO; break;
			case 4104  : dev_fname = CONNECTX3_PRO; break;
			case 4113  : dev_fname = CONNECTIB; break;
			case 4115  : dev_fname = CONNECTX4; break;
			case 4116  : dev_fname = CONNECTX4; break;
			case 4117  : dev_fname = CONNECTX4LX; break;
			case 4118  : dev_fname = CONNECTX4LX; break;
			case 4119  : dev_fname = CONNECTX5; break;
			case 4120  : dev_fname = CONNECTX5; break;
			case 4121  : dev_fname = CONNECTX5EX; break;
			case 4122  : dev_fname = CONNECTX5EX; break;
			case 4123  : dev_fname = CONNECTX6; break;
			case 4124  : dev_fname = CONNECTX6; break;
			case 4125  : dev_fname = CONNECTX6DX; break;
			case 4126  : dev_fname = MLX5GENVF; break;
			case 4127  : dev_fname = CONNECTX6LX; break;
			case 4129  : dev_fname = CONNECTX7; break;
			case 4131  : dev_fname = CONNECTX8; break;
			case 41682 : dev_fname = BLUEFIELD; break;
			case 41683 : dev_fname = BLUEFIELD; break;
			case 41686 : dev_fname = BLUEFIELD2; break;
			case 41692 : dev_fname = BLUEFIELD3; break;
			case 26418 : dev_fname = CONNECTX2; break;
			case 26428 : dev_fname = CONNECTX2; break;
			case 26438 : dev_fname = CONNECTX2; break;
			case 26448 : dev_fname = CONNECTX2; break;
			case 26458 : dev_fname = CONNECTX2; break;
			case 26468 : dev_fname = CONNECTX2; break;
			case 26478 : dev_fname = CONNECTX2; break;
			case 25408 : dev_fname = CONNECTX;  break;
			case 25418 : dev_fname = CONNECTX;  break;
			case 25428 : dev_fname = CONNECTX;  break;
			case 25448 : dev_fname = CONNECTX;  break;
			case 1824  : dev_fname = SKYHAWK;   break;
			case 5684  : dev_fname = QLOGIC_E4; break;
			case 5700  : dev_fname = QLOGIC_E4; break;
			case 5716  : dev_fname = QLOGIC_E4; break;
			case 5718  : dev_fname = QLOGIC_E4; break;
			case 5734  : dev_fname = QLOGIC_E4; break;
			case 32880 : dev_fname = QLOGIC_AH; break;
			case 32881 : dev_fname = QLOGIC_AH; break;
			case 32882 : dev_fname = QLOGIC_AH; break;
			case 32883 : dev_fname = QLOGIC_AH; break;
			case 32912 : dev_fname = QLOGIC_AH; break;
			case 33136 : dev_fname = QLOGIC_AHP; break;
			case 33168 : dev_fname = QLOGIC_AHP; break;
			case 5638  : dev_fname = NETXTREME; break;
			case 5652  : dev_fname = NETXTREME; break;
			case 5824  : dev_fname = NETXTREME; break;
			case 5825  : dev_fname = NETXTREME; break;
			case 5827  : dev_fname = NETXTREME; break;
			case 5839  : dev_fname = NETXTREME; break;
			case 5846  : dev_fname = NETXTREME; break;
			case 5847  : dev_fname = NETXTREME; break;
			case 5848  : dev_fname = NETXTREME; break;
			case 5849  : dev_fname = NETXTREME; break;
			case 5855  : dev_fname = NETXTREME; break;
			case 5858  : dev_fname = NETXTREME; break;
			case 5859  : dev_fname = NETXTREME; break;
			case 5861  : dev_fname = NETXTREME; break;
			case 5867  : dev_fname = NETXTREME; break;
			case 5869  : dev_fname = NETXTREME; break;
			case 5871  : dev_fname = NETXTREME; break;
			case 5872  : dev_fname = NETXTREME; break;
			case 5873  : dev_fname = NETXTREME; break;
			case 5968  : dev_fname = NETXTREME; break;
			case 5984 : dev_fname = NETXTREME; break;
			case 6169 : dev_fname = NETXTREME; break;
			case 55296 : dev_fname = NETXTREME; break;
			case 55298 : dev_fname = NETXTREME; break;
			case 55300 : dev_fname = NETXTREME; break;
			case 61344 : dev_fname = EFA; break; /* efa0 */
			case 61345 : dev_fname = EFA; break; /* efa1 */
			case 61346 : dev_fname = EFA; break; /* efa2 */
			case 4223  : dev_fname = ERDMA; break;
			case 41506 : dev_fname = HNS; break;
			case 41507 : dev_fname = HNS; break;
			case 41508 : dev_fname = HNS; break;
			case 41509 : dev_fname = HNS; break;
			case 41510 : dev_fname = HNS; break;
			case 41512 : dev_fname = HNS; break;
			case 41519 : dev_fname = HNS; break;
			default	   : dev_fname = UNKNOWN;
		}
	}

	return dev_fname;
}

/******************************************************************************
 *
 ******************************************************************************/
enum ibv_mtu set_mtu(struct ibv_context *context,uint8_t ib_port,int user_mtu)
{
	struct ibv_port_attr port_attr;
	enum ibv_mtu curr_mtu;

	if (ibv_query_port(context,ib_port,&port_attr)) {
		fprintf(stderr," Error when trying to query port\n");
		exit(1);
	}

	/* User did not ask for specific mtu. */
	if (user_mtu == 0) {
		enum ctx_device current_dev = ib_dev_name(context);
		curr_mtu = port_attr.active_mtu;
		/* CX3_PRO and CX3 have a HW bug in 4K MTU, so we're forcing it to be 2K MTU */
		if (curr_mtu == IBV_MTU_4096 && (current_dev == CONNECTX3_PRO || current_dev == CONNECTX3))
			curr_mtu = IBV_MTU_2048;
	}

	else {
		switch (user_mtu) {
			case 256  :	curr_mtu = IBV_MTU_256;	 break;
			case 512  : curr_mtu = IBV_MTU_512;	 break;
			case 1024 :	curr_mtu = IBV_MTU_1024; break;
			case 2048 :	curr_mtu = IBV_MTU_2048; break;
			case 4096 :	curr_mtu = IBV_MTU_4096; break;
			default   :
					fprintf(stderr," Invalid MTU - %d \n",user_mtu);
					fprintf(stderr," Please choose mtu from {256,512,1024,2048,4096}\n");
					//coverity[uninit_use_in_call]
					fprintf(stderr," Will run with the port active mtu - %d\n",port_attr.active_mtu);
					curr_mtu = port_attr.active_mtu;
		}

		if (curr_mtu > port_attr.active_mtu) {
			fprintf(stdout,"Requested mtu is higher than active mtu \n");
			fprintf(stdout,"Changing to active mtu - %d\n",port_attr.active_mtu);
			curr_mtu = port_attr.active_mtu;
		}
	}
	return curr_mtu;
}

/******************************************************************************
 * Set both link layers and return SUCCESS if both ports are active.
 * FAILURE is returned when requested port/link is not active/known except
 * when the link type is over-rode (--force-link="..."), in which case FAILURE
 * is returned only when the link(s) are not active.
 *
 * When --force-link is specified both ports are over-rode (ie no support for
 * forcing different link types on different ports).
 ******************************************************************************/
static int set_link_layer(struct ibv_context *context, struct perftest_parameters *params)
{
	struct ibv_port_attr port_attr;
	int8_t curr_link = params->link_type;

	if (ibv_query_port(context, params->ib_port, &port_attr)) {
		fprintf(stderr, " Unable to query port %d attributes\n", params->ib_port);
		return FAILURE;
	}

	if (curr_link == LINK_UNSPEC) {
		//coverity[uninit_use]
		params->link_type = port_attr.link_layer;
	}

	if (port_attr.state != IBV_PORT_ACTIVE) {
		fprintf(stderr, " Port number %d state is %s\n"
				,params->ib_port
				,portStates[port_attr.state]);
		return FAILURE;
	}

	if (strcmp("Unknown", link_layer_str(params->link_type)) == 0) {
		fprintf(stderr, "Link layer on port %d is Unknown\n", params->ib_port);
		return FAILURE;
	}

	if (params->dualport == ON) {
		curr_link = params->link_type2;
		if (ibv_query_port(context, params->ib_port2, &port_attr)) {
			fprintf(stderr, " Unable to query port %d attributes\n", params->ib_port2);
			return FAILURE;
		}

		if (curr_link == LINK_UNSPEC)
			params->link_type2 = port_attr.link_layer;

		if (port_attr.state != IBV_PORT_ACTIVE) {
			fprintf(stderr, " Port number %d state is %s\n"
				,params->ib_port2
				,portStates[port_attr.state]);
			return FAILURE;
		}

		if (strcmp("Unknown", link_layer_str(params->link_type2)) == 0) {
			fprintf(stderr, "Link layer on port %d is Unknown\n", params->ib_port2);
			return FAILURE;
		}
	}

	return SUCCESS;
}

static int get_device_max_reads_dc(struct ibv_context *context)
{
#ifdef HAVE_MLX5_DEVX
	uint32_t in[DEVX_ST_SZ_DW(query_hca_cap_in)] = {};
	uint32_t out[DEVX_ST_SZ_DW(query_hca_cap_out)] = {};
	uint16_t opmod = HCA_CAP_OPMOD_GET_CUR;
	int ret;

	DEVX_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	DEVX_SET(query_hca_cap_in, in, op_mod, opmod);

	ret = mlx5dv_devx_general_cmd(context, in, sizeof(in), out,
				      sizeof(out));
	if (!ret)
		return (1 << DEVX_GET(query_hca_cap_out, out,
				      cmd_hca_cap.log_max_ra_req_dc));
#endif
	return 0;
}

static int get_device_max_reads(struct ibv_context *context,
				struct perftest_parameters *user_param)
{
	struct ibv_device_attr attr;
	int max_reads = 0;

	if (user_param->connection_type == DC)
		max_reads = get_device_max_reads_dc(context);
	if (!max_reads && !ibv_query_device(context,&attr)) {
		//coverity[uninit_use]
		max_reads = attr.max_qp_rd_atom;
	}
	return max_reads;
}

/******************************************************************************
 *
 ******************************************************************************/
static int ctx_set_out_reads(struct ibv_context *context,
			     struct perftest_parameters *user_param)
{
	int max_reads = 0;
	int num_user_reads = user_param->out_reads;

	max_reads = get_device_max_reads(context, user_param);

	if (num_user_reads > max_reads) {
		printf(RESULT_LINE);
		fprintf(stderr," Number of outstanding reads is above max = %d\n",max_reads);
		fprintf(stderr," Changing to that max value\n");
		num_user_reads = max_reads;
	}
	else if (num_user_reads <= 0) {
		num_user_reads = max_reads;
	}

	return num_user_reads;
}

/******************************************************************************
 *
 ******************************************************************************/
static void ctx_set_max_inline(struct ibv_context *context,struct perftest_parameters *user_param)
{
	enum ctx_device current_dev = ib_dev_name(context);

	if (current_dev == UNKNOWN || current_dev == DEVICE_ERROR) {

		if (user_param->inline_size != DEF_INLINE) {
			printf(RESULT_LINE);
			fprintf(stderr,"Device not recognized to implement inline feature. Disabling it\n");
		}
		user_param->inline_size = 0;
		return;
	}

	if (user_param->inline_size == DEF_INLINE) {
		if (user_param->memory_type == MEMORY_CUDA){
			user_param->inline_size = 0;
			printf("Perftest doesn't supports CUDA tests with inline messages: inline size set to 0\n");
			return;
		}

		if (user_param->tst == LAT) {
			switch(user_param->verb) {
				case WRITE_IMM:
				case WRITE: user_param->inline_size = (user_param->connection_type == DC)? DEF_INLINE_DC : DEF_INLINE_WRITE; break;
				case SEND : user_param->inline_size = (user_param->connection_type == DC)? DEF_INLINE_DC : (user_param->connection_type == UD)? DEF_INLINE_SEND_UD :
					    DEF_INLINE_SEND_RC_UC_XRC ; break;
				default   : user_param->inline_size = 0;
			}
			if (current_dev == NETXTREME)
				user_param->inline_size = 96;
			else if (current_dev == EFA)
				user_param->inline_size = 32;
			else if (current_dev == QLOGIC_E4)
				user_param->inline_size = 128;
			else if (current_dev == ERDMA)
				user_param->inline_size = 96;
			else if (current_dev == HNS)
				user_param->inline_size = 32;
			else if (current_dev == INTEL_GEN1)
				user_param->inline_size = 48;
			else if (current_dev == INTEL_GEN2)
				user_param->inline_size = 101;

		} else {
			user_param->inline_size = 0;
		}
	}

	return;
}
/******************************************************************************
 *
 ******************************************************************************/
void set_raw_eth_parameters(struct perftest_parameters *user_param)
{

	if (user_param->is_new_raw_eth_param == 1 && user_param->is_old_raw_eth_param == 1) {
		printf(RESULT_LINE);
		fprintf(stderr," Invalid Command line.\nMix of source with local|remote and dest with local|remote is not supported.\n");
		fprintf(stderr,"For L2 tests you must enter local and remote mac  by this format --local_mac XX:XX:XX:XX:XX:XX --remote_mac XX:XX:XX:XX:XX:XX\n");
		fprintf(stderr,"For L3 tests You must add also local and remote ip  by this format --local_ip X.X.X.X --remote_ip X.X.X.X\n");
		fprintf(stderr,"For L4 you need to add also local and remote port  by this format --local_port XXXX  --remote_port XXXX\n");
		exit(1);
	}
	if (user_param->is_new_raw_eth_param) {
		int i;
		for (i = 0; i < MAC_ARR_LEN; i++)
		{
			user_param->source_mac[i] = user_param->local_mac[i];
			user_param->dest_mac[i] = user_param->remote_mac[i];
		}

		if (user_param->machine == SERVER) {
			user_param->server_ip = user_param->local_ip;
			user_param->client_ip = user_param->remote_ip;
			user_param->server_port = user_param->local_port;
			user_param->client_port = user_param->remote_port;
		} else if (user_param->machine == CLIENT) {
			user_param->server_ip = user_param->remote_ip;
			user_param->client_ip = user_param->local_ip;
			user_param->server_port = user_param->remote_port;
			user_param->client_port = user_param->local_port;
		}
	}
}
/******************************************************************************
 *
 ******************************************************************************/
int parser(struct perftest_parameters *user_param,char *argv[], int argc)
{
	int c,size_len;
	int size_factor = 1;
	static int run_inf_flag = 0;
	static int report_fmt_flag = 0;
	static int srq_flag = 0;
	static int use_null_mr_flag = 0;
	static int report_both_flag = 0;
	static int is_reversed_flag = 0;
	static int pkey_flag = 0;
	static int tcp_flag = 0;
	static int burst_size_flag = 0;
	static int typical_pkt_size_flag = 0;
	static int rate_limit_flag = 0;
	static int rate_units_flag = 0;
	static int rate_limit_type_flag = 0;
	static int verbosity_output_flag = 0;
	static int cpu_util_flag = 0;
	static int out_json_flag = 0;
	static int out_json_file_flag = 0;
	static int latency_gap_flag = 0;
	static int flow_label_flag = 0;
	static int retry_count_flag = 0;
	static int dont_xchg_versions_flag = 0;
	static int use_cuda_flag = 0;
	static int use_cuda_bus_id_flag = 0;
	static int use_cuda_dmabuf_flag = 0;
	static int use_rocm_flag = 0;
	static int use_neuron_flag = 0;
	static int use_neuron_dmabuf_flag = 0;
	static int use_hl_flag = 0;
	static int disable_pcir_flag = 0;
	static int mmap_file_flag = 0;
	static int mmap_offset_flag = 0;
	static int ipv6_flag = 0;
	static int ipv6_addr_flag = 0;
	static int raw_ipv6_flag = 0;
	static int report_per_port_flag = 0;
	static int odp_flag = 0;
	static int hugepages_flag = 0;
	static int old_post_send_flag = 0;
	static int use_promiscuous_flag = 0;
	static int use_sniffer_flag = 0;
	static int raw_mcast_flag = 0;
	static int mr_per_qp_flag = 0;
	static int dlid_flag = 0;
	static int tclass_flag = 0;
	static int wait_destroy_flag = 0;
	static int flows_flag = 0;
	static int flows_burst_flag = 0;
	static int force_link_flag = 0;
	static int source_ip_flag = 0;
	static int local_ip_flag = 0;
	static int remote_ip_flag = 0;
	static int local_port_flag = 0;
	static int remote_port_flag = 0;
	static int local_mac_flag = 0;
	static int remote_mac_flag = 0;
	static int reply_every_flag = 0;
	static int perform_warm_up_flag = 0;
	static int use_ooo_flag = 0;
	static int vlan_en = 0;
	static int vlan_pcp_flag = 0;
	static int recv_post_list_flag = 0;
	static int payload_flag = 0;
	static int use_write_with_imm_flag = 0;
	#ifdef HAVE_DCS
	static int log_dci_streams_flag = 0;
	static int log_active_dci_streams_flag = 0;
	static int log_active_dci_streams_flag_was_ever_set = 0;
	#endif
	#ifdef HAVE_AES_XTS
	static int aes_xts_flag = 0;
	static int encrypt_on_tx_flag = 0;
	static int sig_before_flag = 0;
	static int aes_block_size_flag = 0;
	static int data_enc_keys_number_flag = 0;
	static int kek_path_flag = 0;
	static int credentials_path_flag = 0;
	static int data_enc_key_app_path_flag = 0;
	#endif
	#ifdef HAVE_HNSDV
	static int congest_type_flag = 0;
	#endif

	char *server_ip = NULL;
	char *client_ip = NULL;
	char *local_ip = NULL;
	char *remote_ip = NULL;
	char *not_int_ptr = NULL;

	int *duplicates_checker = NULL;

	init_perftest_params(user_param);

	if(user_param->connection_type == RawEth)
		user_param->machine = UNCHOSEN;

	while (1) {
		int long_option_index = -1;
		static const struct option long_options[] = {
			{ .name = "port",		.has_arg = 1, .val = 'p' },
			{ .name = "ib-dev",		.has_arg = 1, .val = 'd' },
			{ .name = "ib-port",		.has_arg = 1, .val = 'i' },
			{ .name = "mtu",		.has_arg = 1, .val = 'm' },
			{ .name = "size",		.has_arg = 1, .val = 's' },
			{ .name = "iters",		.has_arg = 1, .val = 'n' },
			{ .name = "tx-depth",		.has_arg = 1, .val = 't' },
			{ .name = "qp-timeout",		.has_arg = 1, .val = 'u' },
			{ .name = "sl",			.has_arg = 1, .val = 'S' },
			{ .name = "gid-index",		.has_arg = 1, .val = 'x' },
			{ .name = "all",		.has_arg = 0, .val = 'a' },
			{ .name = "CPU-freq",		.has_arg = 0, .val = 'F' },
			{ .name = "connection",		.has_arg = 1, .val = 'c' },
			{ .name = "qp",			.has_arg = 1, .val = 'q' },
			{ .name = "events",		.has_arg = 0, .val = 'e' },
			{ .name = "vector",		.has_arg = 1, .val = 'X' },
			{ .name = "inline_size",	.has_arg = 1, .val = 'I' },
			{ .name = "outs",		.has_arg = 1, .val = 'o' },
			{ .name = "mcg",		.has_arg = 0, .val = 'g' },
			{ .name = "comm_rdma_cm",	.has_arg = 0, .val = 'z' },
			{ .name = "rdma_cm",		.has_arg = 0, .val = 'R' },
			{ .name = "tos",		.has_arg = 1, .val = 'T' },
			{ .name = "hop_limit",		.has_arg = 1, .val = 'L' },
			{ .name = "help",		.has_arg = 0, .val = 'h' },
			{ .name = "MGID",		.has_arg = 1, .val = 'M' },
			{ .name = "rx-depth",		.has_arg = 1, .val = 'r' },
			{ .name = "bidirectional",	.has_arg = 0, .val = 'b' },
			{ .name = "cq-mod",		.has_arg = 1, .val = 'Q' },
			{ .name = "noPeak",		.has_arg = 0, .val = 'N' },
			{ .name = "version",		.has_arg = 0, .val = 'V' },
			{ .name = "report-cycles",	.has_arg = 0, .val = 'C' },
			{ .name = "report-histogrm",	.has_arg = 0, .val = 'H' },
			{ .name = "report-unsorted",	.has_arg = 0, .val = 'U' },
			{ .name = "atomic_type",	.has_arg = 1, .val = 'A' },
			{ .name = "dualport",		.has_arg = 0, .val = 'O' },
			{ .name = "post_list",		.has_arg = 1, .val = 'l' },
			{ .name = "duration",		.has_arg = 1, .val = 'D' },
			{ .name = "margin",		.has_arg = 1, .val = 'f' },
			{ .name = "source_mac",		.has_arg = 1, .val = 'B' },
			{ .name = "dest_mac",		.has_arg = 1, .val = 'E' },
			{ .name = "dest_ip",		.has_arg = 1, .val = 'J' },
			{ .name = "source_ip",		.has_arg = 1, .val = 'j' },
			{ .name = "dest_port",		.has_arg = 1, .val = 'K' },
			{ .name = "source_port",	.has_arg = 1, .val = 'k' },
			{ .name = "ethertype",		.has_arg = 1, .val = 'Y' },
			{ .name = "limit_bw",		.has_arg = 1, .val = 'w' },
			{ .name = "limit_msgrate",	.has_arg = 1, .val = 'y' },
			{ .name = "server",		.has_arg = 0, .val = 'Z' },
			{ .name = "client",		.has_arg = 0, .val = 'P' },
			{ .name = "mac_fwd",		.has_arg = 0, .val = 'v' },
			{ .name = "use_rss",		.has_arg = 0, .val = 'G' },
			{ .name = "report-counters",	.has_arg = 1, .val = 'W' },
			{ .name = "force-link",		.has_arg = 1, .flag = &force_link_flag, .val = 1},
			{ .name = "remote_mac",		.has_arg = 1, .flag = &remote_mac_flag, .val = 1 },
			{ .name = "local_mac",		.has_arg = 1, .flag = &local_mac_flag, .val = 1 },
			{ .name = "remote_ip",		.has_arg = 1, .flag = &remote_ip_flag, .val = 1 },
			{ .name = "local_ip",		.has_arg = 1, .flag = &local_ip_flag, .val = 1 },
			{ .name = "remote_port",	.has_arg = 1, .flag = &remote_port_flag, .val = 1 },
			{ .name = "local_port",		.has_arg = 1, .flag = &local_port_flag, .val = 1 },
			{ .name = "run_infinitely",	.has_arg = 0, .flag = &run_inf_flag, .val = 1 },
			{ .name = "report_gbits",	.has_arg = 0, .flag = &report_fmt_flag, .val = 1},
			{ .name = "use-srq",		.has_arg = 0, .flag = &srq_flag, .val = 1},
			{ .name = "use-null-mr",	.has_arg = 0, .flag = &use_null_mr_flag, .val = 1},
			{ .name = "report-both",	.has_arg = 0, .flag = &report_both_flag, .val = 1},
			{ .name = "reversed",		.has_arg = 0, .flag = &is_reversed_flag, .val = 1},
			{ .name = "pkey_index",		.has_arg = 1, .flag = &pkey_flag, .val = 1},
			{ .name = "tcp",		.has_arg = 0, .flag = &tcp_flag, .val = 1},
			{ .name = "burst_size",		.has_arg = 1, .flag = &burst_size_flag, .val = 1},
			{ .name = "typical_pkt_size",	.has_arg = 1, .flag = &typical_pkt_size_flag, .val = 1},
			{ .name = "rate_limit",		.has_arg = 1, .flag = &rate_limit_flag, .val = 1},
			{ .name = "rate_limit_type",	.has_arg = 1, .flag = &rate_limit_type_flag, .val = 1},
			{ .name = "rate_units",		.has_arg = 1, .flag = &rate_units_flag, .val = 1},
			{ .name = "output",		.has_arg = 1, .flag = &verbosity_output_flag, .val = 1},
			{ .name = "cpu_util",		.has_arg = 0, .flag = &cpu_util_flag, .val = 1},
			{ .name = "out_json",		.has_arg = 0, .flag = &out_json_flag, .val = 1},
			{ .name = "out_json_file",	.has_arg = 1, .flag = &out_json_file_flag, .val = 1},
			{ .name = "latency_gap",	.has_arg = 1, .flag = &latency_gap_flag, .val = 1},
			{ .name = "flow_label",		.has_arg = 1, .flag = &flow_label_flag, .val = 1},
			{ .name = "retry_count",	.has_arg = 1, .flag = &retry_count_flag, .val = 1},
			{ .name = "dont_xchg_versions",	.has_arg = 0, .flag = &dont_xchg_versions_flag, .val = 1},
			{ .name = "payload_file_path",	.has_arg = 1, .flag = &payload_flag, .val = 1},
			{ .name = "use_cuda",		.has_arg = 1, .flag = &use_cuda_flag, .val = 1},
			{ .name = "use_cuda_bus_id",	.has_arg = 1, .flag = &use_cuda_bus_id_flag, .val = 1},
			{ .name = "use_cuda_dmabuf",	.has_arg = 0, .flag = &use_cuda_dmabuf_flag, .val = 1},
			{ .name = "use_rocm",		.has_arg = 1, .flag = &use_rocm_flag, .val = 1},
			{ .name = "use_neuron",		.has_arg = 1, .flag = &use_neuron_flag, .val = 1},
			{ .name = "use_neuron_dmabuf",	.has_arg = 0, .flag = &use_neuron_dmabuf_flag, .val = 1},
			{ .name = "use_hl",		.has_arg = 1, .flag = &use_hl_flag, .val = 1},
			{ .name = "mmap",		.has_arg = 1, .flag = &mmap_file_flag, .val = 1},
			{ .name = "mmap-offset",	.has_arg = 1, .flag = &mmap_offset_flag, .val = 1},
			{ .name = "ipv6",		.has_arg = 0, .flag = &ipv6_flag, .val = 1},
			{ .name = "ipv6-addr",		.has_arg = 0, .flag = &ipv6_addr_flag, .val = 1},
			#ifdef HAVE_IPV6
			{ .name = "raw_ipv6",		.has_arg = 0, .flag = &raw_ipv6_flag, .val = 1},
			#endif
			{.name = "report-per-port", .has_arg = 0, .flag = &report_per_port_flag, .val = 1},
			{.name = "odp", .has_arg = 0, .flag = &odp_flag, .val = 1},
			{.name = "use_hugepages", .has_arg = 0, .flag = &hugepages_flag, .val = 1},
			{.name = "use_old_post_send", .has_arg = 0, .flag = &old_post_send_flag, .val = 1},
			{.name = "promiscuous", .has_arg = 0, .flag = &use_promiscuous_flag, .val = 1},
			#if defined HAVE_SNIFFER
			{.name = "sniffer", .has_arg = 0, .flag = &use_sniffer_flag, .val = 1},
			#endif
			{.name = "raw_mcast", .has_arg = 0, .flag = &raw_mcast_flag, .val = 1},

			{.name = "mr_per_qp", .has_arg = 0, .flag = &mr_per_qp_flag, .val = 1},
			{.name = "dlid", .has_arg = 1, .flag = &dlid_flag, .val = 1},
			{.name = "tclass", .has_arg = 1, .flag = &tclass_flag, .val = 1},
			{.name = "wait_destroy", .has_arg = 1, .flag = &wait_destroy_flag, .val = 1},
			{.name = "flows", .has_arg = 1, .flag = &flows_flag, .val = 1},
			{.name = "flows_burst", .has_arg = 1, .flag = &flows_burst_flag, .val = 1},
			{.name = "reply_every", .has_arg = 1, .flag = &reply_every_flag, .val = 1},
			{.name = "perform_warm_up", .has_arg = 0, .flag = &perform_warm_up_flag, .val = 1},
			{.name = "vlan_en", .has_arg = 0, .flag = &vlan_en, .val = 1},
			{.name = "vlan_pcp", .has_arg = 1, .flag = &vlan_pcp_flag, .val = 1},
			{.name = "recv_post_list", .has_arg = 1, .flag = &recv_post_list_flag, .val = 1},
			#if defined HAVE_AES_XTS
			{.name = "aes_xts", .has_arg=0 , .flag = &aes_xts_flag, .val = 1},
			{.name = "encrypt_on_tx", .has_arg=0 , .flag = &encrypt_on_tx_flag, .val = 1},
			{.name = "sig_before", .has_arg=0 , .flag = &sig_before_flag, .val = 1},
			{.name = "aes_block_size", .has_arg=1 , .flag = &aes_block_size_flag, .val = 1},
			{.name = "kek_path", .has_arg=1 , .flag = &kek_path_flag, .val = 1},
			{.name = "credentials_path", .has_arg=1 , .flag = &credentials_path_flag, .val = 1},
			{.name = "data_enc_keys_number", .has_arg=1 , .flag = &data_enc_keys_number_flag, .val = 1},
			{.name = "data_enc_key_app_path", .has_arg=1 , .flag = &data_enc_key_app_path_flag, .val = 1},
			#endif
			#if defined HAVE_DCS
			{.name = "log_dci_streams", .has_arg = 1, .flag = &log_dci_streams_flag, .val = 1},
			{.name = "log_active_dci_streams", .has_arg = 1, .flag = &log_active_dci_streams_flag, .val = 1},
			#endif
			#if defined HAVE_RO
			{.name = "disable_pcie_relaxed", .has_arg = 0, .flag = &disable_pcir_flag, .val = 1 },
			#endif
			#if defined HAVE_OOO_ATTR
			{.name = "use_ooo", .has_arg = 0, .flag = &use_ooo_flag, .val = 1},
			#endif
			#ifdef HAVE_HNSDV
			{ .name = "congest_type", .has_arg = 1, .flag = &congest_type_flag, .val = 1},
			#endif
			{.name = "bind_source_ip", .has_arg = 1, .flag = &source_ip_flag, .val = 1},
			{.name = "write_with_imm", .has_arg = 0, .flag = &use_write_with_imm_flag, .val = 1 },
			{0}
		};
		if (!duplicates_checker) {
			int size_long_options = sizeof(long_options)/ sizeof(struct option);
			ALLOCATE(duplicates_checker, int, size_long_options);
			memset(duplicates_checker, 0, size_long_options * sizeof(int));
		}
		c = getopt_long(argc,argv,"w:y:p:d:i:m:s:n:t:u:S:x:c:q:I:o:M:r:Q:A:l:D:f:B:T:L:E:J:j:K:k:X:W:aFegzRvhbNVCHUOZP",long_options, &long_option_index);

		/* c == 0: the argumenet is a long option (example: --report_gbits) */
		/* c > 0: the argument is a short option (example: -s/--size) */
		if (c == 0) {
			if (duplicates_checker[long_option_index]) {
				fprintf(stderr," Duplicated argument: %s \n", long_options[long_option_index].name);
				free(duplicates_checker);
				return FAILURE;
			}
			duplicates_checker[long_option_index]++;
		} else if(c > 0) {
			/* first 52 elements of 'duplicates_checker' are saved for short options (A-Z, a-z) */
			int char_index = (c >= 'A' && c <= 'Z') ? c - 'A' : c - ('A' + 6);
			if (duplicates_checker[char_index] && c != '?') {
				fprintf(stderr," Duplicated argument: -%c \n", c);
				free(duplicates_checker);
				return FAILURE;
			}
			duplicates_checker[char_index]++;
		}

		if (c == -1)
			break;

		switch (c) {
			case 'p': CHECK_VALUE(user_param->port,int,"Port",not_int_ptr); break;
			case 'd': GET_STRING(user_param->ib_devname,strdupa(optarg)); break;
			case 'i': CHECK_VALUE(user_param->ib_port,uint8_t,"IB Port",not_int_ptr);
				  if (user_param->ib_port < MIN_IB_PORT) {
					  fprintf(stderr, "IB Port can't be less than %d\n", MIN_IB_PORT);
					  free(duplicates_checker);
					  return FAILURE;
				  }
				  break;
			case 'm': CHECK_VALUE(user_param->mtu,int,"MTU",not_int_ptr); break;
			case 'n': CHECK_VALUE_IN_RANGE_UNS(user_param->iters,uint64_t,MIN_ITER,MAX_ITER,"Iteration num",not_int_ptr); break;
			case 't': CHECK_VALUE_IN_RANGE(user_param->tx_depth,int,MIN_TX,MAX_TX,"Tx depth",not_int_ptr); break;
			case 'T': CHECK_VALUE_IN_RANGE(user_param->tos,int,MIN_TOS,MAX_TOS,"TOS",not_int_ptr); break;
			case 'L': CHECK_VALUE_IN_RANGE(user_param->hop_limit,int,MIN_HOP_LIMIT,MAX_HOP_LIMIT,"Hop Limit",not_int_ptr); break;
			case 'u': CHECK_VALUE(user_param->qp_timeout,uint8_t,"QP Timeout",not_int_ptr); break;
			case 'S': CHECK_VALUE(user_param->sl,uint8_t,"Service Level",not_int_ptr);
				  if (user_param->sl > MAX_SL) {
					  fprintf(stderr," Only %d Service levels\n",MAX_SL);
					  free(duplicates_checker);
					  return FAILURE;
				  }
				  if (user_param->connection_type == RawEth)
					  user_param->raw_qos = 1;
				  break;
			case 'x': CHECK_VALUE_IN_RANGE_UNS(user_param->gid_index,uint8_t,MIN_GID_IX,MAX_GID_IX,"Gid index",not_int_ptr);
				  user_param->use_gid_user = 1; break;
			case 'c': change_conn_type(&user_param->connection_type,user_param->verb,optarg); break;
			case 'q': if (user_param->tst != BW) {
					fprintf(stderr," Multiple QPs only available on bw tests\n");
					free(duplicates_checker);
					return FAILURE;
				  }
				  CHECK_VALUE_IN_RANGE(user_param->num_of_qps,int,MIN_QP_NUM,MAX_QP_NUM,"num of Qps",not_int_ptr);
				  break;
			case 'I': CHECK_VALUE_IN_RANGE(user_param->inline_size,int,0,MAX_INLINE,"Max inline",not_int_ptr);
				  if (user_param->verb == READ || user_param->verb ==ATOMIC) {
					  fprintf(stderr," Inline feature not available on READ/Atomic verbs\n");
					  free(duplicates_checker);
					  return FAILURE;
				  }
				  break;
			case 'o': CHECK_VALUE(user_param->out_reads,int,"Outstanding Reads",not_int_ptr);
				  if (user_param->verb != READ && user_param->verb != ATOMIC) {
					  fprintf(stderr," Setting Outstanding reads only available on READ verb\n");
					  free(duplicates_checker);
					  return FAILURE;
				  }
				  break;
			case 'M': GET_STRING(user_param->user_mgid,strdupa(optarg)); break;
			case 'r': CHECK_VALUE_IN_RANGE(user_param->rx_depth,int,MIN_RX,MAX_RX," Rx depth",not_int_ptr);
				  if (user_param->verb != SEND && user_param->verb != WRITE && user_param->verb != WRITE_IMM && user_param->rx_depth > DEF_RX_RDMA) {
					  fprintf(stderr," On RDMA verbs rx depth can be only 1\n");
					  free(duplicates_checker);
					  return FAILURE;
				  }
				  break;
			case 'Q': CHECK_VALUE_IN_RANGE(user_param->cq_mod,int,MIN_CQ_MOD,MAX_CQ_MOD,"CQ moderation",not_int_ptr);
				  user_param->req_cq_mod = 1;
				  break;
			case 'A':
				  if (user_param->verb != ATOMIC) {
					  fprintf(stderr," You are not running the atomic_lat/bw test!\n");
					  fprintf(stderr," To change the atomic action type, you must run one of the atomic tests\n");
					  free(duplicates_checker);
					  return FAILURE;
				  }

				  if (strcmp(atomicTypesStr[0],optarg)==0)
					  user_param->atomicType = CMP_AND_SWAP;

				  else if (strcmp(atomicTypesStr[1],optarg)==0)
					  user_param->atomicType = FETCH_AND_ADD;

				  else {
					  fprintf(stderr," Invalid Atomic type! please choose from {CMP_AND_SWAP,FETCH_AND_ADD}\n");
					  free(duplicates_checker);
					  exit(1);
				  }
				  break;
			case 'l': CHECK_VALUE(user_param->post_list,int,"Send Post List size",not_int_ptr); break;
			case 'D': CHECK_VALUE_POSITIVE(user_param->duration,int,"Duration period",not_int_ptr);
				  user_param->test_type = DURATION;
				  break;
			case 'f': CHECK_VALUE_NON_NEGATIVE(user_param->margin,int,"Margin",not_int_ptr); break;
			case 'O':
				  user_param->ib_port  = DEF_IB_PORT;
				  user_param->ib_port2 = DEF_IB_PORT2;
				  user_param->dualport = ON;
				  break;
			case 'a': user_param->test_method = RUN_ALL; break;
			case 'F': user_param->cpu_freq_f = ON; break;
			case 'V': printf("Version: %s\n",user_param->version); return VERSION_EXIT;
			case 'h': usage(argv[0], user_param->verb, user_param->tst, user_param->connection_type);
				  if(user_param->connection_type == RawEth) {
					  usage_raw_ethernet(user_param->tst);
				  }
				  return HELP_EXIT;
			case 'z': user_param->use_rdma_cm = ON; break;
			case 'R': user_param->work_rdma_cm = ON; break;
			case 's': size_len = (int)strlen(optarg);
				  if (optarg[size_len-1] == 'K') {
					  optarg[size_len-1] = '\0';
					  size_factor = 1024;
				  }
				  if (optarg[size_len-1] == 'M') {
					  optarg[size_len-1] = '\0';
					  size_factor = 1024*1024;
				  }
				  user_param->size = (uint64_t)strtol(optarg, NULL, 0) * size_factor;
				  user_param->req_size = 1;
				  if (user_param->size < 1 || user_param->size > (UINT_MAX / 2)) {
					  fprintf(stderr," Message Size should be between %d and %d\n",1,UINT_MAX/2);
					  free(duplicates_checker);
					  return FAILURE;
				  }
				  break;
			case 'e': user_param->use_event = ON;
				  if (user_param->verb == WRITE || user_param->verb == WRITE_IMM) {
					  fprintf(stderr," Events feature not available on WRITE verb\n");
					  free(duplicates_checker);
					  return FAILURE;
				  }
				  break;
			case 'X':
				  if (user_param->verb == WRITE || user_param->verb == WRITE_IMM) {
					  fprintf(stderr, " Events feature not available on WRITE verb\n");
					  free(duplicates_checker);
					  return FAILURE;
				  }
				  user_param->use_eq_num = ON;
				  CHECK_VALUE_IN_RANGE(user_param->eq_num, int, MIN_EQ_NUM, MAX_EQ_NUM, "EQN", not_int_ptr);
				  break;
			case 'b': user_param->duplex = ON;
				  if (user_param->tst == LAT) {
					  fprintf(stderr," Bidirectional is only available in BW test\n");
					  free(duplicates_checker);
					  return FAILURE;
				  } break;
			case 'N': user_param->noPeak = ON;
				  if (user_param->tst == LAT) {
					  fprintf(stderr," NoPeak only valid for BW tests\n");
					  free(duplicates_checker);
					  return FAILURE;
				  } break;
			case 'C':
				  if (user_param->tst != LAT) {
					  fprintf(stderr," Available only on Latency tests\n");
					  free(duplicates_checker);
					  return FAILURE;
				  }
				  user_param->r_flag->cycles = ON;
				  break;
			case 'g': user_param->use_mcg = ON;
				  if (user_param->verb != SEND) {
					  fprintf(stderr," MultiCast feature only available on SEND verb\n");
					  free(duplicates_checker);
					  return FAILURE;
				  } break;
			case 'H':
				  if (user_param->tst == BW) {
					  fprintf(stderr," Available only on Latency tests\n");
					  free(duplicates_checker);
					  return FAILURE;
				  }
				  user_param->r_flag->histogram = ON;
				  break;
			case 'U':
				  if (user_param->tst == BW) {
					fprintf(stderr," is Available only on Latency tests\n");
					free(duplicates_checker);
					  return FAILURE;
				  }
				  user_param->r_flag->unsorted = ON;
				  break;
			case 'B':
				  user_param->is_old_raw_eth_param = 1;
				  user_param->is_source_mac = ON;
				  if(parse_mac_from_str(optarg, user_param->source_mac)) {
					free(duplicates_checker);
					return FAILURE;
				  } break;
			case 'E':
				  user_param->is_old_raw_eth_param = 1;
				  user_param->is_dest_mac = ON;
				  if(parse_mac_from_str(optarg, user_param->dest_mac)) {
					free(duplicates_checker);
					return FAILURE;
				  } break;
			case 'J':
				  user_param->is_old_raw_eth_param = 1;
				  user_param->is_server_ip = ON;
				  server_ip = optarg;
				  break;
			case 'j':
				  user_param->is_old_raw_eth_param = 1;
				  user_param->is_client_ip = ON;
				  client_ip = optarg;
				  break;
			case 'K':
				  user_param->is_old_raw_eth_param = 1;
				  user_param->is_server_port = ON;
				  CHECK_VALUE(user_param->server_port,int,"Server Port",not_int_ptr);
				  if(OFF == check_if_valid_udp_port(user_param->server_port)) {
					  fprintf(stderr," Invalid server UDP port\n");
					  free(duplicates_checker);
					  return FAILURE;
				  }
				  break;
			case 'k':
				  user_param->is_old_raw_eth_param = 1;
				  user_param->is_client_port = ON;
				  CHECK_VALUE(user_param->client_port,int,"Client Port",not_int_ptr);
				  if(OFF == check_if_valid_udp_port(user_param->client_port)) {
					  fprintf(stderr," Invalid client UDP port\n");
					  free(duplicates_checker);
					  return FAILURE;
				  }
				  break;
			case 'Y':
				  user_param->is_ethertype = ON;
				  if (parse_ethertype_from_str(optarg, &user_param->ethertype)) {
					  fprintf(stderr, " Invalid ethertype value\n");
					  free(duplicates_checker);
					  return FAILURE;
				  }
				  break;
			case 'w':
				  user_param->is_limit_bw = ON;
				  user_param->limit_bw = strtof(optarg,NULL);
				  if (user_param->limit_bw < 0) {
					  fprintf(stderr, " Invalid Minimum BW Limit\n");
					  free(duplicates_checker);
					  return FAILURE;
				  }
				  break;
			case 'y':
				  user_param->is_limit_msgrate = ON;
				  user_param->limit_msgrate = strtof(optarg,NULL);
				  if (user_param->limit_msgrate < 0) {
					  fprintf(stderr, " Invalid Minimum msgRate Limit\n");
					  free(duplicates_checker);
					  return FAILURE;
				  }
				  break;
			case 'W':
				  if (counters_alloc(optarg, &user_param->counter_ctx)) {
					  fprintf(stderr, "Failed to parse the performance counter list\n");
					  free(duplicates_checker);
					  return FAILURE;
				  }
				  break;
			case 'P': user_param->machine = CLIENT; break;
			case 'Z': user_param->machine = SERVER; break;
			case 'v': user_param->mac_fwd = ON; break;
			case 'G':
				fprintf(stderr, "RSS isn't supported\n");
				free(duplicates_checker);
				return FAILURE;
			case 0: /* required for long options to work. */
				#ifdef HAVE_HNSDV
				if (congest_type_flag) {
					set_congest_type(&user_param->congest_type, optarg);
					congest_type_flag = 0;
				}
				#endif
				if (pkey_flag) {
					CHECK_VALUE(user_param->pkey_index,int,"Pkey index",not_int_ptr);
					pkey_flag = 0;
				}
				if (rate_limit_flag) {
					GET_STRING(user_param->rate_limit_str ,strdupa(optarg));
					user_param->rate_limit = atof(optarg);
					if (user_param->rate_limit <= 0) {
						fprintf(stderr, " Rate limit must be non-negative floating point number\n");
						free(duplicates_checker);
						return FAILURE;
					}
					/* if not specified, choose HW rate limiter as default */
					if (user_param->rate_limit_type == DISABLE_RATE_LIMIT)
						user_param->rate_limit_type = HW_RATE_LIMIT;

					rate_limit_flag = 0;
				}
				if (burst_size_flag) {
					CHECK_VALUE_NON_NEGATIVE(user_param->burst_size,int,"Burst size",not_int_ptr);
					burst_size_flag = 0;
				}
				if (typical_pkt_size_flag) {
					CHECK_VALUE_IN_RANGE(user_param->typical_pkt_size,int,0,0xFFFF,"Typical pkt size",not_int_ptr);
					typical_pkt_size_flag = 0;
				}
				if (rate_units_flag) {
					if (strcmp("M",optarg) == 0) {
						user_param->rate_units = MEGA_BYTE_PS;
					} else if (strcmp("g",optarg) == 0) {
						user_param->rate_units = GIGA_BIT_PS;
					} else if (strcmp("p",optarg) == 0) {
						user_param->rate_units = PACKET_PS;
					} else {
						fprintf(stderr, " Invalid rate limit units. Please use M,g or p\n");
						free(duplicates_checker);
						return FAILURE;
					}
					rate_units_flag = 0;
				}
				if (rate_limit_type_flag) {
					user_param->is_rate_limit_type = 1;
					if(strcmp("SW",optarg) == 0)
						user_param->rate_limit_type = SW_RATE_LIMIT;
					else if(strcmp("HW",optarg) == 0)
						user_param->rate_limit_type = HW_RATE_LIMIT;
					else if(strcmp("PP",optarg) == 0)
						user_param->rate_limit_type = PP_RATE_LIMIT;
					else {
						fprintf(stderr, " Invalid rate limit type flag. Please use HW, SW or PP.\n");
						free(duplicates_checker);
						return FAILURE;
					}
					rate_limit_type_flag = 0;
				}
				if (verbosity_output_flag) {
					if (strcmp("bandwidth",optarg) == 0) {
						user_param->output = OUTPUT_BW;
					} else if (strcmp("message_rate",optarg) == 0) {
						user_param->output = OUTPUT_MR;
					} else if (strcmp("latency",optarg) == 0) {
						user_param->output = OUTPUT_LAT;
					} else {
						fprintf(stderr, " Invalid verbosity level output flag. Please use bandwidth, latency, message_rate\n");
						free(duplicates_checker);
						return FAILURE;
					}
					verbosity_output_flag = 0;
				}
				if (latency_gap_flag) {
					CHECK_VALUE_NON_NEGATIVE(user_param->latency_gap,int,"Latency gap time",not_int_ptr);
					latency_gap_flag = 0;
				}
				/* We statically define memory type options so check if requested option is actually supported. */
				if (((use_cuda_flag || use_cuda_bus_id_flag) && !cuda_memory_supported()) ||
				    (use_cuda_dmabuf_flag && !cuda_memory_dmabuf_supported()) ||
				    (use_rocm_flag && !rocm_memory_supported()) ||
				    (use_neuron_flag && !neuron_memory_supported()) ||
				    (use_neuron_dmabuf_flag && !neuron_memory_dmabuf_supported()) ||
				    (use_hl_flag && !hl_memory_supported())) {
					printf(" Unsupported memory type\n");
					return FAILURE;
				}
				/* Memory types are mutually exclucive, make sure we were not already asked to use a different memory type. */
				if (user_param->memory_type != MEMORY_HOST &&
				    (mmap_file_flag || use_rocm_flag || use_neuron_flag || use_hl_flag ||
				     ((use_cuda_flag || use_cuda_bus_id_flag) && user_param->memory_type != MEMORY_CUDA))) {
					fprintf(stderr, " Can't use multiple memory types\n");
					return FAILURE;
				}
				if (use_cuda_flag) {
					CHECK_VALUE_NON_NEGATIVE(user_param->cuda_device_id,int,"CUDA device",not_int_ptr);
					user_param->memory_type = MEMORY_CUDA;
					user_param->memory_create = cuda_memory_create;
					use_cuda_flag = 0;
				}
				if (use_cuda_bus_id_flag) {
					user_param->cuda_device_bus_id = strdup(optarg);
					printf("Got PCIe address of: %s\n", user_param->cuda_device_bus_id);
					user_param->memory_type = MEMORY_CUDA;
					user_param->memory_create = cuda_memory_create;
					use_cuda_bus_id_flag = 0;
				}
				if (use_cuda_dmabuf_flag) {
					user_param->use_cuda_dmabuf = 1;
					if (user_param->memory_type != MEMORY_CUDA) {
						fprintf(stderr, "CUDA DMA-BUF cannot be used without CUDA\n");
						free(duplicates_checker);
						return FAILURE;
					}
					use_cuda_dmabuf_flag = 0;
				}
				if (use_rocm_flag) {
					CHECK_VALUE_NON_NEGATIVE(user_param->rocm_device_id,int,"ROCm device",not_int_ptr);
					user_param->memory_type = MEMORY_ROCM;
					user_param->memory_create = rocm_memory_create;
					use_rocm_flag = 0;
				}
				if (use_neuron_flag) {
					user_param->neuron_core_id = strtol(optarg, NULL, 0);
					if (user_param->neuron_core_id < 0) {
						fprintf(stderr, "Invalid Neuron Core ID %d\n", user_param->neuron_core_id);
						return FAILURE;
					}
					user_param->memory_type = MEMORY_NEURON;
					user_param->memory_create = neuron_memory_create;
					use_neuron_flag = 0;
				}
				if (use_neuron_dmabuf_flag) {
					user_param->use_neuron_dmabuf = 1;
					if (user_param->memory_type != MEMORY_NEURON) {
						fprintf(stderr, "Neuron DMA-BUF cannot be used without Neuron device\n");
						free(duplicates_checker);
						return FAILURE;
					}
					use_neuron_dmabuf_flag = 0;
				}
				if (use_hl_flag) {
					user_param->hl_device_bus_id = strdup(optarg);
					user_param->memory_type = MEMORY_HL;
					user_param->memory_create = hl_memory_create;
					use_hl_flag = 0;
				}
				if (flow_label_flag) {
					CHECK_VALUE(user_param->flow_label,int,"flow label",not_int_ptr);
					if (user_param->connection_type == RawEth && user_param->flow_label < 0) {
						fprintf(stderr," flow label must be non-negative for RawEth\n");
						return FAILURE;
					}
					flow_label_flag = 0;
				}
				if (retry_count_flag) {
					CHECK_VALUE_NON_NEGATIVE(user_param->retry_count,int,"Retry Count",not_int_ptr);
					retry_count_flag = 0;
				}
				if (mmap_file_flag) {
					user_param->mmap_file = strdup(optarg);
					user_param->memory_type = MEMORY_MMAP;
					user_param->memory_create = mmap_memory_create;
					mmap_file_flag = 0;
				}
				if (out_json_file_flag) {
					user_param->out_json_file_name = strdup(optarg);
					out_json_file_flag = 0;
				}
				if (mmap_offset_flag) {
					CHECK_VALUE(user_param->mmap_offset,unsigned long,"mmap offset",not_int_ptr);
					mmap_offset_flag = 0;
				}
				if (dlid_flag) {
					CHECK_VALUE(user_param->dlid,uint16_t,"dlid",not_int_ptr);
					dlid_flag = 0;
				}
				if (tclass_flag) {
					CHECK_VALUE(user_param->traffic_class,uint16_t,"traffic class",not_int_ptr);
					tclass_flag = 0;
				}
				if (wait_destroy_flag) {
					CHECK_VALUE(user_param->wait_destroy,uint32_t,"wait_destroy",not_int_ptr);
					wait_destroy_flag = 0;
				}
				if (flows_flag) {
					CHECK_VALUE(user_param->flows,uint16_t,"flows",not_int_ptr);
					if (user_param->flows == 0) {
						fprintf(stderr, "Invalid flows value. Please set a positive number\n");
						free(duplicates_checker);
						return FAILURE;
					}
					flows_flag = 0;
				}
				if (flows_burst_flag) {
					CHECK_VALUE(user_param->flows_burst,uint16_t,"flows burst",not_int_ptr);
					if (user_param->flows_burst == 0) {
						fprintf(stderr, "Invalid burst flow value. Please set a positive number\n");
						free(duplicates_checker);
						return FAILURE;
					}
					flows_burst_flag = 0;
				}
				if (force_link_flag) {
					user_param->link_type = str_link_layer(optarg);
					if (user_param->link_type == LINK_FAILURE) {
						fprintf(stderr, "Invalid link layer value should be IB or ETHERNET.\n");
						free(duplicates_checker);
						return FAILURE;
					}
					force_link_flag = 0;
				}
				if (remote_mac_flag) {
					user_param->is_new_raw_eth_param = 1;
					user_param->is_dest_mac = 1;
					if(parse_mac_from_str(optarg, user_param->remote_mac))
					{
						free(duplicates_checker);
						return FAILURE;
					}
					remote_mac_flag = 0;
				}
				if (local_mac_flag) {
					user_param->is_new_raw_eth_param = 1;
					user_param->is_source_mac = 1;
					if(parse_mac_from_str(optarg, user_param->local_mac)) {
						free(duplicates_checker);
						return FAILURE;
					}
					local_mac_flag = 0;
				}
				if (remote_ip_flag) {
					user_param->is_new_raw_eth_param = 1;
					user_param->is_client_ip = 1;
					remote_ip = optarg;
					remote_ip_flag = 0;
				}
				if (local_ip_flag) {
					user_param->is_new_raw_eth_param = 1;
					user_param->is_server_ip = 1;
					local_ip = optarg;
					local_ip_flag = 0;
				}
				if (source_ip_flag) {
					user_param->has_source_ip = 1;
					GET_STRING(user_param->source_ip, strdupa(optarg));
					source_ip_flag = 0;
				}
				if (payload_flag){
					user_param->has_payload_modification = 1;
					GET_STRING(user_param->payload_file_path, strdupa(optarg));
					payload_flag = 0;
				}
				if (remote_port_flag) {
					user_param->is_new_raw_eth_param = 1;
					user_param->is_client_port = 1;
					CHECK_VALUE(user_param->remote_port,int,"remote port",not_int_ptr);
					if(OFF == check_if_valid_udp_port(user_param->remote_port)) {
						fprintf(stderr," Invalid remote UDP port\n");
						free(duplicates_checker);
						return FAILURE;
					}
					remote_port_flag = 0;
				}
				if (local_port_flag) {
					user_param->is_new_raw_eth_param = 1;
					user_param->is_server_port = 1;
					CHECK_VALUE(user_param->local_port,int,"local port",not_int_ptr);
					if(OFF == check_if_valid_udp_port(user_param->local_port)) {
						fprintf(stderr," Invalid local UDP port\n");
						free(duplicates_checker);
						return FAILURE;
					}
					local_port_flag = 0;
				}
				if (reply_every_flag) {
					CHECK_VALUE(user_param->reply_every,uint32_t,"reply_every",not_int_ptr);
					reply_every_flag = 0;
				}
				if (vlan_pcp_flag) {
					// cppcheck-suppress unsignedLessThanZero
					CHECK_VALUE_IN_RANGE_UNS(user_param->vlan_pcp,uint32_t,0,8,"VLAN PCP",not_int_ptr);
					user_param->vlan_en = ON;
					vlan_pcp_flag = 0;
				}
				if (recv_post_list_flag) {
					CHECK_VALUE(user_param->recv_post_list,int,"Receive Post List size",not_int_ptr);
					recv_post_list_flag = 0;
				}
				#ifdef HAVE_AES_XTS
				if (aes_xts_flag) {
					user_param->aes_xts = 1;
				}
				if (encrypt_on_tx_flag) {
					user_param->encrypt_on_tx = 1;
				}
				if (sig_before_flag) {
					user_param->sig_before = 1;
				}
				if (aes_block_size_flag) {
					CHECK_VALUE(user_param->aes_block_size,uint16_t,"aes_block_size",not_int_ptr);
					aes_block_size_flag = 0;
				}
				if (data_enc_keys_number_flag) {
					CHECK_VALUE(user_param->data_enc_keys_number,uint16_t,"data_enc_keys_number",not_int_ptr);
					data_enc_keys_number_flag = 0;
				}
				if (kek_path_flag) {
					GET_STRING(user_param->kek_path, strdupa(optarg));
					kek_path_flag = 0;
				}
				if (data_enc_key_app_path_flag) {
					GET_STRING(user_param->data_enc_key_app_path, strdupa(optarg));
					data_enc_key_app_path_flag = 0;
				}
				if (credentials_path_flag) {
					GET_STRING(user_param->credentials_path, strdupa(optarg));
					credentials_path_flag = 0;
				}
				#endif
				#ifdef HAVE_DCS
				if (log_dci_streams_flag) {
					CHECK_VALUE(user_param->log_dci_streams,uint16_t,"log_dci_streams",not_int_ptr);
					log_dci_streams_flag = 0;
				}
				if (log_active_dci_streams_flag) {
					CHECK_VALUE(user_param->log_active_dci_streams,uint16_t,"log_active_dci_streams",not_int_ptr);
					log_active_dci_streams_flag = 0;
					log_active_dci_streams_flag_was_ever_set = 1;
				}
				#endif
				if (use_write_with_imm_flag) {
					if ((user_param->tst != LAT && user_param->tst != BW) || user_param->verb != WRITE) {
						fprintf(stderr, "Write_with_imm can only be used with write_lat and write_bw tests\n");
						return FAILURE;
					}
					user_param->verb = WRITE_IMM;
					use_write_with_imm_flag = 0;
				}
				break;
			default:
				  fprintf(stderr," Invalid Command or flag.\n");
				  fprintf(stderr," Please check command line and run again.\n\n");
				  usage(argv[0], user_param->verb, user_param->tst, user_param->connection_type);
				  if(user_param->connection_type == RawEth) {
					  usage_raw_ethernet(user_param->tst);
				  }
				  free(duplicates_checker);
				  return FAILURE;
		}

	}
	free(duplicates_checker);

#ifdef HAVE_DCS
	if (!log_active_dci_streams_flag_was_ever_set) {
		user_param->log_active_dci_streams = user_param->log_dci_streams;
	}
#endif
	if (tcp_flag) {
		user_param->tcp = 1;
	}
	if (run_inf_flag) {
		user_param->test_method = RUN_INFINITELY;
	}

	if (srq_flag) {
		user_param->use_srq = 1;
	}

	if (use_null_mr_flag) {
		user_param->use_null_mr = 1;
	}

	if (report_fmt_flag) {
		user_param->report_fmt = GBS;
	}

	if (dont_xchg_versions_flag) {
		user_param->dont_xchg_versions = 1;
	}

	if (disable_pcir_flag) {
		user_param->disable_pcir = 1;
	}

	if (report_both_flag) {
		user_param->report_both = 1;
	}

	if (is_reversed_flag) {
		user_param->is_reversed = 1;
	}

	if (cpu_util_flag) {
		user_param->cpu_util = 1;
	}

	if (out_json_flag) {
		user_param->out_json = 1;
	}

	if (report_per_port_flag) {
		user_param->report_per_port = 1;
	}

	if (ipv6_flag) {
		user_param->ipv6 = 1;
	}

	if (ipv6_addr_flag) {
		user_param->ai_family = AF_INET6;
	}

	if (raw_ipv6_flag) {
		if (user_param->is_new_raw_eth_param) {
			if (user_param->is_server_ip) {
				if(1 != parse_ip6_from_str(local_ip,
							  (struct in6_addr *)&(user_param->local_ip6))) {
					fprintf(stderr," Invalid local IP address\n");
					return FAILURE;
				}
			}
			if (user_param->is_client_ip) {
				if(1 != parse_ip6_from_str(remote_ip,
							  (struct in6_addr *)&(user_param->remote_ip6))) {
					fprintf(stderr," Invalid remote IP address\n");
					return FAILURE;
				}
			}
		} else {
			if (user_param->is_server_ip) {
				if(1 != parse_ip6_from_str(server_ip,
							  (struct in6_addr *)&(user_param->server_ip6))) {
					fprintf(stderr," Invalid server IP address\n");
					return FAILURE;
				}
			}
			if (user_param->is_client_ip) {
				if(1 != parse_ip6_from_str(client_ip,
							  (struct in6_addr *)&(user_param->client_ip6))) {
					fprintf(stderr," Invalid client IP address\n");
					return FAILURE;
				}
			}
		}
		user_param->raw_ipv6 = 1;
	} else {
		if (user_param->is_new_raw_eth_param) {
			if (user_param->is_server_ip) {
				if(1 != parse_ip_from_str(local_ip,
							  &(user_param->local_ip))) {
					fprintf(stderr," Invalid local IP address\n");
					return FAILURE;
				}
			}
			if (user_param->is_client_ip) {
				if(1 != parse_ip_from_str(remote_ip,
							  &(user_param->remote_ip))) {
					fprintf(stderr," Invalid remote IP address\n");
					return FAILURE;
				}
			}
		} else {
			if (user_param->is_server_ip) {
				if(1 != parse_ip_from_str(server_ip,
							  &(user_param->server_ip))) {
					fprintf(stderr," Invalid server IP address\n");
					return FAILURE;
				}
			}
			if (user_param->is_client_ip) {
				if(1 != parse_ip_from_str(client_ip,
							  &(user_param->client_ip))) {
					fprintf(stderr," Invalid client IP address\n");
					return FAILURE;
				}
			}
		}
	}

	if(odp_flag) {
		user_param->use_odp = 1;
	}

	if(hugepages_flag) {
		user_param->use_hugepages = 1;
	}

	if(old_post_send_flag) {
		user_param->use_old_post_send = 1;
	}

	if (use_promiscuous_flag) {
		user_param->use_promiscuous = 1;
	}

	if (use_sniffer_flag) {
		user_param->use_sniffer = 1;
	}

	if (raw_mcast_flag) {
		user_param->raw_mcast = 1;
	}

	if (mr_per_qp_flag) {
		user_param->mr_per_qp = 1;
	}

	if (perform_warm_up_flag) {
		user_param->perform_warm_up = 1;
	}
	if (use_ooo_flag)
		user_param->use_ooo = 1;
	if(vlan_en) {
		user_param->vlan_en = ON;
		user_param->print_eth_func = &print_ethernet_vlan_header;
		vlan_en = 0;
	}
	if (optind == argc - 1) {
		GET_STRING(user_param->servername,strdupa(argv[optind]));

	} else if (optind < argc) {
		fprintf(stderr," Invalid Command line. Please check command rerun \n");
		return 1;
	}

	if(user_param->connection_type != RawEth)
		user_param->machine = user_param->servername ? CLIENT : SERVER;

	/* fan-in addition */
	if (user_param->is_reversed) {
		if (user_param->machine == SERVER)
			user_param->machine = CLIENT;
		else
			user_param->machine = SERVER;
	}
	set_raw_eth_parameters(user_param);
	force_dependecies(user_param);
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
int check_link_and_mtu(struct ibv_context *context,struct perftest_parameters *user_param)
{
	user_param->transport_type = context->device->transport_type;

	if (set_link_layer(context, user_param) == FAILURE) {
		fprintf(stderr, " Couldn't set the link layer\n");
		return FAILURE;
	}

	if (user_param->link_type == IBV_LINK_LAYER_ETHERNET &&  user_param->gid_index == -1) {
		user_param->gid_index = 0;
	}

	if (user_param->connection_type == RawEth) {

		if (user_param->link_type != IBV_LINK_LAYER_ETHERNET) {
			fprintf(stderr, " Raw Etherent test can only run on Ethernet link! exiting ...\n");
			return FAILURE;
		}

		if (set_eth_mtu(user_param) != 0 ) {
			fprintf(stderr, " Couldn't set Eth MTU\n");
			return FAILURE;
		}
	} else {
		user_param->curr_mtu = set_mtu(context,user_param->ib_port,user_param->mtu);
	}

	if (user_param->dualport==ON) {

		if (user_param->link_type2 == IBV_LINK_LAYER_ETHERNET &&  user_param->gid_index2 == -1) {
			user_param->gid_index2 = 1;
		}
	}

	/* Compute Max inline size with pre found statistics values */
	ctx_set_max_inline(context,user_param);

	if (user_param->verb == READ || user_param->verb == ATOMIC)
		user_param->out_reads = ctx_set_out_reads(context,user_param);
	else
		user_param->out_reads = 1;

	if (user_param->connection_type == UD && user_param->size > MTU_SIZE(user_param->curr_mtu)) {

		if (user_param->test_method == RUN_ALL) {
			fprintf(stderr," Max msg size in UD is MTU %lu\n",MTU_SIZE(user_param->curr_mtu));
			fprintf(stderr," Changing to this MTU\n");
		}
		user_param->size = MTU_SIZE(user_param->curr_mtu);
	}

	/* checking msg size in raw ethernet */
	if (user_param->connection_type == RawEth){
		if (user_param->size > user_param->curr_mtu) {
			fprintf(stderr," Max msg size in RawEth is MTU %d\n",user_param->curr_mtu);
			fprintf(stderr," Changing msg size to this MTU\n");
			user_param->size = user_param->curr_mtu;
		} else if (user_param->size < RAWETH_MIN_MSG_SIZE) {
			printf(" Min msg size for RawEth is 64B - changing msg size to 64 \n");
			user_param->size = RAWETH_MIN_MSG_SIZE;
		}
	}

	if (user_param->pkey_index > 0)
		user_param->pkey_index = ctx_chk_pkey_index(context, user_param->pkey_index);

	return SUCCESS;
}


/******************************************************************************
 *
 ******************************************************************************/
int check_link(struct ibv_context *context,struct perftest_parameters *user_param)
{
	user_param->transport_type = context->device->transport_type;
	if (set_link_layer(context, user_param) == FAILURE){
		fprintf(stderr, " Couldn't set the link layer\n");
		return FAILURE;
	}

	if (user_param->link_type == IBV_LINK_LAYER_ETHERNET &&  user_param->gid_index == -1) {
		user_param->gid_index = 0;
	}

	if (user_param->connection_type == RawEth) {

		if (user_param->link_type != IBV_LINK_LAYER_ETHERNET) {
			fprintf(stderr, " Raw Etherent test can only run on Ethernet link! exiting ...\n");
			return FAILURE;
		}
	}

	/* in case of dual-port mode */
	if (user_param->dualport==ON) {
		if (user_param->link_type2 == IBV_LINK_LAYER_ETHERNET &&  user_param->gid_index2 == -1) {
			user_param->gid_index2 = 1;
		}
	}

	/* Compute Max inline size with pre found statistics values */
	ctx_set_max_inline(context,user_param);

	if (user_param->verb == READ || user_param->verb == ATOMIC)
		user_param->out_reads = ctx_set_out_reads(context,user_param);
	else
		user_param->out_reads = 1;

	if (user_param->pkey_index > 0)
		user_param->pkey_index = ctx_chk_pkey_index(context, user_param->pkey_index);

	return SUCCESS;
}

/******************************************************************************
 *
 ******************************************************************************/
void ctx_print_test_info(struct perftest_parameters *user_param)
{
	int temp = 0;

	if (user_param->output != FULL_VERBOSITY)
		return;

	printf(RESULT_LINE);
	printf("                    ");
	printf("%s ",testsStr[user_param->verb]);

	if (user_param->verb == ATOMIC) {
		printf("%s ",atomicTypesStr[user_param->atomicType]);
	}

	if (user_param->tst == BW) {

		if (user_param->duplex) {
			printf("Bidirectional ");
		}

		if (user_param->post_list > 1) {
			printf("Post List ");
		}

		printf("BW ");

	} else if (user_param->tst == LAT) {
		printf("Latency ");
	}

	if (user_param->mac_fwd) {
		printf("forwarding ");
	}

	if (user_param->use_mcg)
		printf("Multicast ");

	printf("Test\n");

	if (user_param->use_event) {
		printf(" Test with events. Using %s_comp%d\n", user_param->ib_devname, user_param->eq_num);
	}

	if (user_param->use_mcg)
		printf(" MultiCast runs on UD!\n");

	printf(" Dual-port       : %s\t\tDevice         : %s\n", user_param->dualport ? "ON" : "OFF",user_param->ib_devname);
	printf(" Number of qps   : %d\t\tTransport type : %s\n", user_param->num_of_qps, transport_str(user_param->transport_type));
	printf(" Connection type : %s\t\tUsing SRQ      : %s\n", connStr[user_param->connection_type], user_param->use_srq ? "ON"  : "OFF");
	#ifdef HAVE_RO
	printf(" PCIe relax order: %s\n", user_param->disable_pcir ? "OFF"  : "ON");
	if ((check_pcie_relaxed_ordering_compliant() == false) &&
	    (user_param->disable_pcir == 0)) {
		printf(" WARNING: CPU is not PCIe relaxed ordering compliant.\n");
		printf(" WARNING: You should disable PCIe RO with `--disable_pcie_relaxed` for both server and client.\n");
	}
	#else
	printf(" PCIe relax order: %s\n", "Unsupported");
	#endif
	printf(" ibv_wr* API     : %s\n", user_param->use_old_post_send ? "OFF" : "ON");
	if (user_param->machine == CLIENT || user_param->duplex) {
		printf(" TX depth        : %d\n",user_param->tx_depth);
	}

	if (user_param->post_list > 1)
		printf(" Post List       : %d\n",user_param->post_list);
	if (user_param->recv_post_list > 1)
		printf(" Recv Post List  : %d\n", user_param->recv_post_list);

	if ((user_param->verb == SEND || user_param->verb == WRITE_IMM) && (user_param->machine == SERVER || user_param->duplex)) {
		printf(" RX depth        : %d\n",user_param->rx_depth);
	}

	#ifdef HAVE_DCS
	if (user_param->log_dci_streams) {
		printf(" DCS num streams : %d\n",user_param->log_dci_streams);
	}
	if ( user_param->log_active_dci_streams) {
		printf(" DCS active num streams : %d\n",user_param->log_active_dci_streams);
	}
	#endif
	#ifdef HAVE_AES_XTS
	if(user_param->aes_xts){
		int aes_block_size_array [AES_XTS_BLOCK_ARRAY_SIZE] = {512, 520, 4048, 4096, 4160};

		printf(" Number of data encryption keys  : %d\n", user_param->data_enc_keys_number);
		printf(" Block Size      : %d\n", aes_block_size_array[user_param->aes_block_size]);
	}
	#endif

	if (user_param->tst == BW) {
		printf(" CQ Moderation   : %d\n",user_param->cq_mod);
	}

	printf(" Mtu             : %lu[B]\n",user_param->connection_type == RawEth ? user_param->curr_mtu : MTU_SIZE(user_param->curr_mtu));
	printf(" Link type       : %s\n" ,link_layer_str(user_param->link_type));

	/* we use the receive buffer only for mac forwarding. */
	if (user_param->mac_fwd == ON)
		printf(" Buffer size     : %d[B]\n" ,user_param->buff_size/2);

	if (user_param->gid_index != DEF_GID_INDEX)
		printf(" GID index       : %d\n", user_param->gid_index);
	if ((user_param->dualport == ON) && (user_param->gid_index2 != DEF_GID_INDEX))
		printf(" GID index2      : %d\n", user_param->gid_index2);

	if (user_param->verb != READ && user_param->verb != ATOMIC)
		printf(" Max inline data : %d[B]\n",user_param->inline_size);

	else
		printf(" Outstand reads  : %d\n",user_param->out_reads);

	printf(" rdma_cm QPs	 : %s\n",qp_state[user_param->work_rdma_cm]);

	if (user_param->use_rdma_cm)
		temp = 1;

	if (rocm_memory_supported())
		printf(" Use ROCm memory : %s\n", user_param->memory_type == MEMORY_ROCM ? "ON" : "OFF");

	printf(" Data ex. method : %s",exchange_state[temp]);

	if (user_param->work_rdma_cm) {

		if (user_param->tos != DEF_TOS) {
			printf(" \tTOS    : %d",user_param->tos);
		}

		if (user_param->machine == SERVER) {
			putchar('\n');
			printf(RESULT_LINE);
			printf(" Waiting for client rdma_cm QP to connect\n");
			printf(" Please run the same command with the IB/RoCE interface IP");
		}
	}
	putchar('\n');

	printf(RESULT_LINE);

}

static float calc_cpu_util (struct perftest_parameters *user_param)
{
	long long ustat_diff, idle_diff;
	ustat_diff = user_param->cpu_util_data.ustat[1] - user_param->cpu_util_data.ustat[0];
	idle_diff = user_param->cpu_util_data.idle[1] - user_param->cpu_util_data.idle[0];

	if ((ustat_diff + idle_diff) != 0)
		return ((float)ustat_diff / (ustat_diff + idle_diff)) * 100;
	else
		return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
void print_report_bw (struct perftest_parameters *user_param, struct bw_report_data *my_bw_rep)
{
	double cycles_to_units,sum_of_test_cycles;
	int location_arr;
	int opt_completed = 0;
	int opt_posted = 0;
	int run_inf_bi_factor;
	int num_of_qps = user_param->num_of_qps;
	long format_factor;
	uint64_t num_of_calculated_iters = user_param->iters;

	int free_my_bw_rep = 0;
	if (user_param->test_method == RUN_INFINITELY) {
		user_param->tcompleted[opt_posted]= get_cycles();
		/*
                 * cumulative iterations may reach maximum and restarts from 0
                 * then iters < last_iters
                 */
		num_of_calculated_iters = (uint64_t)(user_param->iters - user_param->last_iters);
	}

	cycles_t opt_delta, peak_up, peak_down,tsize;

	opt_delta = user_param->tcompleted[opt_posted] - user_param->tposted[opt_completed];

	if((user_param->connection_type == DC ||user_param->use_xrc) && user_param->duplex)
		num_of_qps /= 2;

	if (user_param->noPeak == OFF) {
		int i, j;
		cycles_t t;
		/* Find the peak bandwidth unless asked not to in command line */
		for (i = 0; i < num_of_calculated_iters * num_of_qps; i += user_param->post_list) {
			for (j = ROUND_UP(i + 1, user_param->cq_mod) - 1; j < num_of_calculated_iters * num_of_qps;
					j += user_param->cq_mod) {
				t = (user_param->tcompleted[j] - user_param->tposted[i]) / (j - i + 1);
				if (t < opt_delta)
					opt_delta  = t;
			}

			/* Handle case where CQE was explicitly signaled on last iteration. */
			if ((num_of_calculated_iters * num_of_qps) % user_param->cq_mod) {
				j = num_of_calculated_iters * num_of_qps - 1;
				t = (user_param->tcompleted[j] - user_param->tposted[i]) / (j - i + 1);
				if (t < opt_delta)
					opt_delta  = t;
			}
		}
	}

	cycles_to_units = get_cpu_mhz(user_param->cpu_freq_f) * 1000000;
	if ((cycles_to_units == 0 && !user_param->cpu_freq_f)) {
		fprintf(stderr,"Can't produce a report\n");
		exit(1);
	}

	run_inf_bi_factor = (user_param->duplex && user_param->test_method == RUN_INFINITELY) ? (user_param->verb == SEND ? 1 : 2) : 1 ;
	tsize = run_inf_bi_factor * user_param->size;
	num_of_calculated_iters *= (user_param->test_type == DURATION) ? 1 : num_of_qps;
	location_arr = (user_param->noPeak) ? 0 : num_of_calculated_iters - 1;
	/* support in GBS format */
	format_factor = (user_param->report_fmt == MBS) ? 0x100000 : 125000000;

	sum_of_test_cycles = ((double)(user_param->tcompleted[location_arr] - user_param->tposted[0]));

	double bw_avg = ((double)tsize*num_of_calculated_iters * cycles_to_units) / (sum_of_test_cycles * format_factor);
	double msgRate_avg = ((double)num_of_calculated_iters * cycles_to_units * run_inf_bi_factor) / (sum_of_test_cycles * 1000000);

	double bw_avg_p1 = ((double)tsize*user_param->iters_per_port[0] * cycles_to_units) / (sum_of_test_cycles * format_factor);
	double msgRate_avg_p1 = ((double)user_param->iters_per_port[0] * cycles_to_units * run_inf_bi_factor) / (sum_of_test_cycles * 1000000);

	double bw_avg_p2 = ((double)tsize*user_param->iters_per_port[1] * cycles_to_units) / (sum_of_test_cycles * format_factor);
	double msgRate_avg_p2 = ((double)user_param->iters_per_port[1] * cycles_to_units * run_inf_bi_factor) / (sum_of_test_cycles * 1000000);

	peak_up = !(user_param->noPeak)*(cycles_t)tsize*(cycles_t)cycles_to_units;
	peak_down = (cycles_t)opt_delta * format_factor;

	if (my_bw_rep == NULL) {
		free_my_bw_rep = 1;
		ALLOCATE(my_bw_rep , struct bw_report_data , 1);
		memset(my_bw_rep, 0, sizeof(struct bw_report_data));
	}

	my_bw_rep->size = (unsigned long)user_param->size;
	my_bw_rep->iters = num_of_calculated_iters;
	my_bw_rep->bw_peak = (double)peak_up/peak_down;
	my_bw_rep->bw_avg = bw_avg;
	my_bw_rep->msgRate_avg = msgRate_avg;
	my_bw_rep->bw_avg_p1 = bw_avg_p1;
	my_bw_rep->msgRate_avg_p1 = msgRate_avg_p1;
	my_bw_rep->bw_avg_p2 = bw_avg_p2;
	my_bw_rep->msgRate_avg_p2 = msgRate_avg_p2;
	my_bw_rep->sl = user_param->sl;

	if (!user_param->duplex || ((user_param->verb == SEND || user_param->verb == WRITE_IMM) && user_param->test_type == DURATION)
			|| user_param->test_method == RUN_INFINITELY || user_param->connection_type == RawEth)
		print_full_bw_report(user_param, my_bw_rep, NULL);

	if (free_my_bw_rep == 1) {
		free(my_bw_rep);
	}
}

static void write_test_info_to_file(int out_json_fds, struct perftest_parameters *user_param)
{
	int temp = 0;
	dprintf(out_json_fds, "\"test_info\": {\n");
	dprintf(out_json_fds, "\"test\": \"%s_",testsStr[user_param->verb]);

	if (user_param->verb == ATOMIC) {
		dprintf(out_json_fds, "%s_",atomicTypesStr[user_param->atomicType]);
	}

	if (user_param->tst == BW) {
		if (user_param->duplex) {
			dprintf(out_json_fds, "Bidirectional_");
		}

		if (user_param->post_list > 1) {
			dprintf(out_json_fds, "Post_List_");
		}

		dprintf(out_json_fds, "BW_");

	} else if (user_param->tst == LAT) {
		dprintf(out_json_fds, "Latency_");
	}

	if (user_param->mac_fwd) {
		dprintf(out_json_fds, "forwarding_");
	}

	if (user_param->use_mcg)
		dprintf(out_json_fds, "Multicast_");

	dprintf(out_json_fds, "Test\",\n");

	if (user_param->use_event) {
		dprintf(out_json_fds, "\"Test with events\": \"Using_%s_comp%d\",\n", user_param->ib_devname, user_param->eq_num);
	}

	if (user_param->use_mcg)
		dprintf(out_json_fds, " \"MultiCast_runs\": \"on_UD\",\n");

	dprintf(out_json_fds, "\"Dual_port\": \"%s\",\n\"Device\": \"%s\",\n", user_param->dualport ? "ON" : "OFF",user_param->ib_devname);
	dprintf(out_json_fds, "\"Number_of_qps\": %d,\n\"Transport_type\": \"%s\",\n", user_param->num_of_qps, transport_str(user_param->transport_type));
	dprintf(out_json_fds, "\"Connection_type\": \"%s\",\n\"Using_SRQ\": \"%s\",\n", connStr[user_param->connection_type], user_param->use_srq ? "ON"  : "OFF");
	#ifdef HAVE_RO
	dprintf(out_json_fds, "\"PCIe_relax_order\": \"%s\",\n", user_param->disable_pcir ? "OFF"  : "ON");
	if ((check_pcie_relaxed_ordering_compliant() == false) &&
	    (user_param->disable_pcir == 0)) {
		dprintf(out_json_fds, "\"WARNING1\": \"CPU is not PCIe relaxed ordering compliant\",\n");
		dprintf(out_json_fds, "\"WARNING2\": \"You should disable PCIe RO with --disable_pcie_relaxed for both server and client\",\n");
	}
	#else
	dprintf(out_json_fds, " \"PCIe_relax_order\": \"%s\",\n", "Unsupported");
	#endif
	dprintf(out_json_fds, "\"ibv_wr_API\": \"%s\",\n", user_param->use_old_post_send ? "OFF" : "ON");
	if (user_param->machine == CLIENT || user_param->duplex) {
		dprintf(out_json_fds, "\"TX_depth\": %d,\n",user_param->tx_depth);
	}

	if (user_param->post_list > 1)
		dprintf(out_json_fds, "\"Post_List\": %d,\n",user_param->post_list);
	if (user_param->recv_post_list > 1)
		dprintf(out_json_fds, "\"Recv_Post_List\": %d,\n", user_param->recv_post_list);

	if ((user_param->verb == SEND || user_param->verb == WRITE_IMM) &&
			(user_param->machine == SERVER || user_param->duplex)) {
		dprintf(out_json_fds, "\"RX_depth\": %d,\n",user_param->rx_depth);
	}

	if (user_param->tst == BW) {
		dprintf(out_json_fds, "\"CQ_Moderation\": %d,\n",user_param->cq_mod);
	}

	dprintf(out_json_fds, "\"Mtu\": %lu,\n",user_param->connection_type == RawEth ? user_param->curr_mtu : MTU_SIZE(user_param->curr_mtu));
	dprintf(out_json_fds, "\"Link_type\": \"%s\",\n" ,link_layer_str(user_param->link_type));

	/* we use the receive buffer only for mac forwarding. */
	if (user_param->mac_fwd == ON)
		dprintf(out_json_fds, "\"Buffer_size\": %d,\n" ,user_param->buff_size/2);

	if (user_param->gid_index != DEF_GID_INDEX)
		dprintf(out_json_fds, "\"GID_index\": %d,\n", user_param->gid_index);
	if ((user_param->dualport == ON) && (user_param->gid_index2 != DEF_GID_INDEX))
		dprintf(out_json_fds, "\"GID_index2\": %d,\n", user_param->gid_index2);

	if (user_param->verb != READ && user_param->verb != ATOMIC)
		dprintf(out_json_fds, "\"Max_inline_data\": %d,\n",user_param->inline_size);

	else
		dprintf(out_json_fds, "\"Outstand_reads\": %d,\n",user_param->out_reads);

	dprintf(out_json_fds, "\"rdma_cm_QPs\": \"%s\",\n",qp_state[user_param->work_rdma_cm]);

	if (user_param->use_rdma_cm)
		temp = 1;

	dprintf(out_json_fds, "\"Use_ROCm_memory\": \"%s\",\n", user_param->memory_type == MEMORY_ROCM ? "ON" : "OFF");

	dprintf(out_json_fds, "\"Data_ex_method\": \"%s\"",exchange_state[temp]);

	if (user_param->work_rdma_cm) {

		if (user_param->tos != DEF_TOS) {
			dprintf(out_json_fds, ",\n\"TOS\": %d",user_param->tos);
		}

	}

	dprintf(out_json_fds, "\n},\n");
}

static void write_bw_report_to_file(int out_json_fd, struct perftest_parameters *user_param, int inc_accuracy,
		double bw_avg, double msgRate_avg, unsigned long size, int sl, uint64_t iters, double bw_peak) {

	dprintf(out_json_fd, "\"results\": {\n");

	if (user_param->output == OUTPUT_BW)
		dprintf(out_json_fd, "\"bw_avg\": %lf,\n", bw_avg);
	else if (user_param->output == OUTPUT_MR)
		dprintf(out_json_fd, "\"msgRate_avg\": %lf,\n", msgRate_avg);
	else if (user_param->raw_qos)
		dprintf(out_json_fd, REPORT_FMT_QOS_JSON, size, sl, iters, bw_peak, bw_avg, msgRate_avg);
	else
		dprintf(out_json_fd, inc_accuracy ? REPORT_FMT_EXT_JSON : REPORT_FMT_JSON,
								   size, iters, bw_peak, bw_avg, msgRate_avg);

	dprintf(out_json_fd, user_param->cpu_util_data.enable ?
							REPORT_EXT_CPU_UTIL_JSON : REPORT_EXT_JSON, calc_cpu_util(user_param));

	dprintf(out_json_fd, "}\n");
}

/******************************************************************************
 *
 ******************************************************************************/

void print_full_bw_report (struct perftest_parameters *user_param, struct bw_report_data *my_bw_rep, struct bw_report_data *rem_bw_rep)
{

	double bw_peak     = my_bw_rep->bw_peak;
	double bw_avg      = my_bw_rep->bw_avg;
	double bw_avg_p1      = my_bw_rep->bw_avg_p1;
	double bw_avg_p2      = my_bw_rep->bw_avg_p2;
	double msgRate_avg = my_bw_rep->msgRate_avg;
	double msgRate_avg_p1 = my_bw_rep->msgRate_avg_p1;
	double msgRate_avg_p2 = my_bw_rep->msgRate_avg_p2;
	int inc_accuracy = ((bw_avg < 0.1) && (user_param->report_fmt == GBS));

	if (rem_bw_rep != NULL) {
		bw_peak     += rem_bw_rep->bw_peak;
		bw_avg      += rem_bw_rep->bw_avg;
		bw_avg_p1      += rem_bw_rep->bw_avg_p1;
		bw_avg_p2      += rem_bw_rep->bw_avg_p2;
		msgRate_avg += rem_bw_rep->msgRate_avg;
		msgRate_avg_p1 += rem_bw_rep->msgRate_avg_p1;
		msgRate_avg_p2 += rem_bw_rep->msgRate_avg_p2;
	}

	if ( (user_param->duplex && rem_bw_rep != NULL) ||  (!user_param->duplex && rem_bw_rep == NULL) || (user_param->duplex && user_param->verb == SEND)) {
		/* Verify Limits */
		if ( ((user_param->is_limit_bw == ON )&& (user_param->limit_bw > bw_avg)) )
			user_param->is_bw_limit_passed |= 0;
		else
			user_param->is_bw_limit_passed |= 1;

		if ( (user_param->is_limit_msgrate) && (user_param->limit_msgrate > msgRate_avg) )
			user_param->is_msgrate_limit_passed |= 0;
		else
			user_param->is_msgrate_limit_passed |= 1;
	}

	if(user_param->out_json) {
		int out_json_fd = open_file_write(user_param->out_json_file_name);
		if(out_json_fd >= 0){
			dprintf(out_json_fd,"{\n");
			write_test_info_to_file(out_json_fd, user_param);
			write_bw_report_to_file(out_json_fd, user_param, inc_accuracy,
					bw_avg, msgRate_avg, my_bw_rep->size, my_bw_rep->sl, my_bw_rep->iters, bw_peak);
			dprintf(out_json_fd,"}\n");
			close(out_json_fd);
		}
	}

	if (user_param->output == OUTPUT_BW)
		printf("%lf\n",bw_avg);
	else if (user_param->output == OUTPUT_MR)
		printf("%lf\n",msgRate_avg);
	else if (user_param->raw_qos)
		printf( REPORT_FMT_QOS, my_bw_rep->size, my_bw_rep->sl, my_bw_rep->iters, bw_peak, bw_avg, msgRate_avg);
	else if (user_param->report_per_port)
		printf(REPORT_FMT_PER_PORT, my_bw_rep->size, my_bw_rep->iters, bw_peak, bw_avg, msgRate_avg, bw_avg_p1, msgRate_avg_p1, bw_avg_p2, msgRate_avg_p2);
	else
		printf( inc_accuracy ? REPORT_FMT_EXT : REPORT_FMT, my_bw_rep->size, my_bw_rep->iters, bw_peak, bw_avg, msgRate_avg);
	if (user_param->output == FULL_VERBOSITY) {
		fflush(stdout);
		fprintf(stdout, user_param->cpu_util_data.enable ? REPORT_EXT_CPU_UTIL : REPORT_EXT , calc_cpu_util(user_param));
	}
	if (user_param->counter_ctx) {
		counters_print(user_param->counter_ctx);
	}
}
/******************************************************************************
 *
 ******************************************************************************/
// cppcheck-suppress constParameter
static inline cycles_t get_median(int n, cycles_t delta[])
{
	if ((n - 1) % 2)
		return(delta[n / 2] + delta[n / 2 - 1]) / 2;
	else
		return delta[n / 2];
}

/******************************************************************************
 *
 ******************************************************************************/
static int cycles_compare(const void *aptr, const void *bptr)
{
	const cycles_t *a = aptr;
	const cycles_t *b = bptr;
	if (*a < *b) return -1;
	if (*a > *b) return 1;

	return 0;
}


void write_report_lat_to_file(int out_json_fd, struct perftest_parameters *user_param,
		double latency, double stdev, double average_sum, double average, double stdev_sum,
		int iters_99, int iters_99_9, double cycles_rtt_quotient, cycles_t *delta, int measure_cnt) // cppcheck-suppress constParameter
		{

	dprintf(out_json_fd, "\"results\": {\n");

	if (user_param->output == OUTPUT_LAT)
		dprintf(out_json_fd, "\"avg_lat\": %lf\n",average);
	else {
		dprintf(out_json_fd, REPORT_FMT_LAT_JSON,
				(unsigned long)user_param->size,
				user_param->iters,
				delta[0] / cycles_rtt_quotient,
				delta[measure_cnt] / cycles_rtt_quotient,
				latency,
				average,
				stdev,
				delta[iters_99] / cycles_rtt_quotient,
				delta[iters_99_9] / cycles_rtt_quotient);
		dprintf(out_json_fd, user_param->cpu_util_data.enable ?
		REPORT_EXT_CPU_UTIL_JSON : REPORT_EXT_JSON , calc_cpu_util(user_param));
	}

	dprintf(out_json_fd, "}\n");
}

/******************************************************************************
 *
 ******************************************************************************/
#define LAT_MEASURE_TAIL (2)
void print_report_lat (struct perftest_parameters *user_param)
{

	int i;
	int rtt_factor;
	double cycles_to_units, cycles_rtt_quotient;
	cycles_t median ;
	cycles_t *delta = NULL;
	const char* units;
	double latency, stdev, average_sum = 0 , average, stdev_sum = 0;
	int iters_99, iters_99_9;
	int measure_cnt;

	measure_cnt = (user_param->tst == LAT) ? user_param->iters - 1 : (user_param->iters) / user_param->reply_every;
	rtt_factor = (user_param->verb == READ || user_param->verb == ATOMIC) ? 1 : 2;
	ALLOCATE(delta, cycles_t, measure_cnt);

	if (user_param->r_flag->cycles) {
		cycles_to_units = 1;
		units = "cycles";
	} else {
		cycles_to_units = get_cpu_mhz(user_param->cpu_freq_f);
		units = "usec";
	}

	if (user_param->tst == LAT) {
		for (i = 0; i < measure_cnt; ++i) {
			delta[i] = user_param->tposted[i + 1] - user_param->tposted[i];
		}
	} else if (user_param->tst == LAT_BY_BW) {
		for (i = 0; i < measure_cnt; ++i) {
			delta[i] = user_param->tcompleted[i] - user_param->tposted[i];
		}
	}
	else {
		fprintf(stderr,"print report LAT is support in LAT and LAT_BY_BW tests only\n");
		exit(1);
	}

	cycles_rtt_quotient = cycles_to_units * rtt_factor;
	if (user_param->r_flag->unsorted) {
		printf("#, %s\n", units);
		for (i = 0; i < measure_cnt; ++i)
			printf("%d, %g\n", i + 1, delta[i] / cycles_rtt_quotient);
	}

	qsort(delta, measure_cnt, sizeof *delta, cycles_compare);
	measure_cnt = measure_cnt - LAT_MEASURE_TAIL;
	median = get_median(measure_cnt, delta);

	/* calcualte average sum on sorted array*/
	for (i = 0; i < measure_cnt; ++i)
		average_sum += (delta[i] / cycles_rtt_quotient);

	average = average_sum / measure_cnt;

	/* Calculate stdev by variance*/
	for (i = 0; i < measure_cnt; ++i) {
		int temp_var = average - (delta[i] / cycles_rtt_quotient);
		int pow_var = pow(temp_var, 2 );
		stdev_sum += pow_var;
	}

	if (user_param->r_flag->histogram) {
		printf("#, %s\n", units);
		for (i = 0; i < measure_cnt; ++i)
			printf("%d, %g\n", i + 1, delta[i] / cycles_rtt_quotient);
	}

	if (user_param->r_flag->unsorted || user_param->r_flag->histogram) {
		if (user_param->output == FULL_VERBOSITY) {
			printf(RESULT_LINE);
			printf("%s",(user_param->test_type == ITERATIONS) ? RESULT_FMT_LAT : RESULT_FMT_LAT_DUR);
			printf((user_param->cpu_util_data.enable ? RESULT_EXT_CPU_UTIL : RESULT_EXT));
		}
	}

	latency = median / cycles_rtt_quotient;
	stdev = sqrt(stdev_sum / measure_cnt);
	iters_99 = ceil((measure_cnt) * 0.99);
	iters_99_9 = ceil((measure_cnt) * 0.999);

	if(user_param->out_json) {
		int out_json_fd = open_file_write(user_param->out_json_file_name);
		if(out_json_fd >= 0){
			dprintf(out_json_fd,"{\n");
			write_test_info_to_file(out_json_fd, user_param);
			write_report_lat_to_file(out_json_fd, user_param, latency, stdev, average_sum, average, stdev_sum,
					iters_99, iters_99_9, cycles_rtt_quotient, delta, measure_cnt);
			dprintf(out_json_fd,"}\n");
			close(out_json_fd);
		}
	}

	if (user_param->output == OUTPUT_LAT)
		printf("%lf\n",average);
	else {
		printf(REPORT_FMT_LAT,
				(unsigned long)user_param->size,
				user_param->iters,
				delta[0] / cycles_rtt_quotient,
				delta[measure_cnt] / cycles_rtt_quotient,
				latency,
				average,
				stdev,
				delta[iters_99] / cycles_rtt_quotient,
				delta[iters_99_9] / cycles_rtt_quotient);
		printf( user_param->cpu_util_data.enable ? REPORT_EXT_CPU_UTIL : REPORT_EXT , calc_cpu_util(user_param));
	}

	if (user_param->counter_ctx) {
		counters_print(user_param->counter_ctx);
	}

	free(delta);
}

void write_report_lat_duration_to_file (int out_json_fd, struct perftest_parameters *user_param, double latency, double tps){

	dprintf(out_json_fd, "\"results\": {\n");

	if (user_param->output == OUTPUT_LAT) {
		dprintf(out_json_fd, "\"t_avg\": %lf\n",latency);
	}
	else {
		dprintf(out_json_fd, REPORT_FMT_LAT_DUR_JSON,
				user_param->size,
				user_param->iters,
				latency, tps);
		dprintf(out_json_fd,  user_param->cpu_util_data.enable ?
		REPORT_EXT_CPU_UTIL_JSON : REPORT_EXT_JSON,
		calc_cpu_util(user_param));
	}

	dprintf(out_json_fd, "}\n");
}
/******************************************************************************
 *
 ******************************************************************************/
void print_report_lat_duration (struct perftest_parameters *user_param)
{
	int rtt_factor;
	double cycles_to_units;
	cycles_t test_sample_time;
	double latency, tps;

	rtt_factor = (user_param->verb == READ || user_param->verb == ATOMIC) ? 1 : 2;
	cycles_to_units = get_cpu_mhz(user_param->cpu_freq_f);

	test_sample_time = (user_param->tcompleted[0] - user_param->tposted[0]);
	latency = (((test_sample_time / cycles_to_units) / rtt_factor) / user_param->iters);
	tps = user_param->iters / (test_sample_time / (cycles_to_units * 1000000));


	if(user_param->out_json) {
		int out_json_fd = open_file_write(user_param->out_json_file_name);
		if(out_json_fd >= 0){
			dprintf(out_json_fd,"{\n");
			write_test_info_to_file(out_json_fd, user_param);
			write_report_lat_duration_to_file(out_json_fd, user_param, latency, tps);
			dprintf(out_json_fd,"}\n");
			close(out_json_fd);
		}
	}

	if (user_param->output == OUTPUT_LAT) {
		printf("%lf\n",latency);
	}
	else {
		printf(REPORT_FMT_LAT_DUR,
				user_param->size,
				user_param->iters,
				latency, tps);
		printf( user_param->cpu_util_data.enable ? REPORT_EXT_CPU_UTIL : REPORT_EXT , calc_cpu_util(user_param));
	}

	if (user_param->counter_ctx) {
		counters_print(user_param->counter_ctx);
	}
}

void print_report_fs_rate (struct perftest_parameters *user_param)
{

	double cycles_to_units, units_to_sec;
	cycles_t average_sum = 0;
	cycles_t *delta = NULL;
	const char* units;
	double latency = 0, average = 0, fps = 0;
	int measure_cnt = 1;

	if (user_param->r_flag->cycles) {
		cycles_to_units = 1;
		units = CYCLES;
		units_to_sec =  get_cpu_mhz(user_param->cpu_freq_f);
	} else {
		cycles_to_units = get_cpu_mhz(user_param->cpu_freq_f);
		units = USEC;
		units_to_sec = 1000000;
	}

	if (user_param->test_type == ITERATIONS) {
		int i;
		cycles_t median;
		measure_cnt = user_param->flows;
		ALLOCATE(delta, cycles_t, measure_cnt);

		for (i = 0; i < measure_cnt; ++i)
			delta[i] = user_param->tcompleted[i] - user_param->tposted[i];

		if (user_param->r_flag->unsorted) {
			printf("#, %s\n", units);
			for (i = 0; i < measure_cnt; ++i)
				printf("%d, %g\n", i + 1, delta[i] / cycles_to_units);
		}

		qsort(delta, measure_cnt, sizeof *delta, cycles_compare);
		median = get_median(measure_cnt, delta);

		/* calcualte average sum on sorted array*/
		for (i = 0; i < measure_cnt; ++i)
			average_sum += delta[i];


		average = average_sum / measure_cnt / cycles_to_units;


		if (user_param->r_flag->histogram) {
			printf("#, %s\n", units);
			for (i = 0; i < measure_cnt; ++i)
				printf("%d, %g\n", i + 1, delta[i] / cycles_to_units);
		}
		latency = median / cycles_to_units;
	}
	else {
		cycles_t test_sample_time = (user_param->tcompleted[0] - user_param->tposted[0]);
		latency = test_sample_time  / user_param->iters / cycles_to_units;
		average = latency;
		fps = user_param->iters / (test_sample_time / (cycles_to_units * units_to_sec));
	}

	if (user_param->output == FULL_VERBOSITY) {
		printf(RESULT_LINE);
		printf("%s", (user_param->test_type == ITERATIONS) ? RESULT_FMT_FS_RATE : RESULT_FMT_FS_RATE_DUR);
		printf((user_param->cpu_util_data.enable ? RESULT_EXT_CPU_UTIL : RESULT_EXT));
	}

	if (user_param->output == OUTPUT_LAT)
		printf("%lf\n", average);
	else {
		if (user_param->test_type == ITERATIONS) {
			fps = measure_cnt / (average_sum / (cycles_to_units * units_to_sec));
			printf(REPORT_FMT_FS_RATE,
				user_param->iters,
				delta[0] / cycles_to_units,
				delta[measure_cnt - 1] / cycles_to_units,
				latency,
				average,
				fps);
		} else {
			printf(REPORT_FMT_FS_RATE_DUR,
				user_param->iters,
				latency,
				fps);
		}
		printf(user_param->cpu_util_data.enable ? REPORT_EXT_CPU_UTIL : REPORT_EXT, calc_cpu_util(user_param));
	}

	free(delta);
}
/******************************************************************************
 * End
 ******************************************************************************/
