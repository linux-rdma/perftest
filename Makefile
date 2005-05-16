TESTS = rdma_lat

all: ${TESTS}

CFLAGS += -Wall -O2 -g -D_GNU_SOURCE 
LOADLIBES += -libverbs
EXTRA_FILES = get_clock.c


${TESTS}: ${EXTRA_FILES}
clean:
	rm -f ${TESTS}
.DELETE_ON_ERROR:
.INTERMEDIATE: ${EXTRA_FILES}
.PHONY: all clean
