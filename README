	     Open Fabrics Enterprise Distribution (OFED)
                Performance Tests README for OFED 2.0
		  
			  January 2013



===============================================================================
Table of Contents
===============================================================================
1. Overview
2. Notes on Testing Methodology
3. Test Descriptions
4. Running Tests
5. Known Issues

===============================================================================
1. Overview
===============================================================================
This is a collection of tests written over uverbs intended for use as a
performance micro-benchmark. As an example, the tests can be used for
HW or SW tuning and/or functional testing.

The collection conatains a set of BW and latency benchmark such as :

	* Read   - ib_read_bw and ib_read_lat.
	* Write  - ib_write_bw and ib_wriet_lat.
	* Send   - ib_send_bw and ib_send_lat.
	* Atomic - ib_atomic_bw and ib_atomic_lat
	* Raw Etherent (when working with MOFED2) - raw_ethernet_bw. 

Please post results/observations to the openib-general mailing list.
See "Contact Us" at http://openib.org/mailman/listinfo/openib-general and
http://www.openib.org.

===============================================================================
2. Notes on Testing Methodology
===============================================================================
- The benchmark used the CPU cycle counter to get time stamps without context
  switch.  Some CPU architectures (e.g., Intel's 80486 or older PPC) do NOT
  have such capability.

- The latency benchmarks measures round-trip time but reports half of that as one-way
  latency. This means that it may not be sufficiently accurate for asymmetrical
  configurations.

- On Bw benchmarks , We calculate the BW on send side only, as he calculates
  the Bw after collecting completion from the receive side.
  In case we use the bidirectional flag , BW is calculated on both sides.
  in ib_send_bw, server side also calculate the received throughput. 

- Min/Median/Max result is reported in latency tests. 
  The median (vs average) is less sensitive to extreme scores.
  Typically, the "Max" value is the first value measured.

- Larger samples help marginally only. The default (1000) is pretty good.
  Note that an array of cycles_t (typically unsigned long) is allocated
  once to collect samples and again to store the difference between them.
  Really big sample sizes (e.g., 1 million) might expose other problems
  with the program. In this case you can use -N flag (No Peak) to instruct 
  the test sample only 2 times (begining and end). 

- All throughput tests now have duration feature as well (-D <seconds to run>) 
  to instruct the test to run for <seconds to run>.
  Another feature added is --run_infinitely, which instruct the test to run 
  all te time and print throughput every 5 seconds. 

- The "-H" option (latency) will dump the histogram for additional statistical analysis.
  See xgraph, ygraph, r-base (http://www.r-project.org/), pspp, or other 
  statistical math programs.

Architectures tested:	i686, x86_64, ia64


===============================================================================
4. Test Descriptions
===============================================================================

The following tests are mainly useful for HW/SW benchmarking.
They are not intended as actual usage examples.

send_lat.c 	latency test with send transactions
send_bw.c 	BW test with send transactions
write_lat.c 	latency test with RDMA write transactions
write_bw.c 	BW test with RDMA write transactions
read_lat.c 	latency test with RDMA read transactions
read_bw.c 	BW test with RDMA read transactions
atomic_lat.c	latency test with atomic transactions
atomic_bw.c 	BW test atomic transactions
raw_ethernet_send_bw.c BW test with Raw Etherent client and receiver. 

The executable name of each test starts with the general prefix "ib_",e.g., ib_write_lat.
Except of raw_ethernet_send_bw.c case, which the binary called raw_ethernet_bw.

Running Tests
-------------

Prerequisites: 
	kernel 2.6
	(kernel module) matches libibverbs
	(kernel module) matches librdmacm
	(kernel module) matches libibumad
	(kernel module) matches libmath (lm).
	

Server:		./<test name> <options>
Client:		./<test name> <options> <server IP address>

		o  <server address> is IPv4 or IPv6 address. You can use the IPoIB
                   address if IPoIB is configured.
		o  --help lists the available <options>

  *** IMPORTANT NOTE: The SAME OPTIONS must be passed to both server and client.
  *** IMPORTANT NOTE: You need to be running a Subnet Manager on the switch or on one of the nodes in your fabric, in case you are in IB fabric.

Common Options to all tests:
----------------------------
  -h, --help			Display this help message screen. 		
  -p, --port=<port>            	Listen on/connect to port <port> (default: 18515) when exchaning data.
  -R, --rdma_cm  	       	Connect QPs with rdma_cm and run test on those QPs.
  -z, --com_rdma_cm  	       	Communicate with rdma_cm module to exchange data - use regular QPs.
  -m, --mtu=<mtu>              	QP Mtu size (default: active_mtu from ibv_devinfo).
  -c, --connection=<RC/UC/UD>  	Connection type RC/UC/UD (default RC)
  -d, --ib-dev=<dev>           	Use IB device <dev> (default: first device found).
  -i, --ib-port=<port>         	Use port <port> of IB device (default: 1).
  -s, --size=<size>            	Size of message to exchange (default: 1).
  -a, --all                    	Run sizes from 2 till 2^23.
  -n, --iters=<iters>          	Number of exchanges (at least 100, default: 1000).
  -x, --gid-index=<index>       Test uses GID with GID index taken from command
  -V, --version                 Display version number.
  -e, --events                  Sleep on CQ events (default poll).
  -F, --CPU-freq                Do not fail even if cpufreq_ondemand module.
  -I, --inline_size=<size>     	Max size of message to be sent in inline mode.
  -u, --qp-timeout=<timeout>   	QP timeout, timeout value is 4 usec*2 ^timeout (default: 14).
  -S, --sl=<sl>                	SL - Service Level (default 0)

   -r, --rx-depth=<dep>          Make rx queue bigger than tx (default 600).

Options for latency tests:
--------------------------

  -C, --report-cycles           Report times in cpu cycle units.
  -H, --report-histogram        Print out all results (Default: summary only).
  -U, --report-unsorted  	Print out unsorted results (default sorted).

Options for BW tests:
---------------------

  -b, --bidirectional           Measure bidirectional bandwidth (default uni).  
  -N, --no peak-bw             	Cancel peak-bw calculation (default with peak-bw)
  -Q, --cq-mod  		Generate Cqe only after <cq-mod> completion
  -t, --tx-depth=<dep>          Size of tx queue (default: 128).
  -O, --dualport		Run test in dual-port mode (2 QPs). both ports must be active (default OFF).
  -D, --duration=<sec> 		Run test for <sec> period of seconds.
  -f, --margin=<sec> 		When in Duration, measure results within margins (default: 2)
  -l, --post_list=<list size>   Post list of WQEs of <list size> size (instead of single post).
  -q, --qp=<num of qp's>	Num of QPs running in the process (default: 1).
      --run_infinitely		Run test forever, print results every 5 seconds.

SEND tests (ib_send_lat or ib_send_bw) flags: 
---------------------------------------------

  -r, --rx-depth=<dep>          Size of RX queue (default: 512 in BW test).
  -g, --mcg=<num_of_qps> 	Send messages to multicast group with <num_of_qps> qps attached to it.
  -M, --MGID=<multicast_gid>    In multicast, uses <multicast_gid> as the group MGID.

ATOMIC tests (ib_atomic_lat or ib_atomic_bw) flags: 
---------------------------------------------------

  -A, --atomic_type=<type>	type of atomic operation from {CMP_AND_SWAP,FETCH_AND_ADD}. 
  -o, --outs=<num>		Number of outstanding read/atomic requests - also on READ tests.

Options for Raw Ethernet BW test:
---------------------------------
  -B, --source_mac              source MAC address by this format XX:XX:XX:XX:XX:XX (default take the MAC address form GID).
  -E, --dest_mac                destination MAC address by this format XX:XX:XX:XX:XX:XX **MUST** be entered.
  -J, --server_ip               server ip address by this format X.X.X.X (using to send packets with IP header).
  -j, --client_ip               client ip address by this format X.X.X.X (using to send packets with IP header).
  -K, --server_port             server udp port number (using to send packets with UPD header).
  -k, --client_port             client udp port number (using to send packets with UDP header).
  -Z, --server                  choose server side for the current machine (--server/--client must be selected ).
  -P, --client                  choose client side for the current machine (--server/--client must be selected).

----------------------------------------------
Special feature detailed explination in tests:
----------------------------------------------

  1. Usage of post_list feature (-l, --post_list=<list size>). 
     In this case, each QP will prepare <list size> ibv_send_wr (instead of 1), and will chain them to each other.
     In Chaning we mean allocating <list_size> array, and setting 'next' pointer of each ibv_send_wr in the array
     to point to the following element in the array. the last ibv_send_wr in the array will point to NULL.
     In this case, when post sending the first ibv_send_wr in the list, will instruct the HW to post all of those WQEs.
     Which means each post_send will post <list_size> messages. 
     This feature is good we want to know the maximum message rate of QPs in a single process. 
     Since we are limited to SW post_send (~ 10 Mpps, since we have ~ 500 ns between each SW post_send), we can see 
     the true HW message rate when setting <list_size> of 64 (for example) since it's not depended on SW limitations. 

  2. RDMA_CM feature.
     You can add to all tests the "-R" flag to connect the QPs from each side with the rdma_cm library.
     In this case, the library will connect the QPs and will use the IPoIB interface for doing it.
     It helps when you don't have Ethernet connection between the 2 nodes.
     You must supply the IPoIB interface as the server IP. 

  3. Multicast feauture in ib_send_lat and in ib_send_bw.
     Send tests have built in feature of testing multicast performance, in verbs level.
     You can use "-g" to specify the number of QPs to attach to this multicast group.
     "-M" flag allows you to choose the multicast group address.


===============================================================================
5. Known Issues
===============================================================================
Up until now, we still have known (unsolved) issues in the package.
Here is a list of the main issues:

 1. Multicast feauture in ib_send_lat and in ib_send_bw still have many problems!
    Will increase the support and bug fixes in this Q, but now the tests may stuck
    and could produce undefine behaviours.

 2. Bidirectional feature in ib_send_bw test, when running in UD or UC mode.
    The algorithm we use for the bidirectional measurement is designed for RC connection type.
    When running in UC or UD connection types, there is a small probablity the test will be stuck.
    We are working now to fix it.

 3. RDMA_CM feature in read tests still doesn't work.

 4. Dual-port support currently works only with ib_write_bw.

 5. Compabilty issues may occur between different versions of perftest.
    Please make sure you work with the same version on both sides to ensure consistency of the test.

