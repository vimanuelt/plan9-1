#include	<u.h>
#include	<libc.h>

int
getpid(void)
{
	char b[20];
	int f;

	memset(b, 0, sizeof(b));
	f = open("#c/pid", 0);
	if(f >= 0) {
		read(f, b, sizeof(b));
		close(f);
	}
	return atol(b);
}
