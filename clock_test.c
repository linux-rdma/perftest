#include <unistd.h>
#include <stdio.h>
#include "get_clock.h"

int main()
{
	int no_cpu_freq_fail = 0;
	double mhz;
	mhz = get_cpu_mhz(no_cpu_freq_fail);
	cycles_t c1, c2;

	if (!mhz) {
		printf("Unable to calibrate cycles. Exiting.\n");
		return 2;
	}

	printf("Type CTRL-C to cancel.\n");
	for(;;)
	{
		c1 = get_cycles();
		sleep(1);
		c2 = get_cycles();
		printf("1 sec = %g usec\n", (c2 - c1) / mhz);
	}
}
