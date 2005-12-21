TESTS = rdma_lat rdma_bw send_lat send_bw write_lat write_bw read_lat read_bw

all: ${TESTS}

CFLAGS += -Wall -O2 -g -D_GNU_SOURCE 
LOADLIBES += -libverbs
EXTRA_FILES = get_clock.c


${TESTS}: ${EXTRA_FILES}
clean:
	rm -f ${TESTS}
.DELETE_ON_ERROR:
.PHONY: all clean
