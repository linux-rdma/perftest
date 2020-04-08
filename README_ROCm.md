# How to build
```
$ ./autogen.sh
$ ./configure --prefix=$PWD/install --enable-rocm --with-rocm=/opt/rocm
$ make && make install
```

# How to run
```
$ HIP_VISIBLE_DEVICES=0 numactl -C 1 ./ib_send_bw -a -F --use_rocm            #on node 1
$ HIP_VISIBLE_DEVICES=0 numactl -C 1 ./ib_send_bw <node1> -a -F --use_rocm    #on node 2
```

# Tips and Tricks
 * Specify a CPU core on the same NUMA node as the HCA
 * Try disabling the inline feature (-I 0) for better performance
 * For GPU-GPU transfers, send is usually faster than write
 * By default, GPU#0 is selected. This can be changed by setting
   `HIP_VISIBLE_DEVICES` or `ROCR_VISIBLE_DEVICES` to the appropriate
   GPU id.
