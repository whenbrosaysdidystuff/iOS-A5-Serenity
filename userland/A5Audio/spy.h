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

#ifndef kAudioObjectSystemObject
#define kAudioObjectSystemObject                    1
#endif
#define A5_kAudioHardwarePropertyDevices            'dev#'
#define A5_kAudioDevicePropertyBufferFrameSize      'fsiz'
#define A5_kAudioDevicePropertyBufferFrameSizeRange 'fsz#'
#define A5_kAudioDevicePropertyStreamConfiguration  'slay'
static AudioObjectID rdp(AudioObjectID o, const AudioObjectPropertyAddress *a);
static int gRedirect;

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

/* deep copy of a property list with every "Null_Device" string replaced by the A5 device UID */
static CFTypeRef
swap_null(CFTypeRef v, int *hits)
{
	if (!v) {
		return NULL;
	}
	CFTypeID t = CFGetTypeID(v);
	if (t == CFStringGetTypeID()) {
		if (CFEqual(v, CFSTR("Null_Device"))) {
			(*hits)++;
			return CFRetain(gUID);
		}
		return CFRetain(v);
	}
	if (t == CFArrayGetTypeID()) {
		CFIndex n = CFArrayGetCount(v);
		CFMutableArrayRef m = CFArrayCreateMutable(NULL, n, &kCFTypeArrayCallBacks);
		for (CFIndex i = 0; i < n; i++) {
			CFTypeRef e = swap_null(CFArrayGetValueAtIndex(v, i), hits);
			CFArrayAppendValue(m, e);
			CFRelease(e);
		}
		return m;
	}
	if (t == CFDictionaryGetTypeID()) {
		CFIndex n = CFDictionaryGetCount(v);
		const void *ks[64], *vs[64];
		if (n > 64) {
			return CFRetain(v);
		}
		CFDictionaryGetKeysAndValues(v, ks, vs);
		CFMutableDictionaryRef m = CFDictionaryCreateMutable(NULL, n, &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);
		for (CFIndex i = 0; i < n; i++) {
			CFTypeRef e = swap_null(vs[i], hits);
			CFDictionarySetValue(m, ks[i], e);
			CFRelease(e);
		}
		return m;
	}
	return CFRetain(v);
}

static void
log_plist(const char *tag, CFTypeRef v)
{
	char buf[1500];
	CFStringRef d = v ? CFCopyDescription(v) : NULL;
	if (d && CFStringGetCString(d, buf, sizeof(buf), kCFStringEncodingUTF8)) {
		trace("%s %s\n", tag, buf);
	}
	if (d) {
		CFRelease(d);
	}
}

static OSStatus
spy_get(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 *sz, void *d)
{
	if (gRedirect && a && a->mSelector == 'cagg' && q && qs == sizeof(CFDictionaryRef)) {
		int hits = 0;
		CFTypeRef desc = *(const CFTypeRef *)q;
		log_plist("cagg in:", desc);
		CFTypeRef nd = swap_null(desc, &hits);
		if (hits) {
			log_plist("cagg out:", nd);
			OSStatus r = real_get(o, a, qs, &nd, sz, d);
			spy_log("get-cagg-swapped", o, a, r, (sz && *sz >= 4 && d) ? *(UInt32 *)d : 0);
			CFRelease(nd);
			return r;
		}
		CFRelease(nd);
	}
	UInt32 in = sz ? *sz : 0;
	OSStatus r = real_get(rdp(o, a), a, qs, q, sz, d);
	UInt32 v = (r == 0 && sz && *sz >= 4 && d) ? *(UInt32 *)d : in;
	spy_log("get", o, a, r, v);
	return r;
}

static OSStatus
spy_size(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 *sz)
{
	OSStatus r = real_size(rdp(o, a), a, qs, q, sz);
	spy_log("size", o, a, r, sz ? *sz : 0);
	return r;
}

static OSStatus
spy_set(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 sz, const void *d)
{
	OSStatus r = real_set(rdp(o, a), a, qs, q, sz, d);
	spy_log("set", o, a, r, (sz >= 4 && d) ? *(const UInt32 *)d : sz);
	return r;
}

static Boolean
spy_has(AudioObjectID o, const AudioObjectPropertyAddress *a)
{
	Boolean r = real_has(rdp(o, a), a);
	spy_log("has", o, a, r, 0);
	return r;
}

static OSStatus
spy_settable(AudioObjectID o, const AudioObjectPropertyAddress *a, Boolean *out)
{
	OSStatus r = real_settable(rdp(o, a), a, out);
	spy_log("settable", o, a, r, out ? *out : 0);
	return r;
}

