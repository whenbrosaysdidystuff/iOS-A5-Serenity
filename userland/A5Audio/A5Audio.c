/*
 * A5Audio.driver: CoreAudio AudioServerPlugIn that gives iOS (mediaserverd) an output device on the
 * Redmi A5. One device "Speaker" (built-in), one stereo output stream, 48 kHz. The HAL mixes into
 * Float32; DoIOOperation converts to S16LE and writes it to /dev/a5audio (bsd/dev/sweet_a5_audio.c,
 * xnu patch 0094), which feeds the audio DSP -> codec -> FS1588 amp -> loudspeaker.
 *
 * The device clock is the host clock: one zero time stamp every RING_FRAMES frames at exactly
 * 48000 Hz of mach_absolute_time. The kernel ring (~680 ms) absorbs the difference to the DSP's
 * crystal; the kernel stops the amplifiers 2 s after the last write.
 *
 * Installed as /System/Library/Audio/Plug-Ins/HAL/A5Audio.driver (tools/sweet-root-tweaks.py).
 * When mediaserverd loads it, a short 660 Hz beep is played (proof the plug-in is alive).
 *
 * /private/var/db/a5audio.conf (optional, written by a5update) overrides the device identity for
 * VirtualAudio experiments, one "key=value" per line: uid=, name=, model=, beep=0.
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <mach/mach_time.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdarg.h>
#include <os/lock.h>
#include <pthread.h>

#define kObjectID_PlugIn        kAudioObjectPlugInObject
#define kObjectID_Device        2
#define kObjectID_Stream        3
#define kObjectID_StreamIn      4       /* silent input: VirtualAudio requires the Codec to have one */
/* macOS-only names in the SDK headers (same values on iOS) */
#define kAudioDevicePropertyDataSource                  'ssrc'
#define kAudioDevicePropertyDataSources                 'ssc#'
#define kAudioDevicePropertyDataSourceNameForIDCFString 'lscn'

#define kDevice_UID             "A5AudioSpeaker_UID"
#define kDevice_ModelUID        "A5AudioSpeaker_ModelUID"
#define kSampleRate             48000.0
#define kChannels               2
#define RING_FRAMES             16384u
#define kLatencyFrames          480u
#define kSafetyOffset           256u

static AudioServerPlugInHostRef gHost;
static _Atomic UInt32 gRefCount = 1;
static UInt32 gIOCount;
static int gFd = -1;
static Float64 gHostTicksPerFrame;
static UInt64 gAnchorHostTime;
static UInt64 gNumberTimeStamps;
static Float32 gVolume = 1.0f;          /* reserved for a volume control */
static CFStringRef gUID, gName, gModel;

/* iOS-private device properties VirtualAudio's Device_Codec_Aspen listens to (any scope):
 * 'bb2w' 'w2bb' 'fm2r' (play-through scope) and 'iend' (input). The HAL forwards non-standard
 * selectors of a plug-in only when they are declared in kAudioObjectPropertyCustomPropertyInfoList,
 * and then only as CF types, so they are published as CFNumbers (settable). */
static const UInt32 kExtraSel[] = { 'bb2w', 'w2bb', 'fm2r', 'iend' };
static UInt32 gExtraVal[4];
/* exp8: declaring them made VirtualAudio read them through the HAL, which then failed with 'nope'
 * (the HAL only hands custom properties out as CF objects); "custom=1" in a5audio.conf turns it on */
static int gDeclareCustom;

/* data sources: VirtualAudio's codec needs input data sources (kAudioDevicePropertyDataSources
 * 'ssc#' on the input scope) to build its microphone sub-ports. One built-in mic, 'imic'. */
static UInt32 gInSource = 'imic';

static int
extra_index(UInt32 sel)
{
	for (int i = 0; i < 4; i++) {
		if (kExtraSel[i] == sel) {
			return i;
		}
	}
	return -1;
}

/* trace of what the HAL / VirtualAudio asks this driver, each distinct request once:
 * /private/var/mobile/Library/Logs/a5/a5audio.log (collected by a5update) */
static os_unfair_lock gTraceLock = OS_UNFAIR_LOCK_INIT;
static int gTraceFd = -1;
static UInt64 gSeen[1024];
static UInt32 gSeenN;

