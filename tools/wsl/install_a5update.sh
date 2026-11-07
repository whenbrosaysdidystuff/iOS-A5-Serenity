#!/bin/bash
# One-time root image change for fast iOS-side testing:
#  - /usr/libexec/a5update + its launchd job (applies Desktop-built updates from the blackbox partition,
#    saves logs there; see userland/a5update/a5update.c)
#  - mediaserverd logs (os_log mirrored to stderr) to /private/var/mobile/Library/Logs/a5/mediaserverd.log
#  - A5Audio.driver refreshed from Desktop\a5-ios-port\userland\A5Audio.driver
# then a new Desktop\a5-ios-port\images\ios16-root.simg.   bash install_a5update.sh
# Nothing here touches the phone.
set -euo pipefail
WINHOME=/mnt/c/Users/$(cmd.exe /c 'echo %USERNAME%' 2>/dev/null | tr -d '\r')
U=$WINHOME/Desktop/a5-ios-port/userland
IMG=~/ios-root/ios16-root.img
WINOUT=$WINHOME/Desktop/a5-ios-port/images
MNT=/mnt/iosroot
[[ -f $U/a5update/a5update && -f $U/A5Audio.driver/A5Audio ]] || { echo "ERROR: $U files missing" >&2; exit 1; }
grep -qw hfsplus /proc/filesystems || sudo insmod ~/wsl-hfsplus/hfsplus.ko

echo "1/4 check + mount the root image read-write"
sudo umount /tmp/iosroot-ro 2>/dev/null || true
sudo fsck.hfsplus -fy "$IMG" | tail -2 || true
sudo mkdir -p $MNT
sudo mount -t hfsplus -o loop,force "$IMG" $MNT
trap 'sudo umount $MNT 2>/dev/null || true' EXIT

echo "2/4 install files"
sudo install -o 0 -g 0 -m 0755 "$U/a5update/a5update" $MNT/usr/libexec/a5update
D=$MNT/System/Library/Audio/Plug-Ins/HAL/A5Audio.driver
sudo mkdir -p $D
sudo install -o 0 -g 0 -m 0755 "$U/A5Audio.driver/A5Audio" $D/A5Audio
sudo install -o 0 -g 0 -m 0644 "$U/A5Audio.driver/Info.plist" $D/Info.plist
sudo install -d -o 501 -g 501 -m 0755 $MNT/private/var/mobile/Library/Logs/a5
sha256sum $MNT/usr/libexec/a5update $D/A5Audio

echo "3/4 launchd.plist: a5update job, mediaserverd log"
sudo python3 - $MNT/System/Library/xpc/launchd.plist <<'EOF'
import os, plistlib, sys
p = sys.argv[1]; st = os.stat(p)
d = plistlib.load(open(p, 'rb'))
jobs = d['LaunchDaemons']
jobs['/System/Library/LaunchDaemons/org.a5port.a5update.plist'] = {
    'Label': 'org.a5port.a5update', 'ProgramArguments': ['/usr/libexec/a5update'],
    'RunAtLoad': True, 'LaunchOnlyOnce': True, 'UserName': 'root', 'POSIXSpawnType': 'Interactive'}
n = 0
for job in jobs.values():
    if job.get('Label') == 'com.apple.mediaserverd':
        job['StandardErrorPath'] = '/private/var/mobile/Library/Logs/a5/mediaserverd.log'
        job['StandardOutPath'] = '/private/var/mobile/Library/Logs/a5/mediaserverd.log'
        env = job.setdefault('EnvironmentVariables', {})
        env.update({'OS_ACTIVITY_DT_MODE': 'YES', 'CFLOG_FORCE_STDERR': '1'})
        n += 1
plistlib.dump(d, open(p, 'wb'), fmt=plistlib.FMT_BINARY)
os.chown(p, st.st_uid, st.st_gid); os.chmod(p, st.st_mode & 0o7777)
print('a5update job added; mediaserverd jobs logging:', n)
EOF
sync
sudo umount $MNT
trap - EXIT
fsck.hfsplus -fn "$IMG" | tail -2

echo "4/4 sparse image -> $WINOUT (several minutes)"
img2simg "$IMG" ~/ios-root/ios16-root.simg
cp ~/ios-root/ios16-root.simg "$WINOUT/ios16-root.simg"
ls -la "$WINOUT/ios16-root.simg"
echo "DONE: images\\ios16-root.simg has a5update"
