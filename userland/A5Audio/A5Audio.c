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
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <mach/mach_time.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdatomic.h>

#define kObjectID_PlugIn        kAudioObjectPlugInObject
#define kObjectID_Device        2
#define kObjectID_Stream        3

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
static void
hello_beep(void)
{
	static const int16_t sine16[16] = { 0, 3062, 5657, 7391, 8000, 7391, 5657, 3062,
		0, -3062, -5657, -7391, -8000, -7391, -5657, -3062 };
	int fd = open("/dev/a5audio", O_WRONLY);
	if (fd < 0) {
		return;
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
	hello_beep();
	return 0;
}

static OSStatus A5_CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef a, const AudioServerPlugInClientInfo *c, AudioObjectID *o) { (void)d; (void)a; (void)c; (void)o; return kAudioHardwareUnsupportedOperationError; }
static OSStatus A5_DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID o) { (void)d; (void)o; return kAudioHardwareUnsupportedOperationError; }
static OSStatus A5_AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c) { (void)d; (void)o; (void)c; return 0; }
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
static OSStatus
A5_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a,
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
			*out = (a->mScope == kAudioObjectPropertyScopeInput) ? 0 : sizeof(AudioObjectID); return 0;
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
		break;
	case kObjectID_Stream:
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
			*u = (uid && CFEqual(uid, CFSTR(kDevice_UID))) ? kObjectID_Device : kAudioObjectUnknown;
			return 0;
		}
		}
		break;
	case kObjectID_Device:
		switch (a->mSelector) {
		case kAudioObjectPropertyBaseClass: *u = kAudioObjectClassID; return 0;
		case kAudioObjectPropertyClass: *u = kAudioDeviceClassID; return 0;
		case kAudioObjectPropertyOwner: *u = kObjectID_PlugIn; return 0;
		case kAudioObjectPropertyName: *(CFStringRef *)d = CFSTR("Speaker"); return 0;
		case kAudioObjectPropertyManufacturer: *(CFStringRef *)d = CFSTR("A5 port"); return 0;
		case kAudioDevicePropertyDeviceUID: *(CFStringRef *)d = CFSTR(kDevice_UID); return 0;
		case kAudioDevicePropertyModelUID: *(CFStringRef *)d = CFSTR(kDevice_ModelUID); return 0;
		case kAudioObjectPropertyOwnedObjects:
		case kAudioDevicePropertyStreams: *u = kObjectID_Stream; return 0;
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
		break;
	case kObjectID_Stream:
		switch (a->mSelector) {
		case kAudioObjectPropertyBaseClass: *u = kAudioObjectClassID; return 0;
		case kAudioObjectPropertyClass: *u = kAudioStreamClassID; return 0;
		case kAudioObjectPropertyOwner: *u = kObjectID_Device; return 0;
		case kAudioStreamPropertyIsActive: *u = 1; return 0;
		case kAudioStreamPropertyDirection: *u = 0; return 0;                   /* output */
		case kAudioStreamPropertyTerminalType: *u = kAudioStreamTerminalTypeSpeaker; return 0;
		case kAudioStreamPropertyStartingChannel: *u = 1; return 0;
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
	if (o == kObjectID_Device && a->mSelector == kAudioDevicePropertyNominalSampleRate) {
		NEED(sizeof(Float64));
		return *(const Float64 *)d == kSampleRate ? 0 : kAudioHardwareIllegalOperationError;
	}
	if (o == kObjectID_Stream && (a->mSelector == kAudioStreamPropertyVirtualFormat ||
	    a->mSelector == kAudioStreamPropertyPhysicalFormat)) {
		NEED(sizeof(AudioStreamBasicDescription));
		const AudioStreamBasicDescription *f = d;
		return f->mSampleRate == kSampleRate && f->mChannelsPerFrame == kChannels ? 0 : kAudioDeviceUnsupportedFormatError;
	}
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
	*will = (op == kAudioServerPlugInIOOperationWriteMix);
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
