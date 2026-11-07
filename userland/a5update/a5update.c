/*
 * a5update: apply file updates to the iOS root from the blackbox partition, and save logs there.
 * Runs once per boot as a launchd daemon (root). Kernel side: /dev/a5update (bsd/dev/sweet_emmc.c):
 *   read  blackbox + 0 .. 256 MiB    update image, written on the PC with fastboot flash blackbox
 *   write blackbox + 256 .. 320 MiB  log area, read on the PC with spd_dump read_part
 *
 * Update image (tools/mka5update.py), little endian:
 *   header (4096 B): "A5UPDT01", u64 serial, u32 count, u32 payload_len, u32 payload_crc32, u32 flags
 *   payload: entries {u32 op, mode, uid, gid, path_len, data_len; path; data; pad to 4}
 *     op 1 FILE    write data to path (parents created 0755), then mode/uid/gid; atomic rename
 *     op 2 DIR     mkdir -p path, mode/uid/gid
 *     op 3 RENAME  path -> data (new path)
 *     op 4 REMOVE  path, recursively
 *     op 5 SYMLINK path -> data (target)
 *     op 6 COLLECT copy path (file, or every file in a directory) into the log, newest data at most
 *                  `mode` KiB each (0 = 256), after 30 s, 120 s and 300 s
 *     op 7 KILL    send signal `mode` to every process named path (launchd restarts daemons)
 *     op 8 LIST    log a listing of directory path (every boot, before collecting)
 * An image is applied once: its serial is kept in /private/var/db/a5update.serial. COLLECT and the
 * log run every boot.
 *
 * Log area: "A5LOG001", u32 length, then text: what was applied, then "=== path (n bytes) ===" +
 * contents for every collected file. tools/a5log.py prints it. The last 4 MiB of earlier boots'
 * logs are kept in /private/var/db/a5update.history and come first.
 * Sounds (/dev/a5audio): two short high beeps = update applied, one low beep = update failed.
 */
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>

#define DEV             "/dev/a5update"
#define SERIAL_PATH     "/private/var/db/a5update.serial"
#define LOG_OFF         (256ll << 20)
#define LOG_MAX         (64u << 20)
#define MAX_PAYLOAD     (200u << 20)
#define MAX_COLLECT     32
#define HISTORY_PATH    "/private/var/db/a5update.history"
#define HISTORY_MAX     (4u << 20)

struct hdr {
	char magic[8];
	unsigned long long serial;
	unsigned int count, payload_len, payload_crc, flags;
};

static char *logbuf;
static unsigned int loglen;

static void
logf_(const char *fmt, ...)
{
	va_list ap;
	if (!logbuf || loglen >= LOG_MAX - 1024) {
		return;
	}
	va_start(ap, fmt);
	int n = vsnprintf(logbuf + loglen, LOG_MAX - loglen - 1, fmt, ap);
	va_end(ap);
	if (n > 0) {
		loglen += (unsigned)n < LOG_MAX - loglen - 1 ? (unsigned)n : LOG_MAX - loglen - 1;
	}
}

static void
log_flush(void)
{
	int fd = open(DEV, O_WRONLY);
	if (fd < 0) {
		return;
	}
	/* whole 512-byte sectors: header + text, zero padded */
	unsigned int total = 12 + loglen, padded = (total + 511) & ~511u;
	char *b = calloc(1, padded);
	if (b) {
		memcpy(b, "A5LOG001", 8);
		memcpy(b + 8, &loglen, 4);
		memcpy(b + 12, logbuf, loglen);
		(void)pwrite(fd, b, padded, LOG_OFF);
		free(b);
	}
	close(fd);
}

static unsigned int
crc32(const unsigned char *p, unsigned int n)
{
	unsigned int c = 0xffffffffu;
	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++) {
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
		}
	}
	return ~c;
}

static void
beep(unsigned int hz, unsigned int ms, int times)
{
	static const short sine16[16] = { 0, 3062, 5657, 7391, 8000, 7391, 5657, 3062,
		0, -3062, -5657, -7391, -8000, -7391, -5657, -3062 };
	int fd = open("/dev/a5audio", O_WRONLY);
	if (fd < 0) {
		return;
	}
	unsigned int n = 48 * ms, gap = 48 * 120;
	short *buf = calloc(2 * (n + gap), sizeof(short));
	if (!buf) {
		close(fd);
		return;
	}
	unsigned int ph = 0, step = hz * 16u * 65536u / 48000u;
	for (unsigned int i = 0; i < n; i++) {
		int s = sine16[(ph >> 16) & 15] * (int)(n - i) / (int)n;
		ph += step;
		buf[2 * i] = buf[2 * i + 1] = (short)s;
	}
	for (int t = 0; t < times; t++) {
		(void)write(fd, buf, 4 * (n + gap));
	}
	free(buf);
	close(fd);
}