static OSStatus
spy_listen(AudioObjectID o, const AudioObjectPropertyAddress *a, void *l, void *c)
{
	OSStatus r = real_listen(rdp(o, a), a, l, c);
	spy_log("listen", o, a, r, 0);
	return r;
}


/* ---- redirect (a5audio.conf "redirect=1"): VirtualAudio routes everything to its own Null_Device.
 * Calls VirtualAudio makes on that device (and its streams) for IO, timing and format are sent to
 * the A5 device instead, so whatever iOS plays through the default route reaches the speaker. */
typedef OSStatus (*CreateProcFn)(AudioObjectID, void *, void *, void **);
typedef OSStatus (*ProcFn)(AudioObjectID, void *);
typedef OSStatus (*StartAtFn)(AudioObjectID, void *, void *, UInt32);
typedef OSStatus (*TimeFn)(AudioObjectID, void *);
typedef OSStatus (*NearestFn)(AudioObjectID, void *, UInt32);
typedef OSStatus (*TranslateFn)(AudioObjectID, const void *, void *);
typedef OSStatus (*RemoveListenerFn)(AudioObjectID, const AudioObjectPropertyAddress *, void *, void *);
static CreateProcFn real_create;
static ProcFn real_destroy, real_start, real_stop;
static StartAtFn real_startat;
static TimeFn real_curtime;
static NearestFn real_nearest;
static TranslateFn real_translate;
static RemoveListenerFn real_unlisten;
static AudioObjectID gNullDev, gNullOut, gNullIn, gMyDev, gMyOut, gMyIn;

static AudioObjectID
first_stream(AudioObjectID dev, AudioObjectPropertyScope sc)
{
	AudioObjectPropertyAddress a = { kAudioDevicePropertyStreams, sc, 0 };
	AudioObjectID ids[8];
	UInt32 sz = sizeof(ids);
	if (real_get(dev, &a, 0, NULL, &sz, ids) || sz < sizeof(AudioObjectID)) {
		return 0;
	}
	return ids[0];
}

static void
redirect_resolve(void)
{
	if (gNullDev && gMyDev) {
		return;
	}
	AudioObjectPropertyAddress a = { A5_kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, 0 };
	AudioObjectID ids[64];
	UInt32 sz = sizeof(ids);
	if (real_get(kAudioObjectSystemObject, &a, 0, NULL, &sz, ids)) {
		return;
	}
	for (UInt32 i = 0; i < sz / sizeof(AudioObjectID); i++) {
		AudioObjectPropertyAddress ua = { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, 0 };
		CFStringRef uid = NULL;
		UInt32 us = sizeof(uid);
		if (real_get(ids[i], &ua, 0, NULL, &us, &uid) || !uid) {
			continue;
		}
		if (CFEqual(uid, CFSTR("Null_Device"))) {
			gNullDev = ids[i];
		} else if (CFEqual(uid, gUID)) {
			gMyDev = ids[i];
		}
		CFRelease(uid);
	}
	if (gNullDev && gMyDev) {
		gNullOut = first_stream(gNullDev, kAudioObjectPropertyScopeOutput);
		gNullIn = first_stream(gNullDev, kAudioObjectPropertyScopeInput);
		gMyOut = first_stream(gMyDev, kAudioObjectPropertyScopeOutput);
		gMyIn = first_stream(gMyDev, kAudioObjectPropertyScopeInput);
		trace("redirect: Null_Device %u (out %u in %u) -> A5 %u (out %u in %u)\n", gNullDev, gNullOut, gNullIn,
		    gMyDev, gMyOut, gMyIn);
	}
}

static AudioObjectID
rd(AudioObjectID o)
{
	if (!gRedirect || o == kAudioObjectSystemObject) {
		return o;
	}
	redirect_resolve();
	if (o && o == gNullDev) {
		return gMyDev;
	}
	if (o && o == gNullOut && gMyOut) {
		return gMyOut;
	}
	if (o && o == gNullIn && gMyIn) {
		return gMyIn;
	}
	return o;
}

