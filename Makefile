TESTS = rdma_lat rdma_bw send_lat send_bw write_lat write_bw read_lat read_bw
UTILS = clock_test

all: ${TESTS} ${UTILS}

CFLAGS += -Wall -g -D_GNU_SOURCE 
EXTRA_FILES = get_clock.c
EXTRA_HEADERS = get_clock.h
LOADLIBES += 

${TESTS}: LOADLIBES += -libverbs

${TESTS} ${UTILS}: %: %.c ${EXTRA_FILES} ${EXTRA_HEADERS}
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< ${EXTRA_FILES} $(LOADLIBES) $(LDLIBS) -o $@
clean:
	rm -f ${TESTS} ${UTILS}
.DELETE_ON_ERROR:
.PHONY: all clean