static void
trace(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (n <= 0) {
		return;
	}
	if (gTraceFd < 0) {
		gTraceFd = open("/private/var/mobile/Library/Logs/a5/a5audio.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
	}
	if (gTraceFd >= 0) {
		(void)write(gTraceFd, line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
	}
}

static const char *
fcc(UInt32 v, char *b)
{
	for (int i = 0; i < 4; i++) {
		char c = (char)(v >> (24 - 8 * i));
		b[i] = (c >= 32 && c < 127) ? c : '?';
	}
	b[4] = 0;
	return b;
}

/* log a property request the first time it is seen */
static void
trace_prop(const char *what, AudioObjectID o, const AudioObjectPropertyAddress *a, OSStatus st)
{
	static UInt32 errors;
	UInt64 key = ((UInt64)a->mSelector << 32) ^ ((UInt64)a->mScope << 8) ^ ((UInt64)o << 3) ^ (UInt64)(what[0] + (st ? 1 : 0));
	os_unfair_lock_lock(&gTraceLock);
	/* failures are logged every time (up to 3000), so requests made after the HAL's own are seen */
	if (st && errors < 3000) {
		errors++;
	} else {
		for (UInt32 i = 0; i < gSeenN; i++) {
			if (gSeen[i] == key) {
				os_unfair_lock_unlock(&gTraceLock);
				return;
			}
		}
	}
	if (gSeenN < 1024) {
		gSeen[gSeenN++] = key;
	}
	char s1[5], s2[5];
	trace("%s obj %u '%s' scope '%s' elem %u -> %d\n", what, o, fcc(a->mSelector, s1), fcc(a->mScope, s2), a->mElement, (int)st);
	os_unfair_lock_unlock(&gTraceLock);
}

#include "spy.h"
static int gBeep = 1;
static int gSpy;

static void
read_conf(void)
{
	char buf[1024], uid[128] = kDevice_UID, name[128] = "Speaker", model[128] = kDevice_ModelUID;
	int fd = open("/private/var/db/a5audio.conf", O_RDONLY);
	if (fd >= 0) {
		ssize_t n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		buf[n > 0 ? n : 0] = 0;
		for (char *l = buf; l && *l;) {
			char *nl = strchr(l, '\n');
			if (nl) {
				*nl = 0;
			}
			char *eq = strchr(l, '=');
			if (eq) {
				*eq = 0;
				char *dst = !strcmp(l, "uid") ? uid : !strcmp(l, "name") ? name : !strcmp(l, "model") ? model : NULL;
				if (dst) {
					strlcpy(dst, eq + 1, 128);
				} else if (!strcmp(l, "beep")) {
					gBeep = eq[1] != '0';
				} else if (!strcmp(l, "spy")) {
					gSpy = eq[1] == '1';
				} else if (!strcmp(l, "custom")) {
					gDeclareCustom = eq[1] == '1';
				}
			}
			l = nl ? nl + 1 : NULL;
		}
	}
	gUID = CFStringCreateWithCString(NULL, uid, kCFStringEncodingUTF8);
	gName = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
	gModel = CFStringCreateWithCString(NULL, model, kCFStringEncodingUTF8);
}

/* ---------------------------------------------------------------- helpers */
static void
open_dev(void)
{
	if (gFd < 0) {
		gFd = open("/dev/a5audio", O_WRONLY);
	}
}

static void
close_dev(void)
{
	if (gFd >= 0) {
		close(gFd);
		gFd = -1;
	}
}

/* 200 ms 660 Hz beep with a linear fade, written directly to the kernel device */
static void *
hello_beep(void)
{
	static const int16_t sine16[16] = { 0, 3062, 5657, 7391, 8000, 7391, 5657, 3062,
		0, -3062, -5657, -7391, -8000, -7391, -5657, -3062 };
	int fd = -1;
	for (int t = 0; t < 60 && fd < 0; t++) {        /* the kernel driver may still be starting */
		fd = open("/dev/a5audio", O_WRONLY);
		if (fd < 0) {
			usleep(250000);
		}
	}
	if (fd < 0) {
		return NULL;
	}
	enum { N = 48000 / 5 };
	static int16_t buf[N * 2];
	UInt32 ph = 0, step = (UInt32)(660u * 16u * 65536u / 48000u);
	for (UInt32 i = 0; i < N; i++) {
		int32_t s = sine16[(ph >> 16) & 15] * (int32_t)(N - i) / N;
		ph += step;
		buf[2 * i] = buf[2 * i + 1] = (int16_t)s;
	}
	(void)write(fd, buf, sizeof(buf));
	close(fd);
	return NULL;
}

static void
fill_asbd(AudioStreamBasicDescription *d, Boolean integer)
{
	memset(d, 0, sizeof(*d));
	d->mSampleRate = kSampleRate;
	d->mFormatID = kAudioFormatLinearPCM;
	d->mChannelsPerFrame = kChannels;
	d->mFramesPerPacket = 1;
	if (integer) {
		d->mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
		d->mBitsPerChannel = 16;
	} else {
		d->mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
		d->mBitsPerChannel = 32;
	}
	d->mBytesPerFrame = d->mBitsPerChannel / 8 * kChannels;
	d->mBytesPerPacket = d->mBytesPerFrame;
}

#define NEED(sz) do { if (inDataSize < (sz)) return kAudioHardwareBadPropertySizeError; } while (0)

/* ---------------------------------------------------------------- IUnknown */
static HRESULT A5_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface);
static ULONG A5_AddRef(void *inDriver);
static ULONG A5_Release(void *inDriver);
static OSStatus A5_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
static OSStatus A5_CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo *, AudioObjectID *);
static OSStatus A5_DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID);
static OSStatus A5_AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
static OSStatus A5_RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
static OSStatus A5_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
static OSStatus A5_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
static Boolean A5_HasProperty(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *);
static OSStatus A5_IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, Boolean *);
static OSStatus A5_GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32 *);
static OSStatus A5_GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32, UInt32 *, void *);
static OSStatus A5_SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32, const void *);
static OSStatus A5_StartIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus A5_StopIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus A5_GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64 *, UInt64 *, UInt64 *);
static OSStatus A5_WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean *, Boolean *);
static OSStatus A5_BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *);
static OSStatus A5_DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *, void *, void *);
static OSStatus A5_EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *);

