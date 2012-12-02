MCAST_TESTS = send_bw send_lat
RAW_ETH_TESTS = raw_ethernet_send_bw
TESTS = write_lat write_bw read_lat read_bw atomic_lat atomic_bw
UTILS = clock_test

all: ${MCAST_TESTS} ${RAW_ETH_TESTS} ${TESTS} ${UTILS}

CFLAGS += -Wall -g -D_GNU_SOURCE -O0
BASIC_FILES = get_clock.c
EXTRA_FILES = perftest_resources.c perftest_communication.c perftest_parameters.c
MCAST_FILES = multicast_resources.c
BASIC_HEADERS = get_clock.h
EXTRA_HEADERS = perftest_resources.h perftest_communication.h perftest_parameters.h
MCAST_HEADERS = multicast_resources.h
#The following seems to help GNU make on some platforms
LOADLIBES += -libverbs -lrdmacm
LDFLAGS +=

${MCAST_TESTS}: LOADLIBES += -libumad -lm

${MCAST_TESTS}: %: %.c ${BASIC_FILES} ${EXTRA_FILES} ${MCAST_FILES} ${BASIC_HEADERS} ${EXTRA_HEADERS} ${MCAST_HEADERS}
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< ${BASIC_FILES} ${EXTRA_FILES} ${MCAST_FILES} $(LOADLIBES) $(LDLIBS) -o ib_$@
${TESTS} ${UTILS}: %: %.c ${BASIC_FILES} ${EXTRA_FILES} ${BASIC_HEADERS} ${EXTRA_HEADERS}
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< ${BASIC_FILES} ${EXTRA_FILES} $(LOADLIBES) $(LDLIBS) -o ib_$@
${RAW_ETH_TESTS}: %: %.c ${BASIC_FILES} ${EXTRA_FILES} ${BASIC_HEADERS} ${EXTRA_HEADERS}
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< ${BASIC_FILES} ${EXTRA_FILES} $(LOADLIBES) $(LDLIBS) -o raw_ethernet_bw

clean:
	$(foreach fname,${MCAST_TESTS}, rm -f ib_${fname})
	$(foreach fname,${RAW_ETH_TESTS}, rm -f raw_ethernet_bw)
	$(foreach fname,${TESTS} ${UTILS}, rm -f ib_${fname})
.DELETE_ON_ERROR:
.PHONY: all clean
