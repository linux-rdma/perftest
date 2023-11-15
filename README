	     Open Fabrics Enterprise Distribution (OFED)
                	Performance Tests README



===============================================================================
Table of Contents
===============================================================================
1. Overview
2. Installation
3. Notes on Testing Methodology
4. Test Descriptions
5. Running Tests
6. Known Issues

===============================================================================
1. Overview
===============================================================================
This is a collection of tests written over uverbs intended for use as a
performance micro-benchmark. The tests may be used for HW or SW tuning
as well as for functional testing.

The collection contains a set of bandwidth and latency benchmark such as:

	* Send        - ib_send_bw and ib_send_lat
	* RDMA Read   - ib_read_bw and ib_read_lat
	* RDMA Write  - ib_write_bw and ib_write_lat
	* RDMA Atomic - ib_atomic_bw and ib_atomic_lat
	* Native Ethernet (when working with MOFED2) - raw_ethernet_bw, raw_ethernet_lat

Please post results/observations to the openib-general mailing list.
See "Contact Us" at http://openib.org/mailman/listinfo/openib-general and
http://www.openib.org.

===============================================================================
2. Installation
===============================================================================
-After cloning the repository a perftest directory should appear in your current
 directory

-Cloning example :
git clone <URL>, In our situation its --> git clone https://github.com/linux-rdma/perftest.git

-After cloning, Follow this commands:

-cd perftest/

-./autogen.sh

-./configure    Note:If you want to install in a specific directory use the optional flag --prefix=<Directory path> , e.g: ./configure --prefix=<Directory path>

-make

-make install