static AudioServerPlugInDriverInterface gInterface = {
	NULL,
	A5_QueryInterface, A5_AddRef, A5_Release,
	A5_Initialize, A5_CreateDevice, A5_DestroyDevice, A5_AddDeviceClient, A5_RemoveDeviceClient,
	A5_PerformDeviceConfigurationChange, A5_AbortDeviceConfigurationChange,
	A5_HasProperty, A5_IsPropertySettable, A5_GetPropertyDataSize, A5_GetPropertyData, A5_SetPropertyData,
	A5_StartIO, A5_StopIO, A5_GetZeroTimeStamp, A5_WillDoIOOperation, A5_BeginIOOperation,
	A5_DoIOOperation, A5_EndIOOperation
};
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;

/* factory named in Info.plist CFPlugInFactories */
void *A5Audio_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID);
void *
A5Audio_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID)
{
	(void)inAllocator;
	if (CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) {
		return gDriverRef;
	}
	return NULL;
}

static HRESULT
A5_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface)
{
	if (inDriver != gDriverRef || !outInterface) {
		return kAudioHardwareIllegalOperationError;
	}
	CFUUIDRef req = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
	HRESULT r = E_NOINTERFACE;
	if (CFEqual(req, IUnknownUUID) || CFEqual(req, kAudioServerPlugInDriverInterfaceUUID)) {
		atomic_fetch_add(&gRefCount, 1);
		*outInterface = gDriverRef;
		r = S_OK;
	}
	CFRelease(req);
	return r;
}

static ULONG
A5_AddRef(void *inDriver)
{
	if (inDriver != gDriverRef) {
		return 0;
	}
	return atomic_fetch_add(&gRefCount, 1) + 1;
}

static ULONG
A5_Release(void *inDriver)
{
	if (inDriver != gDriverRef) {
		return 0;
	}
	UInt32 c = atomic_load(&gRefCount);
	if (c > 0) {
		c = atomic_fetch_sub(&gRefCount, 1) - 1;
	}
	return c;
}