static int
mkdirs(const char *path, mode_t mode)
{
	char tmp[1024];
	size_t n = strlen(path);
	if (n >= sizeof(tmp)) {
		return ENAMETOOLONG;
	}
	memcpy(tmp, path, n + 1);
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			if (mkdir(tmp, 0755) && errno != EEXIST) {
				return errno;
			}
			*p = '/';
		}
	}
	if (mkdir(tmp, mode) && errno != EEXIST) {
		return errno;
	}
	return 0;
}

static int
parent_dirs(const char *path)
{
	char tmp[1024];
	size_t n = strlen(path);
	if (n >= sizeof(tmp)) {
		return ENAMETOOLONG;
	}
	memcpy(tmp, path, n + 1);
	char *s = strrchr(tmp, '/');
	if (!s || s == tmp) {
		return 0;
	}
	*s = 0;
	return mkdirs(tmp, 0755);
}

static int
remove_tree(const char *path)
{
	struct stat st;
	if (lstat(path, &st)) {
		return errno == ENOENT ? 0 : errno;
	}
	if (!S_ISDIR(st.st_mode)) {
		return unlink(path) ? errno : 0;
	}
	DIR *d = opendir(path);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d))) {
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
				continue;
			}
			char sub[1024];
			snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
			remove_tree(sub);
		}
		closedir(d);
	}
	return rmdir(path) ? errno : 0;
}

static int
write_file(const char *path, const unsigned char *data, unsigned int len, mode_t mode, uid_t uid, gid_t gid)
{
	char tmp[1100];
	int e = parent_dirs(path);
	if (e) {
		return e;
	}
	snprintf(tmp, sizeof(tmp), "%s.a5tmp", path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		return errno;
	}
	unsigned int done = 0;
	while (done < len) {
		ssize_t w = write(fd, data + done, len - done);
		if (w <= 0) {
			e = errno;
			close(fd);
			unlink(tmp);
			return e ? e : EIO;
		}
		done += (unsigned int)w;
	}
	fchown(fd, uid, gid);
	fchmod(fd, mode);
	fsync(fd);
	close(fd);
	return rename(tmp, path) ? errno : 0;
}

static void
kill_named(const char *name, int sig)
{
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
	size_t len = 0;
	if (sysctl(mib, 3, NULL, &len, NULL, 0) || !len) {
		return;
	}
	len += 64 * sizeof(struct kinfo_proc);
	struct kinfo_proc *kp = malloc(len);
	if (!kp) {
		return;
	}
	if (!sysctl(mib, 3, kp, &len, NULL, 0)) {
		for (size_t i = 0; i < len / sizeof(*kp); i++) {
			if (!strncmp(kp[i].kp_proc.p_comm, name, sizeof(kp[i].kp_proc.p_comm) - 1)) {
				logf_("kill %s pid %d sig %d: %d\n", name, kp[i].kp_proc.p_pid, sig,
				    kill(kp[i].kp_proc.p_pid, sig) ? errno : 0);
			}
		}
	}
	free(kp);
}

static struct {
	char path[512];
	unsigned int max;
} collect[MAX_COLLECT];
static int ncollect;
static char lists[16][512];
static int nlists;

static void
list_dir(const char *path)
{
	DIR *d = opendir(path);
	logf_("=== ls %s%s ===\n", path, d ? "" : ": cannot open");
	if (!d) {
		return;
	}
	struct dirent *e;
	while ((e = readdir(d))) {
		char sub[1024];
		struct stat st;
		snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
		if (lstat(sub, &st)) {
			logf_("  ? %s\n", e->d_name);
		} else {
			logf_("  %06o %u:%u %10lld %s\n", st.st_mode, st.st_uid, st.st_gid, (long long)st.st_size, e->d_name);
		}
	}
	closedir(d);
}

static void
history_load(void)
{
	int fd = open(HISTORY_PATH, O_RDONLY);
	if (fd < 0) {
		return;
	}
	struct stat st;
	if (!fstat(fd, &st) && st.st_size > 0) {
		off_t start = st.st_size > HISTORY_MAX ? st.st_size - HISTORY_MAX : 0;
		ssize_t r = pread(fd, logbuf, (size_t)(st.st_size - start), start);
		if (r > 0) {
			loglen = (unsigned)r;
		}
	}
	close(fd);
}