/* property calls are only redirected for IO-relevant selectors; identity (uid, class, ...) stays */
static AudioObjectID
rdp(AudioObjectID o, const AudioObjectPropertyAddress *a)
{
	switch (a ? a->mSelector : 0) {
	case kAudioDevicePropertyNominalSampleRate:
	case kAudioDevicePropertyAvailableNominalSampleRates:
	case kAudioDevicePropertyLatency:
	case kAudioDevicePropertySafetyOffset:
	case A5_kAudioDevicePropertyBufferFrameSize:
	case A5_kAudioDevicePropertyBufferFrameSizeRange:
	case A5_kAudioDevicePropertyStreamConfiguration:
	case kAudioDevicePropertyZeroTimeStampPeriod:
	case kAudioDevicePropertyDeviceIsRunning:
	case kAudioStreamPropertyVirtualFormat:
	case kAudioStreamPropertyPhysicalFormat:
	case kAudioStreamPropertyAvailableVirtualFormats:
	case kAudioStreamPropertyAvailablePhysicalFormats:
		return rd(o);
	}
	return o;
}

static OSStatus spy_create(AudioObjectID d, void *proc, void *cl, void **id)
{ OSStatus r = real_create(rd(d), proc, cl, id); spy_log("CreateIOProcID", d, NULL, r, rd(d)); return r; }
static OSStatus spy_destroy(AudioObjectID d, void *id)
{ OSStatus r = real_destroy(rd(d), id); spy_log("DestroyIOProcID", d, NULL, r, rd(d)); return r; }
static OSStatus spy_start(AudioObjectID d, void *id)
{ OSStatus r = real_start(rd(d), id); spy_log("Start", d, NULL, r, rd(d)); return r; }
static OSStatus spy_stop(AudioObjectID d, void *id)
{ OSStatus r = real_stop(rd(d), id); spy_log("Stop", d, NULL, r, rd(d)); return r; }
static OSStatus spy_startat(AudioObjectID d, void *id, void *t, UInt32 f)
{ OSStatus r = real_startat(rd(d), id, t, f); spy_log("StartAtTime", d, NULL, r, rd(d)); return r; }
static OSStatus spy_curtime(AudioObjectID d, void *t) { return real_curtime(rd(d), t); }
static OSStatus spy_nearest(AudioObjectID d, void *t, UInt32 f) { return real_nearest(rd(d), t, f); }
static OSStatus spy_translate(AudioObjectID d, const void *i, void *o) { return real_translate(rd(d), i, o); }
static OSStatus spy_unlisten(AudioObjectID o, const AudioObjectPropertyAddress *a, void *l, void *c)
{ return real_unlisten(rdp(o, a), a, l, c); }

static void
spy_patch_image(const struct mach_header *mh, intptr_t slide)
{
	Dl_info info;
	if (!dladdr(mh, &info) || !info.dli_fname || !strstr(info.dli_fname, "VirtualAudio")) {
		return;
	}
	const void *real[15] = { real_get, real_size, real_set, real_has, real_settable, real_listen,
		real_create, real_destroy, real_start, real_stop, real_startat, real_curtime, real_nearest, real_translate,
		real_unlisten };
	const void *wrap[15] = { spy_get, spy_size, spy_set, spy_has, spy_settable, spy_listen,
		spy_create, spy_destroy, spy_start, spy_stop, spy_startat, spy_curtime, spy_nearest, spy_translate,
		spy_unlisten };
	int nfn = gRedirect ? 15 : 6;
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
			for (int k = 0; k < nfn; k++) {
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
	real_create = (CreateProcFn)dlsym(RTLD_DEFAULT, "AudioDeviceCreateIOProcID");
	real_destroy = (ProcFn)dlsym(RTLD_DEFAULT, "AudioDeviceDestroyIOProcID");
	real_start = (ProcFn)dlsym(RTLD_DEFAULT, "AudioDeviceStart");
	real_stop = (ProcFn)dlsym(RTLD_DEFAULT, "AudioDeviceStop");
	real_startat = (StartAtFn)dlsym(RTLD_DEFAULT, "AudioDeviceStartAtTime");
	real_curtime = (TimeFn)dlsym(RTLD_DEFAULT, "AudioDeviceGetCurrentTime");
	real_nearest = (NearestFn)dlsym(RTLD_DEFAULT, "AudioDeviceGetNearestStartTime");
	real_translate = (TranslateFn)dlsym(RTLD_DEFAULT, "AudioDeviceTranslateTime");
	real_unlisten = (RemoveListenerFn)dlsym(RTLD_DEFAULT, "AudioObjectRemovePropertyListener");
	trace("spy: get %p size %p set %p has %p\n", (void *)real_get, (void *)real_size, (void *)real_set, (void *)real_has);
	_dyld_register_func_for_add_image(spy_patch_image);
}