/* ---------------------------------------------------------------- basic operations */
static OSStatus
A5_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost)
{
	if (inDriver != gDriverRef) {
		return kAudioHardwareBadObjectError;
	}
	gHost = inHost;
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	Float64 hostHz = 1e9 * (Float64)tb.denom / (Float64)tb.numer;
	gHostTicksPerFrame = hostHz / kSampleRate;
	read_conf();
	if (gSpy) {
		spy_install();
	}
	trace("--- A5Audio Initialize (pid %d)\n", getpid());
	if (gBeep) {
		pthread_t th;
		if (pthread_create(&th, NULL, (void *(*)(void *))hello_beep, NULL) == 0) {
			pthread_detach(th);
		}
	}
	return 0;
}

static OSStatus A5_CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef a, const AudioServerPlugInClientInfo *c, AudioObjectID *o) { (void)d; (void)a; (void)c; (void)o; return kAudioHardwareUnsupportedOperationError; }
static OSStatus A5_DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID o) { (void)d; (void)o; return kAudioHardwareUnsupportedOperationError; }
static OSStatus
A5_AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c)
{
	char id[256] = "?";
	(void)d;
	if (c && c->mBundleID) {
		CFStringGetCString(c->mBundleID, id, sizeof(id), kCFStringEncodingUTF8);
	}
	trace("client add obj %u id %u pid %d native %d bundle %s\n", o, c ? c->mClientID : 0, c ? c->mProcessID : 0,
	    c ? c->mIsNativeEndian : 0, id);
	return 0;
}
static OSStatus A5_RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c) { (void)d; (void)o; (void)c; return 0; }
static OSStatus A5_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i) { (void)d; (void)o; (void)a; (void)i; return 0; }
static OSStatus A5_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i) { (void)d; (void)o; (void)a; (void)i; return 0; }

/* ---------------------------------------------------------------- properties */
static Boolean
A5_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t pid, const AudioObjectPropertyAddress *a)
{
	UInt32 size = 0;
	(void)pid;
	return A5_GetPropertyDataSize(inDriver, inObjectID, 0, a, 0, NULL, &size) == 0;
}

static OSStatus
A5_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *out)
{
	UInt32 size = 0;
	(void)pid;
	OSStatus e = A5_GetPropertyDataSize(inDriver, inObjectID, 0, a, 0, NULL, &size);
	if (e) {
		return e;
	}
	*out = false;
	if (inObjectID == kObjectID_Device && a->mSelector == kAudioDevicePropertyNominalSampleRate) {
		*out = true;            /* only the one rate is accepted */
	}
	return 0;
}

/* size of each property; also the "has property" test */
static OSStatus A5_GetPropertyDataSize_(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *,
    UInt32, const void *, UInt32 *);
static OSStatus
A5_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a,
    UInt32 qs, const void *q, UInt32 *out)
{
	OSStatus st = A5_GetPropertyDataSize_(inDriver, o, pid, a, qs, q, out);
	trace_prop("query", o, a, st);
	return st;
}

