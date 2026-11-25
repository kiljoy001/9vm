/*
 * hostfs against a guest that sends what it likes: names that would leave
 * the export, walks longer than walk(5) allows, reply buffers too small for
 * a reply, counts larger than their message, and short messages.  Also the
 * wstat rename the guest's mv relies on, and a reused fid number.
 *
 *	make test_hostfs && ./test_hostfs
 *
 * Built with AddressSanitizer and UndefinedBehaviorSanitizer, so a read or
 * write outside a buffer fails the run.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "hostfs.h"

enum {
	Tversion = 100, Tattach = 104, Rerror = 107, Twalk = 110, Rwalk = 111,
	Topen = 112, Ropen = 113, Tcreate = 114, Rcreate = 115, Tread = 116, Rread = 117,
	Twrite = 118, Rwrite = 119, Tclunk = 120, Twstat = 126, Rwstat = 127,
};

static struct hostfs h;
static uint8_t req[70000], resp[70000];
static uint32_t len;
static int failures;
static char top[PATH_MAX], root[PATH_MAX];

static void
check(int ok, const char *what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if(!ok)
		failures++;
}

static void begin(int type) { len = 7; req[4] = type; req[5] = 1; req[6] = 0; }
static void p8(uint8_t v) { req[len++] = v; }
static void p16(uint16_t v) { memcpy(req + len, &v, 2); len += 2; }
static void p32(uint32_t v) { memcpy(req + len, &v, 4); len += 4; }
static void p64(uint64_t v) { memcpy(req + len, &v, 8); len += 8; }
static void ps(const char *s) { p16(strlen(s)); memcpy(req + len, s, strlen(s)); len += strlen(s); }

static uint32_t
send(void)
{
	memcpy(req, &len, 4);
	return hostfs_dispatch(&h, req, len, resp, sizeof resp);
}

static uint32_t
r32(int off)
{
	uint32_t v;

	memcpy(&v, resp + off, 4);
	return v;
}

static int
walk(uint32_t fid, uint32_t newfid, int n, const char **names)
{
	begin(Twalk);
	p32(fid);
	p32(newfid);
	p16(n);
	for(int i = 0; i < n; i++)
		ps(names[i]);
	send();
	return resp[4] == Rwalk ? resp[7] : -1;
}

static int
openfid(uint32_t fid, int mode)
{
	begin(Topen);
	p32(fid);
	p8(mode);
	send();
	return resp[4] == Ropen;
}

static int
readfid(uint32_t fid, char *out, uint32_t count)
{
	uint32_t n;

	begin(Tread);
	p32(fid);
	p64(0);
	p32(count);
	send();
	if(resp[4] != Rread)
		return -1;
	n = r32(7);
	memcpy(out, resp + 11, n);
	out[n] = '\0';
	return n;
}

static void
clunk(uint32_t fid)
{
	begin(Tclunk);
	p32(fid);
	send();
}

static void
put(const char *dir, const char *name, const char *text)
{
	char path[PATH_MAX];
	int fd;

	snprintf(path, sizeof path, "%s/%s", dir, name);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if(fd < 0 || write(fd, text, strlen(text)) != (ssize_t)strlen(text)){
		perror(path);
		exit(1);
	}
	close(fd);
}

static int
exists(const char *dir, const char *name)
{
	char path[PATH_MAX];
	struct stat st;

	snprintf(path, sizeof path, "%s/%s", dir, name);
	return stat(path, &st) == 0;
}

int
main(void)
{
	char text[256], outside[PATH_MAX];
	const char *names[HOSTFS_MAXWELEM + 1];

	snprintf(top, sizeof top, "/tmp/test_hostfs.%d", (int)getpid());
	snprintf(root, sizeof root, "%s/root", top);
	snprintf(outside, sizeof outside, "%s/outside", top);
	mkdir(top, 0700);
	mkdir(root, 0700);
	mkdir(outside, 0700);
	snprintf(text, sizeof text, "%s/sub", root);
	mkdir(text, 0700);
	put(outside, "secret", "secret\n");
	put(root, "file", "inside\n");

	hostfs_init(&h, root);
	begin(Tversion);
	p32(8192);
	ps("9P2000");
	send();
	begin(Tattach);
	p32(0);
	p32(~0U);
	ps("guest");
	ps("");
	send();

	/* Names that would leave the export. */
	names[0] = "../outside/secret";
	check(walk(0, 1, 1, names) == 0, "a walk element holding '/' is refused");
	names[0] = "sub/../../outside";
	check(walk(0, 1, 1, names) == 0, "a walk element climbing through a subdirectory is refused");
	names[0] = "..";
	names[1] = "..";
	names[2] = "outside";
	check(walk(0, 1, 3, names) <= 2, "'..' stops at the export root");
	names[0] = "file";
	check(walk(0, 1, 1, names) == 1 && openfid(1, 0) &&
	      readfid(1, text, sizeof text - 1) == 7 && strcmp(text, "inside\n") == 0,
	      "a file inside the export is read");
	clunk(1);

	begin(Tcreate);
	p32(0);
	ps("../outside/planted");
	p32(0644);
	p8(1);
	send();
	check(resp[4] == Rerror && !exists(outside, "planted"),
	      "create with a '/' in the name is refused");
	begin(Tcreate);
	p32(0);
	ps("..");
	p32(0x80000000U | 0755);
	p8(0);
	send();
	check(resp[4] == Rerror, "create named '..' is refused");

	/* Walks longer than walk(5) allows. */
	for(int i = 0; i <= HOSTFS_MAXWELEM; i++)
		names[i] = ".";
	begin(Twalk);
	p32(0);
	p32(2);
	p16(40);
	for(int i = 0; i < 40; i++)
		ps(".");
	send();
	check(resp[4] == Rerror, "a walk of 40 elements is refused");
	check(walk(0, 2, HOSTFS_MAXWELEM, names) == HOSTFS_MAXWELEM, "a walk of 16 elements is answered");
	clunk(2);

	/* Reply buffers too small for a reply. */
	names[0] = "file";
	walk(0, 3, 1, names);
	openfid(3, 0);
	begin(Tread);
	p32(3);
	p64(0);
	p32(4096);
	memcpy(req, &len, 4);
	{
		uint8_t *small = malloc(8);

		check(hostfs_dispatch(&h, req, len, small, 8) == 0,
		      "a read into an 8-byte reply buffer gets no reply");
		free(small);
	}

	/* Counts larger than their message, and short messages. */
	clunk(3);
	walk(0, 3, 1, names);
	openfid(3, 2);
	begin(Twrite);
	p32(3);
	p64(0);
	p32(0xFFFFFFF0U);
	p8('x');
	send();
	check(resp[4] == Rwrite && r32(7) == 0, "a write whose count exceeds its message writes nothing");
	begin(Twrite);
	p32(3);
	p64(0);
	p32(8);
	memcpy(req + len, "written\n", 8);
	len += 8;
	send();
	check(resp[4] == Rwrite && r32(7) == 8, "a write within its message is written");
	clunk(3);

	begin(Tread);
	p32(0);
	send();
	check(resp[4] == Rerror, "a read too short for its fields is refused");
	begin(Tcreate);
	p32(0);
	p16(200);
	memcpy(req + len, "abc", 3);
	len += 3;
	send();
	check(resp[4] == Rerror, "a create whose name runs past the message is refused");

	/* wstat renames, as mv in one directory does. */
	names[0] = "file";
	walk(0, 4, 1, names);
	begin(Twstat);
	p32(4);
	{
		uint32_t at = len;

		p16(0);
		p16(0);		/* size, filled below */
		p16(~0);
		p32(~0U);
		p8(0xFF);
		p32(~0U);
		p64(~0ULL);
		p32(~0U);
		p32(~0U);
		p32(~0U);
		p64(~0ULL);
		ps("renamed");
		ps("");
		ps("");
		ps("");
		{
			uint16_t n = len - at - 2, size = len - at - 4;

			memcpy(req + at, &n, 2);
			memcpy(req + at + 2, &size, 2);
		}
	}
	send();
	check(resp[4] == Rwstat && exists(root, "renamed") && !exists(root, "file"),
	      "wstat with a new name renames the file");
	check(openfid(4, 0) && readfid(4, text, sizeof text - 1) == 8 &&
	      strcmp(text, "written\n") == 0, "the fid follows the renamed file");
	clunk(4);

	/* A fid number reused before it was clunked starts afresh. */
	check(openfid(0, 0), "the root opens as a directory");
	names[0] = "renamed";
	walk(0, 6, 0, NULL);
	openfid(6, 0);
	walk(0, 6, 1, names);
	check(openfid(6, 0) && readfid(6, text, sizeof text - 1) == 8 &&
	      strcmp(text, "written\n") == 0,
	      "a reused fid reads its new file, not its old directory");

	hostfs_free(&h);
	snprintf(text, sizeof text, "rm -rf %s", top);
	if(system(text) != 0)
		fprintf(stderr, "could not remove %s\n", top);
	printf("%s\n", failures ? "FAIL" : "PASS");
	return failures != 0;
}
