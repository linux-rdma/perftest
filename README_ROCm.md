# How to build
```
$ ./autogen.sh
$ ./configure --prefix=$PWD/install --enable-rocm --with-rocm=/opt/rocm
$ make && make install
```

# How to run

## Running Host to Host
```
$ numactl -C 1 ./ib_send_bw -a -F            #on node 1
$ numactl -C 1 ./ib_send_bw <node1> -a -F    #on node 2
```

## Running GPU#0 to GPU#0
```
$ numactl -C 1 ./ib_send_bw -a -F --use_rocm=0            #on node 1
$ numactl -C 1 ./ib_send_bw <node1> -a -F --use_rocm=0    #on node 2
```

# Tips and Tricks
 * Specify a CPU core on the same NUMA node as the HCA
 * Lock CPU core speed to the highest frequency
 * Try disabling the inline feature (-I 0) for better performance
 * For GPU-GPU transfers, send is usually faster than write
 * If `--use_rocm` is specified without any additional parameters,
   The first visible device (GPU#0) is selected.
 * Visibility of devices can be controlled by setting the variables
   `HIP_VISIBLE_DEVICES` or `ROCR_VISIBLE_DEVICES`.