static OSStatus
A5_GetPropertyDataSize_(AudioServerPlugInDriverRef inDriver, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a,
    UInt32 qs, const void *q, UInt32 *out)
{
	(void)pid; (void)qs; (void)q;
	if (inDriver != gDriverRef) {
		return kAudioHardwareBadObjectError;
	}
	switch (o) {
	case kObjectID_PlugIn:
		switch (a->mSelector) {
		case kAudioObjectPropertyBaseClass:
		case kAudioObjectPropertyClass:
			*out = sizeof(AudioClassID); return 0;
		case kAudioObjectPropertyOwner:
		case kAudioPlugInPropertyTranslateUIDToDevice:
			*out = sizeof(AudioObjectID); return 0;
		case kAudioObjectPropertyManufacturer:
		case kAudioPlugInPropertyResourceBundle:
			*out = sizeof(CFStringRef); return 0;
		case kAudioObjectPropertyOwnedObjects:
		case kAudioPlugInPropertyDeviceList:
			*out = sizeof(AudioObjectID); return 0;
		}
		break;
	case kObjectID_Device:
		switch (a->mSelector) {
		case kAudioObjectPropertyBaseClass:
		case kAudioObjectPropertyClass:
			*out = sizeof(AudioClassID); return 0;
		case kAudioObjectPropertyOwner:
			*out = sizeof(AudioObjectID); return 0;
		case kAudioObjectPropertyName:
		case kAudioObjectPropertyManufacturer:
		case kAudioDevicePropertyDeviceUID:
		case kAudioDevicePropertyModelUID:
			*out = sizeof(CFStringRef); return 0;
		case kAudioObjectPropertyOwnedObjects:
		case kAudioDevicePropertyStreams:
			*out = (a->mScope == kAudioObjectPropertyScopeGlobal ? 2 : 1) * sizeof(AudioObjectID); return 0;
		case kAudioObjectPropertyControlList:
		case kAudioDevicePropertyRelatedDevices:
			*out = (a->mSelector == kAudioDevicePropertyRelatedDevices) ? sizeof(AudioObjectID) : 0; return 0;
		case kAudioDevicePropertyTransportType:
		case kAudioDevicePropertyClockDomain:
		case kAudioDevicePropertyDeviceIsAlive:
		case kAudioDevicePropertyDeviceIsRunning:
		case kAudioDevicePropertyDeviceCanBeDefaultDevice:
		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
		case kAudioDevicePropertyLatency:
		case kAudioDevicePropertySafetyOffset:
		case kAudioDevicePropertyIsHidden:
		case kAudioDevicePropertyZeroTimeStampPeriod:
			*out = sizeof(UInt32); return 0;
		case kAudioDevicePropertyNominalSampleRate:
			*out = sizeof(Float64); return 0;
		case kAudioDevicePropertyAvailableNominalSampleRates:
			*out = sizeof(AudioValueRange); return 0;
		case kAudioDevicePropertyPreferredChannelsForStereo:
			*out = 2 * sizeof(UInt32); return 0;
		case kAudioDevicePropertyPreferredChannelLayout:
			*out = offsetof(AudioChannelLayout, mChannelDescriptions) + kChannels * sizeof(AudioChannelDescription); return 0;
		}
		if (extra_index(a->mSelector) >= 0) {
			*out = sizeof(CFPropertyListRef);
			return 0;
		}
		if (a->mScope == kAudioObjectPropertyScopeInput && (a->mSelector == kAudioDevicePropertyDataSources ||
		    a->mSelector == kAudioDevicePropertyDataSource)) {
			*out = sizeof(UInt32);
			return 0;
		}
		if (a->mScope == kAudioObjectPropertyScopeInput && a->mSelector == kAudioDevicePropertyDataSourceNameForIDCFString) {
			*out = sizeof(AudioValueTranslation);
			return 0;
		}
		if (a->mSelector == kAudioObjectPropertyCustomPropertyInfoList && gDeclareCustom) {
			*out = 4 * sizeof(AudioServerPlugInCustomPropertyInfo);
			return 0;
		}
		break;
	case kObjectID_Stream:
	case kObjectID_StreamIn:
		switch (a->mSelector) {
		case kAudioObjectPropertyBaseClass:
		case kAudioObjectPropertyClass:
			*out = sizeof(AudioClassID); return 0;
		case kAudioObjectPropertyOwner:
			*out = sizeof(AudioObjectID); return 0;
		case kAudioObjectPropertyOwnedObjects:
			*out = 0; return 0;
		case kAudioStreamPropertyIsActive:
		case kAudioStreamPropertyDirection:
		case kAudioStreamPropertyTerminalType:
		case kAudioStreamPropertyStartingChannel:
		case kAudioStreamPropertyLatency:
		case 'taps':    /* iOS HAL stream property (UInt32) VirtualAudio reads on input streams; 0 */
			*out = sizeof(UInt32); return 0;
		case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat:
			*out = sizeof(AudioStreamBasicDescription); return 0;
		case kAudioStreamPropertyAvailableVirtualFormats:
		case kAudioStreamPropertyAvailablePhysicalFormats:
			*out = sizeof(AudioStreamRangedDescription); return 0;
		}
		break;
	}
	return kAudioHardwareUnknownPropertyError;
}

