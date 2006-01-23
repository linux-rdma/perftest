#include <unistd.h>
#include <stdio.h>
#include "get_clock.h"

int main()
{
	double mhz;
	mhz = get_cpu_mhz();
	cycles_t c1, c2;
	for(;;)
	{
		c1 = get_cycles();
		sleep(1);
		c2 = get_cycles();
		printf("1 sec = %g usec\n", (c2 - c1) / mhz);
	}
}