static void
history_save(void)
{
	unsigned int start = loglen > HISTORY_MAX ? loglen - HISTORY_MAX : 0;
	int fd = open(HISTORY_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		(void)write(fd, logbuf + start, loglen - start);
		close(fd);
	}
}

static void
collect_file(const char *path, unsigned int max)
{
	struct stat st;
	if (stat(path, &st) || !S_ISREG(st.st_mode)) {
		return;
	}
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		logf_("=== %s: open %d ===\n", path, errno);
		return;
	}
	off_t start = st.st_size > (off_t)max ? st.st_size - (off_t)max : 0;
	size_t n = (size_t)(st.st_size - start);
	if (loglen + n + 1024 >= LOG_MAX) {
		logf_("=== %s (%lld bytes): no room ===\n", path, (long long)st.st_size);
		close(fd);
		return;
	}
	/* exactly n bytes follow the header (zero padded on a short read), so binary files can be cut out */
	logf_("=== %s (%lld bytes, %zu from %lld) ===\n", path, (long long)st.st_size, n, (long long)start);
	ssize_t r = pread(fd, logbuf + loglen, n, start);
	if (r < 0) {
		r = 0;
	}
	memset(logbuf + loglen + r, 0, n - (size_t)r);
	loglen += (unsigned)n;
	logf_("\n");
	close(fd);
}

static void
collect_all(int round)
{
	logf_("\n##### collect round %d #####\n", round);
	for (int i = 0; i < ncollect; i++) {
		struct stat st;
		if (stat(collect[i].path, &st)) {
			logf_("=== %s: %d ===\n", collect[i].path, errno);
			continue;
		}
		if (!S_ISDIR(st.st_mode)) {
			collect_file(collect[i].path, collect[i].max);
			continue;
		}
		DIR *d = opendir(collect[i].path);
		if (!d) {
			continue;
		}
		struct dirent *e;
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.') {
				continue;
			}
			char sub[1024];
			snprintf(sub, sizeof(sub), "%s/%s", collect[i].path, e->d_name);
			collect_file(sub, collect[i].max);
		}
		closedir(d);
	}
}

/* returns 0 ok, >0 failed entries, -1 no/bad image */
static int
apply(const unsigned char *pl, unsigned int len, unsigned int count, int dry)
{
	unsigned int off = 0, failed = 0;
	for (unsigned int i = 0; i < count; i++) {
		if (off + 24 > len) {
			logf_("entry %u: truncated\n", i);
			return -1;
		}
		unsigned int h[6];
		memcpy(h, pl + off, 24);
		unsigned int op = h[0], mode = h[1], uid = h[2], gid = h[3], plen = h[4], dlen = h[5];
		if (plen == 0 || plen >= 512 || off + 24 + plen + dlen > len) {
			logf_("entry %u: bad lengths\n", i);
			return -1;
		}
		char path[512], arg[512];
		memcpy(path, pl + off + 24, plen);
		path[plen] = 0;
		const unsigned char *data = pl + off + 24 + plen;
		off = (off + 24 + plen + dlen + 3) & ~3u;
		if (dry) {
			continue;
		}
		int e = 0;
		switch (op) {
		case 1:
			e = write_file(path, data, dlen, (mode_t)mode, uid, gid);
			break;
		case 2:
			e = mkdirs(path, (mode_t)mode);
			if (!e) {
				chown(path, uid, gid);
				chmod(path, (mode_t)mode);
			}
			break;
		case 3:
		case 5:
			if (dlen == 0 || dlen >= sizeof(arg)) {
				e = EINVAL;
				break;
			}
			memcpy(arg, data, dlen);
			arg[dlen] = 0;
			if (op == 3) {
				e = parent_dirs(arg);
				if (!e) {
					remove_tree(arg);
					e = rename(path, arg) ? errno : 0;
				}
			} else {
				e = parent_dirs(path);
				unlink(path);
				if (!e) {
					e = symlink(arg, path) ? errno : 0;
				}
			}
			break;
		case 4:
			e = remove_tree(path);
			break;
		case 6:
			if (ncollect < MAX_COLLECT) {
				strlcpy(collect[ncollect].path, path, sizeof(collect[ncollect].path));
				collect[ncollect].max = (mode ? mode : 256) * 1024;
				ncollect++;
			}
			continue;
		case 7:
			continue;               /* after everything else */
		case 8:
			if (nlists < 16) {
				strlcpy(lists[nlists++], path, sizeof(lists[0]));
			}
			continue;
		default:
			e = EINVAL;
		}
		logf_("op %u %s%s%s: %s\n", op, path, (op == 3 || op == 5) ? " -> " : "", (op == 3 || op == 5) ? arg : "",
		    e ? strerror(e) : "ok");
		failed += e != 0;
	}
	return (int)failed;
}