static OSStatus
A5_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a,
    UInt32 qs, const void *q, UInt32 inDataSize, UInt32 *outSize, void *d)
{
	UInt32 need = 0;
	OSStatus e = A5_GetPropertyDataSize(inDriver, o, pid, a, qs, q, &need);
	if (e) {
		return e;
	}
	NEED(need);
	*outSize = need;
	if (need == 0) {
		return 0;
	}
	UInt32 *u = d;
	switch (o) {
	case kObjectID_PlugIn:
		switch (a->mSelector) {
		case kAudioObjectPropertyBaseClass: *u = kAudioObjectClassID; return 0;
		case kAudioObjectPropertyClass: *u = kAudioPlugInClassID; return 0;
		case kAudioObjectPropertyOwner: *u = kAudioObjectUnknown; return 0;
		case kAudioObjectPropertyManufacturer: *(CFStringRef *)d = CFSTR("A5 port"); return 0;
		case kAudioPlugInPropertyResourceBundle: *(CFStringRef *)d = CFSTR(""); return 0;
		case kAudioObjectPropertyOwnedObjects:
		case kAudioPlugInPropertyDeviceList: *u = kObjectID_Device; return 0;
		case kAudioPlugInPropertyTranslateUIDToDevice: {
			CFStringRef uid = (qs == sizeof(CFStringRef) && q) ? *(const CFStringRef *)q : NULL;
			*u = (uid && CFEqual(uid, gUID)) ? kObjectID_Device : kAudioObjectUnknown;
			return 0;
		}
		}
		break;
	case kObjectID_Device:
		switch (a->mSelector) {
		case kAudioObjectPropertyBaseClass: *u = kAudioObjectClassID; return 0;
		case kAudioObjectPropertyClass: *u = kAudioDeviceClassID; return 0;
		case kAudioObjectPropertyOwner: *u = kObjectID_PlugIn; return 0;
		case kAudioObjectPropertyName: *(CFStringRef *)d = CFRetain(gName); return 0;
		case kAudioObjectPropertyManufacturer: *(CFStringRef *)d = CFSTR("A5 port"); return 0;
		case kAudioDevicePropertyDeviceUID: *(CFStringRef *)d = CFRetain(gUID); return 0;
		case kAudioDevicePropertyModelUID: *(CFStringRef *)d = CFRetain(gModel); return 0;
		case kAudioObjectPropertyOwnedObjects:
		case kAudioDevicePropertyStreams:
			if (a->mScope == kAudioObjectPropertyScopeInput) {
				u[0] = kObjectID_StreamIn;
			} else {
				u[0] = kObjectID_Stream;
				if (a->mScope == kAudioObjectPropertyScopeGlobal) {
					u[1] = kObjectID_StreamIn;
				}
			}
			return 0;
		case kAudioDevicePropertyRelatedDevices: *u = kObjectID_Device; return 0;
		case kAudioDevicePropertyTransportType: *u = kAudioDeviceTransportTypeBuiltIn; return 0;
		case kAudioDevicePropertyClockDomain: *u = 0; return 0;
		case kAudioDevicePropertyDeviceIsAlive: *u = 1; return 0;
		case kAudioDevicePropertyDeviceIsRunning: *u = gIOCount > 0; return 0;
		case kAudioDevicePropertyDeviceCanBeDefaultDevice: *u = 1; return 0;
		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: *u = 1; return 0;
		case kAudioDevicePropertyLatency: *u = kLatencyFrames; return 0;
		case kAudioDevicePropertySafetyOffset: *u = kSafetyOffset; return 0;
		case kAudioDevicePropertyIsHidden: *u = 0; return 0;
		case kAudioDevicePropertyZeroTimeStampPeriod: *u = RING_FRAMES; return 0;
		case kAudioDevicePropertyNominalSampleRate: *(Float64 *)d = kSampleRate; return 0;
		case kAudioDevicePropertyAvailableNominalSampleRates: {
			AudioValueRange *r = d;
			r->mMinimum = r->mMaximum = kSampleRate;
			return 0;
		}
		case kAudioDevicePropertyPreferredChannelsForStereo: u[0] = 1; u[1] = 2; return 0;
		case kAudioDevicePropertyPreferredChannelLayout: {
			AudioChannelLayout *l = d;
			memset(l, 0, need);
			l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
			l->mNumberChannelDescriptions = kChannels;
			l->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
			l->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
			return 0;
		}
		}
		if (extra_index(a->mSelector) >= 0) {
			SInt32 v = (SInt32)gExtraVal[extra_index(a->mSelector)];
			*(CFPropertyListRef *)d = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
			return 0;
		}
		if (a->mSelector == kAudioDevicePropertyDataSources) {
			*u = 'imic';
			return 0;
		}
		if (a->mSelector == kAudioDevicePropertyDataSource) {
			*u = gInSource;
			return 0;
		}
		if (a->mSelector == kAudioDevicePropertyDataSourceNameForIDCFString) {
			AudioValueTranslation *t = d;
			if (t->mOutputDataSize < sizeof(CFStringRef) || !t->mOutputData) {
				return kAudioHardwareBadPropertySizeError;
			}
			*(CFStringRef *)t->mOutputData = CFSTR("Microphone");
			t->mOutputDataSize = sizeof(CFStringRef);
			return 0;
		}
		if (a->mSelector == kAudioObjectPropertyCustomPropertyInfoList) {
			AudioServerPlugInCustomPropertyInfo *ci = d;
			for (int i = 0; i < 4; i++) {
				ci[i].mSelector = kExtraSel[i];
				ci[i].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFPropertyList;
				ci[i].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
			}
			return 0;
		}
		break;
	case kObjectID_Stream:
	case kObjectID_StreamIn:
		switch (a->mSelector) {
		case kAudioObjectPropertyBaseClass: *u = kAudioObjectClassID; return 0;
		case kAudioObjectPropertyClass: *u = kAudioStreamClassID; return 0;
		case kAudioObjectPropertyOwner: *u = kObjectID_Device; return 0;
		case kAudioStreamPropertyIsActive: *u = 1; return 0;
		case kAudioStreamPropertyDirection: *u = (o == kObjectID_StreamIn); return 0;   /* 0 out, 1 in */
		case kAudioStreamPropertyTerminalType:
			*u = o == kObjectID_StreamIn ? kAudioStreamTerminalTypeMicrophone : kAudioStreamTerminalTypeSpeaker;
			return 0;
		case kAudioStreamPropertyStartingChannel: *u = 1; return 0;
		case 'taps': *u = 0; return 0;
		case kAudioStreamPropertyLatency: *u = 0; return 0;
		case kAudioStreamPropertyVirtualFormat:
			fill_asbd(d, false); return 0;
		case kAudioStreamPropertyPhysicalFormat:
			fill_asbd(d, true); return 0;
		case kAudioStreamPropertyAvailableVirtualFormats:
		case kAudioStreamPropertyAvailablePhysicalFormats: {
			AudioStreamRangedDescription *r = d;
			fill_asbd(&r->mFormat, a->mSelector == kAudioStreamPropertyAvailablePhysicalFormats);
			r->mSampleRateRange.mMinimum = r->mSampleRateRange.mMaximum = kSampleRate;
			return 0;
		}
		}
		break;
	}
	return kAudioHardwareUnknownPropertyError;
}

