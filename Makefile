RDMACM_TESTS = rdma_lat rdma_bw
MCAST_TESTS = send_bw send_lat
TESTS = write_bw_postlist write_lat write_bw read_lat read_bw
UTILS = clock_test

all: ${RDMACM_TESTS} ${MCAST_TESTS} ${TESTS} ${UTILS}

CFLAGS += -Wall -g -D_GNU_SOURCE -O2
BASIC_FILES = get_clock.c
EXTRA_FILES = perftest_resources.c
MCAST_FILES = multicast_resources.c
BASIC_HEADERS = get_clock.h
EXTRA_HEADERS = perftest_resources.h
MCAST_HEADERS = multicast_resources.h
#The following seems to help GNU make on some platforms
LOADLIBES += 
LDFLAGS +=

${RDMACM_TESTS}: LOADLIBES += -libverbs -lrdmacm
${MCAST_TESTS}: LOADLIBES += -libverbs -libumad -lm
${TESTS} ${UTILS}: LOADLIBES += -libverbs

${RDMACM_TESTS}: %: %.c ${BASIC_FILES} ${BASIC_HEADERS}
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< ${BASIC_FILES} $(LOADLIBES) $(LDLIBS) -o $@
${MCAST_TESTS}: %: %.c ${BASIC_FILES} ${EXTRA_FILES} ${MCAST_FILES} ${BASIC_HEADERS} ${EXTRA_HEADERS} ${MCAST_HEADERS}
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< ${BASIC_FILES} ${EXTRA_FILES} ${MCAST_FILES} $(LOADLIBES) $(LDLIBS) -o ib_$@
${TESTS} ${UTILS}: %: %.c ${BASIC_FILES} ${EXTRA_FILES} ${BASIC_HEADERS} ${EXTRA_HEADERS}
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< ${BASIC_FILES} ${EXTRA_FILES} $(LOADLIBES) $(LDLIBS) -o ib_$@

clean:
	$(foreach fname,${RDMACM_TESTS}, rm -f ${fname})
	$(foreach fname,${MCAST_TESTS}, rm -f ib_${fname})
	$(foreach fname,${TESTS} ${UTILS}, rm -f ib_${fname})
.DELETE_ON_ERROR:
.PHONY: all clean
