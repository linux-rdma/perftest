RDMACM_TESTS = rdma_lat rdma_bw
TESTS = write_bw_postlist send_lat send_bw write_lat write_bw read_lat read_bw
UTILS = clock_test

all: ${RDMACM_TESTS} ${TESTS} ${UTILS}

CFLAGS += -Wall -g -D_GNU_SOURCE -O2
EXTRA_FILES = get_clock.c
EXTRA_HEADERS = get_clock.h
#The following seems to help GNU make on some platforms
LOADLIBES += 
LDFLAGS +=

${RDMACM_TESTS}: LOADLIBES += -libverbs -lrdmacm
${TESTS} ${UTILS}: LOADLIBES += -libverbs

${RDMACM_TESTS}: %: %.c ${EXTRA_FILES} ${EXTRA_HEADERS}
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< ${EXTRA_FILES} $(LOADLIBES) $(LDLIBS) -o $@
${TESTS} ${UTILS}: %: %.c ${EXTRA_FILES} ${EXTRA_HEADERS}
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< ${EXTRA_FILES} $(LOADLIBES) $(LDLIBS) -o ib_$@

clean:
	$(foreach fname,${RDMACM_TESTS}, rm -f ${fname})
	$(foreach fname,${TESTS} ${UTILS}, rm -f ib_${fname})
.DELETE_ON_ERROR:
.PHONY: all clean
