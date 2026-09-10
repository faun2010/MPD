/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Compatibility layer for running a GCC 12 libstdc++ on glibc 2.31.
 *
 * MPD 0.25 needs a C++23 toolchain, but Raspbian Bullseye ships glibc 2.31 and
 * GCC 10.  The build therefore links the newer libstdc++ statically.  Exactly
 * two of the symbols that libstdc++ references were added to glibc after 2.31,
 * so they are defined here; the full set was enumerated from libstdc++.a,
 * libsupc++.a, libgcc.a and libgcc_eh.a against the Bullseye runtime.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

/* glibc 2.32.  libstdc++ reads this as a hint to skip atomic reference
 * counting.  Zero means "the process may be multi-threaded", so every atomic
 * path is taken; that is the conservative value and always correct. */
char __libc_single_threaded = 0;

static void
fill_random(void *buffer, size_t length)
{
	unsigned char *out = buffer;
	while (length > 0) {
		ssize_t n = getrandom(out, length, 0);
		if (n > 0) {
			out += (size_t)n;
			length -= (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		break;
	}
	if (length == 0)
		return;

	/* getrandom() is unavailable or refused; fall back to the device. */
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	while (fd >= 0 && length > 0) {
		ssize_t n = read(fd, out, length);
		if (n > 0) {
			out += (size_t)n;
			length -= (size_t)n;
		} else if (!(n < 0 && errno == EINTR))
			break;
	}
	if (fd >= 0)
		close(fd);

	/* glibc's arc4random cannot fail, so neither may this. */
	if (length > 0)
		abort();
}

/* glibc 2.36.  Referenced by libstdc++'s std::random_device backend. */
uint32_t
arc4random(void)
{
	uint32_t value;
	fill_random(&value, sizeof(value));
	return value;
}