-All of the tests will appear in the  perftest directory and in the install directory.
===============================================================================
3. Notes on Testing Methodology
===============================================================================
- The benchmarks use the CPU cycle counter to get time stamps without context
  switch.  Some CPU architectures (e.g., Intel's 80486 or older PPC) do not
  have such capability.

- The latency benchmarks measure round-trip time but report half of that as one-way
  latency. This means that the results may not be accurate for asymmetrical configurations.

- On all unidirectional bandwidth benchmarks, the client measures the bandwidth.
  On bidirectional bandwidth benchmarks, each side measures the bandwidth of
  the traffic it initiates, and at the end of the measurement period, the server
  reports the result to the client, who combines them together.

- Latency tests report minimum, median and maximum latency results. 
  The median latency is typically less sensitive to high latency variations,
  compared to average latency measurement.
  Typically, the first value measured is the maximum value, due to warmup effects.

- Long sampling periods have very limited impact on measurement accuracy.
  The default value of 1000 iterations is pretty good.
  Note that the program keeps data structures with memory footprint proportional
  to the number of iterations. Setting a very high number of iteration may
  have negative impact on the measured performance which are not related to
  the devices under test.
  If a high number of iterations is strictly necessary, it is recommended to
  use the -N flag (No Peak).

- Bandwidth benchmarks may be run for a number of iterations, or for a fixed duration.
  Use the -D flag to instruct the test to run for the specified number of seconds.
  The --run_infinitely flag instructs the program to run until interrupted by
  the user, and print the measured bandwidth every 5 seconds. 

- The "-H" option in latency benchmarks dumps a histogram of the results.
  See xgraph, ygraph, r-base (http://www.r-project.org/), PSPP, or other 
  statistical analysis programs.

  *** IMPORTANT NOTE:
      When running the benchmarks over an Infiniband fabric,
      a Subnet Manager must run on the switch or on one of the
      nodes in your fabric, prior to starting the benchmarks.

Architectures tested:	i686, x86_64, ia64

===============================================================================
4. Benchmarks Description
===============================================================================

The benchmarks generate a synthetic stream of operations, which is very useful
for hardware and software benchmarking and analysis.
The benchmarks are not designed to emulate any real application traffic.
Real application traffic may be affected by many parameters, and hence
might not be predictable based only on the results of those benchmarks.

ib_send_lat 	latency test with send transactions
ib_send_bw 	bandwidth test with send transactions
ib_write_lat 	latency test with RDMA write transactions
ib_write_bw 	bandwidth test with RDMA write transactions
ib_read_lat 	latency test with RDMA read transactions
ib_read_bw 	bandwidth test with RDMA read transactions
ib_atomic_lat	latency test with atomic transactions
ib_atomic_bw 	bandwidth test with atomic transactions

Raw Ethernet interface benchmarks:
raw_ethernet_send_lat  latency test over raw Ethernet interface
raw_ethernet_send_bw   bandwidth test over raw Ethernet interface

===============================================================================
5. Running Tests
===============================================================================

Prerequisites:
	kernel 2.6
	(kernel module) matches libibverbs
	(kernel module) matches librdmacm
	(kernel module) matches libibumad
	(kernel module) matches libmath (lm)
	(linux kernel module) matches pciutils (lpci).

Server:		./<test name> <options>
Client:		./<test name> <options> <server IP address>

		o  <server address> is IPv4 or IPv6 address. You can use the IPoIB
                   address if IPoIB is configured.
		o  --help lists the available <options>

  *** IMPORTANT NOTE:
      The SAME OPTIONS must be passed to both server and client.

Common Options to all tests:
----------------------------
  -h, --help				Display this help message screen
  -p, --port=<port>			Listen on/connect to port <port> (default: 18515)
  -R, --rdma_cm				Connect QPs with rdma_cm and run test on those QPs
  -z, --comm_rdma_cm			Communicate with rdma_cm module to exchange data - use regular QPs
  -m, --mtu=<mtu>			QP Mtu size (default: active_mtu from ibv_devinfo)
  -c, --connection=<type>		Connection type RC/UC/UD/XRC/DC/SRD (default RC).
  -d, --ib-dev=<dev>			Use IB device <dev> (default: first device found)
  -i, --ib-port=<port>			Use network port <port> of IB device (default: 1)
  -s, --size=<size>			Size of message to exchange (default: 1)
  -a, --all				Run sizes from 2 till 2^23
  -n, --iters=<iters>			Number of exchanges (at least 100, default: 1000)
  -x, --gid-index=<index>		Test uses GID with GID index taken from command
  -V, --version				Display version number
  -e, --events				Sleep on CQ events (default poll)
  -F, --CPU-freq			Do not fail even if cpufreq_ondemand module
  -I, --inline_size=<size>		Max size of message to be sent in inline mode
  -u, --qp-timeout=<timeout>		QP timeout = (4 uSec)*(2^timeout) (default: 14)
  -S, --sl=<sl>				Service Level (default 0)
  -r, --rx-depth=<dep>			Receive queue depth (default 600)

Options for latency tests:
--------------------------

  -C, --report-cycles			Report times in CPU cycle units
  -H, --report-histogram		Print out all results (Default: summary only)
  -U, --report-unsorted			Print out unsorted results (default sorted)

Options for BW tests:
---------------------

  -b, --bidirectional			Measure bidirectional bandwidth (default uni)
  -N, --no peak-bw			Cancel peak-bw calculation (default with peak-bw)
  -Q, --cq-mod				Generate Cqe only after <cq-mod> completion
  -t, --tx-depth=<dep>			Size of tx queue (default: 128)
  -O, --dualport			Run test in dual-port mode (2 QPs). Both ports must be active (default OFF)
  -D, --duration=<sec> 			Run test for <sec> period of seconds
  -f, --margin=<sec> 			When in Duration, measure results within margins (default: 2)
  -l, --post_list=<list size>		Post list of send WQEs of <list size> size (instead of single post)
      --recv_post_list=<list size>	Post list of receive WQEs of <list size> size (instead of single post)
  -q, --qp=<num of qp's>		Num of QPs running in the process (default: 1)
      --run_infinitely			Run test until interrupted by user, print results every 5 seconds

SEND tests (ib_send_lat or ib_send_bw) flags: 
---------------------------------------------

  -r, --rx-depth=<dep>			Size of receive queue (default: 512 in BW test)
  -g, --mcg=<num_of_qps> 		Send messages to multicast group with <num_of_qps> qps attached to it
  -M, --MGID=<multicast_gid>		In multicast, uses <multicast_gid> as the group MGID

WRITE latency (ib_write_lat) flags:
-----------------------------------

  --write_with_imm				Use write-with-immediate verb instead of write

ATOMIC tests (ib_atomic_lat or ib_atomic_bw) flags: 
---------------------------------------------------

  -A, --atomic_type=<type>		type of atomic operation from {CMP_AND_SWAP,FETCH_AND_ADD}
  -o, --outs=<num>			Number of outstanding read/atomic requests - also on READ tests

Options for raw_ethernet_send_bw:
---------------------------------
  -B, --source_mac			source MAC address by this format XX:XX:XX:XX:XX:XX (default take the MAC address form GID)
  -E, --dest_mac			destination MAC address by this format XX:XX:XX:XX:XX:XX **MUST** be entered
  -J, --server_ip			server ip address by this format X.X.X.X (using to send packets with IP header)
  -j, --client_ip			client ip address by this format X.X.X.X (using to send packets with IP header)
  -K, --server_port			server udp port number (using to send packets with UDP header)
  -k, --client_port			client udp port number (using to send packets with UDP header)
  -Z, --server				choose server side for the current machine (--server/--client must be selected)
  -P, --client				choose client side for the current machine (--server/--client must be selected)

----------------------------------------------
Special feature detailed explanation in tests:
----------------------------------------------

  1. Usage of post_list feature (-l, --post_list=<list size> and --recv_post_list=<list size>)
     In this case, each QP will prepare <list size> WQEs (instead of 1), and will chain them to each other.
     In chaining we mean allocating <list_size> array, and setting 'next' pointer of each WQE in the array
     to point to the following element in the array. the last WQE in the array will point to NULL.
     In this case, when posting the first WQE in the list, will instruct the HW to post all of those WQEs.
     Which means each post send/recv will post <list_size> messages.
     This feature is good if we want to know the maximum message rate of QPs in a single process.
     Since we are limited to SW posts (for example, on post_send ~ 10 Mpps, since we have ~ 500 ns between
     each SW post_send), we can see the true HW message rate when setting <list_size> of 64 (for example)
     since it's not depended on SW limitations.

  2. RDMA Connected Mode (CM)
     You can add the "-R" flag to all tests to connect the QPs from each side with the rdma_cm library.
     In this case, the library will connect the QPs and will use the IPoIB interface for doing it.
     It helps when you don't have Ethernet connection between the 2 nodes.
     You must supply the IPoIB interface as the server IP.

  3. Multicast support in ib_send_lat and in ib_send_bw
     Send tests have built in feature of testing multicast performance, in verbs level.
     You can use "-g" to specify the number of QPs to attach to this multicast group.
     "-M" flag allows you to choose the multicast group address.

  4. GPUDirect usage:
     To utilize GPUDirect feature, perftest should be compiled as:
     ./autogen.sh && ./configure CUDA_H_PATH=<path to cuda.h> && make -j, e.g.:
     ./autogen.sh && ./configure CUDA_H_PATH=/usr/local/cuda/include/cuda.h && make -j

     Thus --use_cuda=<gpu_index> flag will be available to add to a command line:
     ./ib_write_bw -d ib_dev --use_cuda=<gpu index> -a

    CUDA DMA-BUF requierments:
        1) CUDA Toolkit 11.7 or later.
        2) NVIDIA Open-Source GPU Kernel Modules version 515 or later.
           installation instructions: http://us.download.nvidia.com/XFree86/Linux-x86_64/515.43.04/README/kernel_open.html
        3) Configuration / Usage:
          export the following environment variables:
            1- export LD_LIBRARY_PATH.
              e.g: export LD_LIBRARY_PATH=/usr/local/cuda/lib64:$LD_LIBRARY_PATH
            2- export LIBRARY_PATH.
              e.g: export LIBRARY_PATH=/usr/local/cuda/lib64:$LIBRARY_PATH
          perform compilation as decribe in the begining of section 4 (GPUDirect usage).

  5. AES_XTS (encryption/decryption)
     In perftest repository there are two files as follow:
      1) gen_data_enc_key.c

      2) encrypt_credentials.c

      gen_data_enc_key.c file should be compiled with the following command:
      #gcc gen_data_enc_key.c -o gen_data_enc_key -lcrypto

      encrypt_credentials.c file should be compiled with the following command:
      #gcc encrypt_credentials.c -o encrypt_credentials -lcrypto

      You must provide the plaintext credentials and the kek in seperate files in hex format.

      for example:

      credential_file:
        0x00
        0x00
        0x00
        0x00
        0x10
        etc..

      kek_file:
        0x00
        0x00
        0x11
        0x22
        0x55
        etc..

      Notes:
        1) You should run the encrypt_credentials program and give paths as parameters
        to the plaintext credential_file, kek_file and the path you want the encrypted
        credentials to be in (credentials_file first).
        for example:
          #./encrypt_credentials <PATH>/credential_file <PATH>/kek_file
          <PATH>/encrypted_credentials_file_name

        The output of this is a text file that you must provide its path
        as a parameter to the perftest application with --credentials_path <PATH>

        2)Both encrypt_credentials.c and gen_data_enc_key.c should be compiled
          before using the perftest application.

        3)gen_data_enc_key.c compiled program path must be provided to the perftest
          application with --data_enc_key_app_path <PATH> and the kek file should be
          provided with --kek_path <PATH>

        4) This feature supported only on RC qp type, and on ib_write_bw, ib_read_bw,
           ib_send_bw, ib_read_lat, ib_send_lat.

        5) You should load the kek and credentials you want to the device in the following way:
          #sudo mlxreg -d <pci address> --reg_name CRYPTO_OPERATIONAL --set "credential[0]
          =0x00000000,credential[1]=0x10000000,credential[2]=0x10000000,
          credential[3]=0x10000000,credential[4]=0x10000000,credential[5]=0x10000000
          ,credential[6]=0x10000000,credential[7]=0x10000000,credential[8]=0x10000000
          ,credential[9]=0x10000000,kek[0]=0x00001122,kek[1]=0x55556633,kek[2]=0x33447777,kek[3]=0x22337777"

  6. Payload modification
        Using the --payload_file_path you can pass a text file, which contains a pattern,
        as a parameter to perftest, and use the pattern as the payload of the RDMA verb.

        You must provide the pattern in DWORD's seperated by comma and in hex format.

        for example:
        0xddccbbaa,0xff56f00d,0xffffffff,0x21ab025b, etc...

        Notes:
          1) Perftest parse the pattern and save it in LE format.
          2) The feature available for ib_write_bw, ib_read_bw, ib_send_bw, ib_read_lat and ib_send_lat.
          3) 0 size pattern is not allow.


