/*
 * Call spy for VirtualAudio (debug aid, a5audio.conf "spy=1"): when VirtualAudio.plugin is loaded
 * into mediaserverd, the pointers to the CoreAudio HAL client calls in its data segments (GOT) are
 * replaced with wrappers that log object, selector, scope, element and result to
 * /private/var/mobile/Library/Logs/a5/vaspy.log. Shows exactly which request VirtualAudio makes
 * when it rejects the A5 device. arm64 (no pointer authentication on A11).
 */
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <sys/mman.h>

typedef OSStatus (*GetDataFn)(AudioObjectID, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32 *, void *);
typedef OSStatus (*GetSizeFn)(AudioObjectID, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32 *);
typedef OSStatus (*SetDataFn)(AudioObjectID, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32, const void *);
typedef Boolean (*HasFn)(AudioObjectID, const AudioObjectPropertyAddress *);
typedef OSStatus (*SettableFn)(AudioObjectID, const AudioObjectPropertyAddress *, Boolean *);
typedef OSStatus (*ListenerFn)(AudioObjectID, const AudioObjectPropertyAddress *, void *, void *);

static GetDataFn real_get;
static GetSizeFn real_size;
static SetDataFn real_set;
static HasFn real_has;
static SettableFn real_settable;
static ListenerFn real_listen;
static int gSpyFd = -1;
static os_unfair_lock gSpyLock = OS_UNFAIR_LOCK_INIT;
static UInt32 gSpyLines;

static void
spy_log(const char *fn, AudioObjectID o, const AudioObjectPropertyAddress *a, long long r, UInt32 extra)
{
	char line[200], s1[5], s2[5];
	if (gSpyLines > 20000) {
		return;
	}
	int n = snprintf(line, sizeof(line), "%s obj %u '%s' '%s' el %u -> %lld (%u)\n", fn, o,
	    a ? fcc(a->mSelector, s1) : "----", a ? fcc(a->mScope, s2) : "----", a ? a->mElement : 0, r, extra);
	os_unfair_lock_lock(&gSpyLock);
	if (gSpyFd < 0) {
		gSpyFd = open("/private/var/mobile/Library/Logs/a5/vaspy.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
	}
	if (gSpyFd >= 0 && n > 0) {
		(void)write(gSpyFd, line, (size_t)n);
		gSpyLines++;
	}
	os_unfair_lock_unlock(&gSpyLock);
}

static OSStatus
spy_get(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 *sz, void *d)
{
	UInt32 in = sz ? *sz : 0;
	OSStatus r = real_get(o, a, qs, q, sz, d);
	UInt32 v = (r == 0 && sz && *sz >= 4 && d) ? *(UInt32 *)d : in;
	spy_log("get", o, a, r, v);
	return r;
}

static OSStatus
spy_size(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 *sz)
{
	OSStatus r = real_size(o, a, qs, q, sz);
	spy_log("size", o, a, r, sz ? *sz : 0);
	return r;
}

static OSStatus
spy_set(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 sz, const void *d)
{
	OSStatus r = real_set(o, a, qs, q, sz, d);
	spy_log("set", o, a, r, (sz >= 4 && d) ? *(const UInt32 *)d : sz);
	return r;
}

static Boolean
spy_has(AudioObjectID o, const AudioObjectPropertyAddress *a)
{
	Boolean r = real_has(o, a);
	spy_log("has", o, a, r, 0);
	return r;
}

static OSStatus
spy_settable(AudioObjectID o, const AudioObjectPropertyAddress *a, Boolean *out)
{
	OSStatus r = real_settable(o, a, out);
	spy_log("settable", o, a, r, out ? *out : 0);
	return r;
}

static OSStatus
spy_listen(AudioObjectID o, const AudioObjectPropertyAddress *a, void *l, void *c)
{
	OSStatus r = real_listen(o, a, l, c);
	spy_log("listen", o, a, r, 0);
	return r;
}

static void
spy_patch_image(const struct mach_header *mh, intptr_t slide)
{
	Dl_info info;
	if (!dladdr(mh, &info) || !info.dli_fname || !strstr(info.dli_fname, "VirtualAudio")) {
		return;
	}
	const void *real[6] = { real_get, real_size, real_set, real_has, real_settable, real_listen };
	const void *wrap[6] = { spy_get, spy_size, spy_set, spy_has, spy_settable, spy_listen };
	const struct mach_header_64 *h = (const struct mach_header_64 *)mh;
	const uint8_t *lc = (const uint8_t *)(h + 1);
	UInt32 patched = 0;
	for (UInt32 i = 0; i < h->ncmds; i++, lc += ((const struct load_command *)lc)->cmdsize) {
		const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
		if (seg->cmd != LC_SEGMENT_64 || strncmp(seg->segname, "__DATA", 6)) {
			continue;
		}
		uintptr_t lo = (uintptr_t)(seg->vmaddr + (uint64_t)slide), hi = lo + seg->vmsize;
		size_t pg = (size_t)getpagesize();
		for (uintptr_t p = lo; p + 8 <= hi; p += 8) {
			const void *v = *(const void *const *)p;
			for (int k = 0; k < 6; k++) {
				if (real[k] && v == real[k]) {
					uintptr_t page = p & ~(pg - 1);
					mprotect((void *)page, pg, PROT_READ | PROT_WRITE);
					*(const void **)p = wrap[k];
					patched++;
				}
			}
		}
	}
	char msg[300];
	int n = snprintf(msg, sizeof(msg), "spy: %s patched %u pointers\n", info.dli_fname, patched);
	if (n > 0) {
		trace("%s", msg);
	}
}

static void
spy_install(void)
{
	real_get = (GetDataFn)dlsym(RTLD_DEFAULT, "AudioObjectGetPropertyData");
	real_size = (GetSizeFn)dlsym(RTLD_DEFAULT, "AudioObjectGetPropertyDataSize");
	real_set = (SetDataFn)dlsym(RTLD_DEFAULT, "AudioObjectSetPropertyData");
	real_has = (HasFn)dlsym(RTLD_DEFAULT, "AudioObjectHasProperty");
	real_settable = (SettableFn)dlsym(RTLD_DEFAULT, "AudioObjectIsPropertySettable");
	real_listen = (ListenerFn)dlsym(RTLD_DEFAULT, "AudioObjectAddPropertyListener");
	trace("spy: get %p size %p set %p has %p\n", (void *)real_get, (void *)real_size, (void *)real_set, (void *)real_has);
	_dyld_register_func_for_add_image(spy_patch_image);
}