static OSStatus
A5_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a,
    UInt32 qs, const void *q, UInt32 inDataSize, const void *d)
{
	(void)pid; (void)qs; (void)q;
	if (inDriver != gDriverRef) {
		return kAudioHardwareBadObjectError;
	}
	if (o == kObjectID_Device && a->mSelector == kAudioDevicePropertyDataSource) {
		NEED(sizeof(UInt32));
		trace_prop("set-ok", o, a, 0);
		return *(const UInt32 *)d == 'imic' ? 0 : kAudioHardwareIllegalOperationError;
	}
	if (o == kObjectID_Device && extra_index(a->mSelector) >= 0) {
		NEED(sizeof(CFPropertyListRef));
		CFPropertyListRef pl = *(const CFPropertyListRef *)d;
		SInt32 v = 0;
		if (pl && CFGetTypeID(pl) == CFNumberGetTypeID()) {
			CFNumberGetValue((CFNumberRef)pl, kCFNumberSInt32Type, &v);
		}
		gExtraVal[extra_index(a->mSelector)] = (UInt32)v;
		trace_prop("set-ok", o, a, 0);
		return 0;
	}
	if (o == kObjectID_Device && a->mSelector == kAudioDevicePropertyNominalSampleRate) {
		NEED(sizeof(Float64));
		return *(const Float64 *)d == kSampleRate ? 0 : kAudioHardwareIllegalOperationError;
	}
	if ((o == kObjectID_Stream || o == kObjectID_StreamIn) && (a->mSelector == kAudioStreamPropertyVirtualFormat ||
	    a->mSelector == kAudioStreamPropertyPhysicalFormat)) {
		NEED(sizeof(AudioStreamBasicDescription));
		const AudioStreamBasicDescription *f = d;
		return f->mSampleRate == kSampleRate && f->mChannelsPerFrame == kChannels ? 0 : kAudioDeviceUnsupportedFormatError;
	}
	trace_prop("set", o, a, kAudioHardwareUnknownPropertyError);
	return kAudioHardwareUnknownPropertyError;
}