static void
kill_ops(const unsigned char *pl, unsigned int len, unsigned int count)
{
	unsigned int off = 0;
	for (unsigned int i = 0; i < count && off + 24 <= len; i++) {
		unsigned int h[6];
		memcpy(h, pl + off, 24);
		if (h[4] == 0 || h[4] >= 512 || off + 24 + h[4] + h[5] > len) {
			return;
		}
		if (h[0] == 7) {
			char name[512];
			memcpy(name, pl + off + 24, h[4]);
			name[h[4]] = 0;
			kill_named(name, h[1] ? (int)h[1] : SIGTERM);
		}
		off = (off + 24 + h[4] + h[5] + 3) & ~3u;
	}
}

int
main(void)
{
	logbuf = malloc(LOG_MAX);
	if (!logbuf) {
		return 1;
	}
	history_load();
	logf_("\n######## boot: a5update pid %d, time %ld ########\n", getpid(), (long)time(NULL));
	int fd = open(DEV, O_RDONLY);
	if (fd < 0) {
		logf_("open %s: %s\n", DEV, strerror(errno));
		return 1;
	}
	struct hdr h;
	unsigned char *pl = NULL;
	int result = -1;
	if (pread(fd, &h, sizeof(h), 0) == (ssize_t)sizeof(h) && !memcmp(h.magic, "A5UPDT01", 8) &&
	    h.payload_len <= MAX_PAYLOAD && (pl = malloc(h.payload_len ? h.payload_len : 1)) &&
	    pread(fd, pl, h.payload_len, 4096) == (ssize_t)h.payload_len) {
		unsigned long long done = 0;
		int sf = open(SERIAL_PATH, O_RDONLY);
		if (sf >= 0) {
			(void)read(sf, &done, sizeof(done));
			close(sf);
		}
		logf_("image serial %llu (applied before: %llu), %u entries, %u bytes\n", h.serial, done, h.count, h.payload_len);
		if (crc32(pl, h.payload_len) != h.payload_crc) {
			logf_("payload CRC mismatch: not applied\n");
		} else if (apply(pl, h.payload_len, h.count, 1) < 0) {
			logf_("malformed payload: not applied\n");
		} else if (h.serial == done) {
			/* already applied: only register the COLLECT entries */
			unsigned int off = 0;
			for (unsigned int i = 0; i < h.count; i++) {
				unsigned int e[6];
				memcpy(e, pl + off, 24);
				if (e[0] == 8 && nlists < 16 && e[4] < 512) {
					memcpy(lists[nlists], pl + off + 24, e[4]);
					lists[nlists++][e[4]] = 0;
				}
				if (e[0] == 6 && ncollect < MAX_COLLECT) {
					memcpy(collect[ncollect].path, pl + off + 24, e[4]);
					collect[ncollect].path[e[4]] = 0;
					collect[ncollect].max = (e[1] ? e[1] : 256) * 1024;
					ncollect++;
				}
				off = (off + 24 + e[4] + e[5] + 3) & ~3u;
			}
			logf_("already applied\n");
		} else {
			result = apply(pl, h.payload_len, h.count, 0);
			sf = open(SERIAL_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (sf >= 0) {
				(void)write(sf, &h.serial, sizeof(h.serial));
				fsync(sf);
				close(sf);
			}
			logf_("applied: %d failed\n", result);
			sync();
			kill_ops(pl, h.payload_len, h.count);
			beep(result == 0 ? 1320 : 330, result == 0 ? 90 : 500, result == 0 ? 2 : 1);
		}
	} else {
		logf_("no update image\n");
	}
	close(fd);
	for (int i = 0; i < nlists; i++) {
		list_dir(lists[i]);
	}
	log_flush();
	static const unsigned int waits[3] = { 30, 90, 180 };
	for (int r = 0; r < 3 && ncollect; r++) {
		sleep(waits[r]);
		collect_all(r + 1);
		log_flush();
	}
	history_save();
	return 0;
}
