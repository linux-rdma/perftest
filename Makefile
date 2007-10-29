TESTS = write_bw_noPeak  write_lat_mQP_w write_bw_postlist send_lat send_bw write_bw read_lat read_bw write_lat
UTILS = clock_test

all: ${TESTS} ${UTILS}

CFLAGS += -Wall -g -D_GNU_SOURCE -O2 -I/usr/local/ofed/include
EXTRA_FILES = get_clock.c
EXTRA_HEADERS = get_clock.h
#The following seems to help GNU make on some platforms
LOADLIBES += 
LDFLAGS += -L/usr/local/ofed/lib64
#LDFLAGS += -L/usr/local/ofed/lib
${TESTS}: LOADLIBES += -libverbs -lrdmacm

${TESTS} ${UTILS}: %: %.c ${EXTRA_FILES} ${EXTRA_HEADERS}
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< ${EXTRA_FILES} $(LOADLIBES) $(LDLIBS) -o ib_$@
clean:
	$(foreach fname,${TESTS}, rm -f ib_${fname})
	rm -f ${UTILS}
.DELETE_ON_ERROR:
.PHONY: all clean