===============================================================================
7. Known Issues
===============================================================================

 1. Multicast support in ib_send_lat and in ib_send_bw is not stable.
    The benchmark program may hang or exhibit other unexpected behavior.

 2. Bidirectional support in ib_send_bw test, when running in UD or UC mode.
    In rare cases, the benchmark program may hang.
    perftest-2.3 release includes a feature for hang detection, which will exit test after 2 mins in those situations.

 3. Different versions of perftest may not be compatible with each other.
    Please use the same perftest version on both sides to ensure consistency of benchmark results.

 4. Test version 5.3 and above won't work with previous versions of perftest. As well as 5.70 and above.

 5. This perftest package won't compile on MLNX_OFED-2.1 due to API changes in MLNX_OFED-2.2
    In order to compile it properly, please do:
    ./configure --disable-verbs_exp
    make

 6. In the x390x platform virtualized environment the results shown by package test applications can be incorrect.

 7. perftest-2.3 release includes support for dualport VPI test - port1-Ethernet , port2-IB. (in addition to Eth:Eth, IB:IB)
    Currently, running dualport when port1-IB , port2-Ethernet still not working.

 8. If GPUDirect is not working, (e.g. you see "Couldn't allocate MR" error message), consider disabling Scatter to CQE feature. Set the environmental variable MLX5_SCATTER_TO_CQE=0. E.g.:
    MLX5_SCATTER_TO_CQE=0 ./ib_write_bw -d ib_dev --use_cuda=<gpu index> -a

