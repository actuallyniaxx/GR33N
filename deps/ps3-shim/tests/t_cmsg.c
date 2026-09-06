/* GR33N - tests the macros ps3-shim adds when the console doesn't bring
 * them: the CMSG_* family and timercmp/timeradd/timersub.
 *
 * It compiles against a FAKE console (tests/console) that has struct
 * cmsghdr, struct msghdr and struct timeval but NONE of the macros. That's
 * the situation we believe exists on the PS3, and the one the PC's glibc
 * hides, because there they really are present.
 *
 * Runs on the PC, natively. It does not test the PS3: it tests that the
 * macros' arithmetic is BSD's, and that walking a list of ancillary
 * messages with them doesn't run off the end of the buffer.
 */

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <errno.h>

static int fallos;

#define OK(cond, ...) do { \
	if (cond) { \
		printf("   ok    "); \
	} else { \
		printf("   FAIL  "); \
		fallos++; \
	} \
	printf(__VA_ARGS__); \
	printf("\n"); \
} while (0)

int main(void)
{
	unsigned char buf[256];
	struct msghdr msg;
	struct cmsghdr *c;
	size_t cab = CMSG_ALIGN(sizeof(struct cmsghdr));
	int cuantos;
	unsigned int datos_a, datos_b;

	printf("== who provides the macros ==\n");
#ifdef GR33N_CMSG_PUESTAS_AQUI
	printf("   CMSG_*: ps3-shim (which is what we want to test)\n");
#else
	printf("   CMSG_*: the fake console -- this test is WORTHLESS\n");
	return 1;
#endif
#ifdef GR33N_TIMERCMP_PUESTA_AQUI
	printf("   timer*: ps3-shim\n");
#else
	printf("   timer*: the fake console -- this test is WORTHLESS\n");
	return 1;
#endif

	printf("\n== arithmetic ==\n");

	/* The alignment is BSD's: to the size of a long. */
	OK(CMSG_ALIGN(0) == 0, "CMSG_ALIGN(0) = %u", (unsigned)CMSG_ALIGN(0));
	OK(CMSG_ALIGN(1) == sizeof(long), "CMSG_ALIGN(1) = %u (long is %u)",
	   (unsigned)CMSG_ALIGN(1), (unsigned)sizeof(long));
	OK(CMSG_ALIGN(sizeof(long)) == sizeof(long),
	   "CMSG_ALIGN(sizeof long) doesn't grow");

	/* LEN doesn't align the data; SPACE does. That is the whole
	 * difference between the two, and mixing them up is the classic
	 * mistake: allocating with LEN leaves the next header with no room
	 * for its padding. */
	OK(CMSG_LEN(4) == cab + 4, "CMSG_LEN(4) = %u", (unsigned)CMSG_LEN(4));
	OK(CMSG_SPACE(4) == cab + CMSG_ALIGN(4), "CMSG_SPACE(4) = %u",
	   (unsigned)CMSG_SPACE(4));
	OK(CMSG_SPACE(4) >= CMSG_LEN(4), "SPACE is never smaller than LEN");

	/* CMSG_DATA lands right behind the aligned header. */
	memset(buf, 0, sizeof(buf));
	c = (struct cmsghdr *)buf;
	OK((unsigned char *)CMSG_DATA(c) == buf + cab,
	   "CMSG_DATA points %u bytes past the header",
	   (unsigned)((unsigned char *)CMSG_DATA(c) - buf));

	printf("\n== walking a list of two ==\n");

	/* Two ancillary messages are built the way sctp_indata.c builds them,
	 * and walked the way sctp_output.c walks them. */
	memset(buf, 0xAA, sizeof(buf));
	memset(&msg, 0, sizeof(msg));
	msg.msg_control = buf;
	msg.msg_controllen = (socklen_t)(CMSG_SPACE(4) + CMSG_SPACE(12));

	c = CMSG_FIRSTHDR(&msg);
	OK(c == (struct cmsghdr *)buf, "the first one is the start of the buffer");
	c->cmsg_len = (socklen_t)CMSG_LEN(4);
	c->cmsg_level = 132;   /* IPPROTO_SCTP */
	c->cmsg_type = 1;
	datos_a = 0x11223344u;
	memcpy(CMSG_DATA(c), &datos_a, 4);

	c = (struct cmsghdr *)(buf + CMSG_SPACE(4));
	c->cmsg_len = (socklen_t)CMSG_LEN(12);
	c->cmsg_level = 132;
	c->cmsg_type = 2;
	datos_b = 0x55667788u;
	memcpy(CMSG_DATA(c), &datos_b, 4);

	cuantos = 0;
	for (c = CMSG_FIRSTHDR(&msg); c != NULL; c = CMSG_NXTHDR(&msg, c)) {
		unsigned char *fin = (unsigned char *)c + c->cmsg_len;
		OK(fin <= buf + msg.msg_controllen,
		   "message %d fits entirely inside the buffer", cuantos);
		cuantos++;
		if (cuantos > 8) {
			printf("   FAIL  CMSG_NXTHDR never terminates\n");
			fallos++;
			break;
		}
	}
	OK(cuantos == 2, "%d messages come out (2 went in)", cuantos);

	/* And that it does NOT run past the end: with the buffer exactly
	 * sized, the second NXTHDR has to return NULL and not an address
	 * beyond it. */
	msg.msg_controllen = (socklen_t)CMSG_SPACE(4);
	c = CMSG_FIRSTHDR(&msg);
	c->cmsg_len = (socklen_t)CMSG_LEN(4);
	OK(CMSG_NXTHDR(&msg, c) == NULL,
	   "with room for one only, it doesn't invent a second");

	/* And with the buffer shorter than a header, not even the first. */
	msg.msg_controllen = (socklen_t)(sizeof(struct cmsghdr) - 1);
	OK(CMSG_FIRSTHDR(&msg) == NULL,
	   "with no room even for the header, FIRSTHDR gives NULL");

	printf("\n== timercmp / timeradd / timersub ==\n");
	{
		struct timeval a, b, r;

		a.tv_sec = 10; a.tv_usec = 500000;
		b.tv_sec = 10; b.tv_usec = 500001;
		OK(timercmp(&a, &b, <), "10.500000 < 10.500001");
		OK(!timercmp(&a, &b, >), "and it isn't greater");
		b.tv_sec = 9;
		OK(timercmp(&a, &b, >), "10.5 > 9.500001 (the seconds win)");

		/* The carry, which is where you get it wrong writing these
		 * from memory. */
		a.tv_sec = 1; a.tv_usec = 800000;
		b.tv_sec = 2; b.tv_usec = 300000;
		timeradd(&a, &b, &r);
		OK(r.tv_sec == 4 && r.tv_usec == 100000,
		   "1.8 + 2.3 = %ld.%06ld", r.tv_sec, r.tv_usec);

		a.tv_sec = 4; a.tv_usec = 100000;
		b.tv_sec = 2; b.tv_usec = 300000;
		timersub(&a, &b, &r);
		OK(r.tv_sec == 1 && r.tv_usec == 800000,
		   "4.1 - 2.3 = %ld.%06ld", r.tv_sec, r.tv_usec);

		/* And the case sctp_timer.c:534 depends on, which right after
		 * the timersub checks `tv_sec < 0 || tv_usec < 0`: a negative
		 * result has to come out with the seconds negative and the
		 * microseconds normalised. */
		a.tv_sec = 1; a.tv_usec = 0;
		b.tv_sec = 3; b.tv_usec = 500000;
		timersub(&a, &b, &r);
		OK(r.tv_sec < 0, "1.0 - 3.5 leaves the seconds negative (%ld.%06ld)",
		   r.tv_sec, r.tv_usec);
		OK(r.tv_usec >= 0 && r.tv_usec < 1000000,
		   "and the microseconds normalised");
	}

	printf("\n== the loose constants ==\n");
	OK(SOCK_SEQPACKET != SOCK_STREAM && SOCK_SEQPACKET != SOCK_DGRAM,
	   "SOCK_SEQPACKET (%d) doesn't clash with STREAM (%d) or DGRAM (%d)",
	   SOCK_SEQPACKET, SOCK_STREAM, SOCK_DGRAM);
	OK(IPPORT_RESERVED > 0, "IPPORT_RESERVED = %d", IPPORT_RESERVED);
	OK(ERESTART < 0, "ERESTART = %d (negative, clashes with no errno)",
	   ERESTART);
	OK(ERESTART != EINTR && ERESTART != EWOULDBLOCK,
	   "and differs from EINTR and EWOULDBLOCK, which sit next to it");

	printf("\n%s (%d failures)\n", fallos ? "THERE ARE FAILURES" : "all good",
	       fallos);
	return fallos != 0;
}
