TESTS = rdma_lat rdma_bw send_lat send_bw write_lat write_bw read_lat read_bw \
 clock_test

all: ${TESTS}

CFLAGS += -Wall -g -D_GNU_SOURCE 
LOADLIBES += -libverbs
EXTRA_FILES = get_clock.c


${TESTS}: ${EXTRA_FILES}
clean:
	rm -f ${TESTS}
.DELETE_ON_ERROR:
.PHONY: all clean