/* ---------------------------------------------------------------- IO */
static OSStatus
A5_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID o, UInt32 client)
{
	(void)client;
	if (inDriver != gDriverRef || o != kObjectID_Device) {
		return kAudioHardwareBadObjectError;
	}
	trace("StartIO client %u (io count %u)\n", client, gIOCount);
	if (gIOCount++ == 0) {
		open_dev();
		gNumberTimeStamps = 0;
		gAnchorHostTime = mach_absolute_time();
	}
	return 0;
}

static OSStatus
A5_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID o, UInt32 client)
{
	(void)client;
	if (inDriver != gDriverRef || o != kObjectID_Device) {
		return kAudioHardwareBadObjectError;
	}
	trace("StopIO client %u\n", client);
	if (gIOCount > 0 && --gIOCount == 0) {
		close_dev();
	}
	return 0;
}

static OSStatus
A5_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID o, UInt32 client, Float64 *outSampleTime,
    UInt64 *outHostTime, UInt64 *outSeed)
{
	(void)client;
	if (inDriver != gDriverRef || o != kObjectID_Device) {
		return kAudioHardwareBadObjectError;
	}
	UInt64 now = mach_absolute_time();
	Float64 ticksPerRing = gHostTicksPerFrame * RING_FRAMES;
	UInt64 next = gAnchorHostTime + (UInt64)((Float64)(gNumberTimeStamps + 1) * ticksPerRing);
	if (next <= now) {
		gNumberTimeStamps++;
	}
	*outSampleTime = (Float64)(gNumberTimeStamps * RING_FRAMES);
	*outHostTime = gAnchorHostTime + (UInt64)((Float64)gNumberTimeStamps * ticksPerRing);
	*outSeed = 1;
	return 0;
}

static OSStatus
A5_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID o, UInt32 client, UInt32 op, Boolean *will, Boolean *inPlace)
{
	(void)client;
	if (inDriver != gDriverRef || o != kObjectID_Device) {
		return kAudioHardwareBadObjectError;
	}
	*will = (op == kAudioServerPlugInIOOperationWriteMix || op == kAudioServerPlugInIOOperationReadInput);
	*inPlace = true;
	return 0;
}

static OSStatus
A5_BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 c, UInt32 op, UInt32 n, const AudioServerPlugInIOCycleInfo *i)
{
	(void)d; (void)o; (void)c; (void)op; (void)n; (void)i;
	return 0;
}

static OSStatus
A5_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID o, AudioObjectID stream, UInt32 client, UInt32 op,
    UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle, void *main, void *secondary)
{
	(void)client; (void)cycle; (void)secondary; (void)stream;
	if (inDriver != gDriverRef || o != kObjectID_Device) {
		return kAudioHardwareBadObjectError;
	}
	if (op == kAudioServerPlugInIOOperationReadInput && main) {
		memset(main, 0, (size_t)frames * kChannels * sizeof(Float32));         /* no microphone yet */
		return 0;
	}
	if (op != kAudioServerPlugInIOOperationWriteMix || !main) {
		return 0;
	}
	if (gFd < 0) {
		open_dev();
		if (gFd < 0) {
			return 0;
		}
	}
	static int16_t out[8192 * kChannels];
	const Float32 *in = main;
	UInt32 done = 0;
	while (done < frames) {
		UInt32 n = frames - done;
		if (n > 8192) {
			n = 8192;
		}
		for (UInt32 k = 0; k < n * kChannels; k++) {
			Float32 v = in[done * kChannels + k] * gVolume;
			v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
			out[k] = (int16_t)(v * 32767.0f);
		}
		(void)write(gFd, out, n * kChannels * sizeof(int16_t));
		done += n;
	}
	return 0;
}

static OSStatus
A5_EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 c, UInt32 op, UInt32 n, const AudioServerPlugInIOCycleInfo *i)
{
	(void)d; (void)o; (void)c; (void)op; (void)n; (void)i;
	return 0;
}
