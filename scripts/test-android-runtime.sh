#!/usr/bin/env bash
set -euo pipefail
mkdir -p runtime-evidence
adb shell getprop ro.product.cpu.abilist | tee runtime-evidence/abis.txt
apk_files=(runtime-apk/*.apk)
if [[ ${#apk_files[@]} != 1 || ! -f "${apk_files[0]}" ]]; then
  echo 'Expected one exact APK'; exit 2
fi
adb install -r "${apk_files[0]}"
adb logcat -c
adb shell am start -W -n io.p2pmessenger.app/org.qtproject.qt.android.bindings.QtActivity \
  --es p2p_test_peer "${RECEIVER_INVITE_CODE:?}"
for attempt in $(seq 1 90); do
  adb logcat -d > runtime-evidence/logcat.txt
  if rg -q 'P2P_ANDROID_FRIEND_FLOW=PASS' runtime-evidence/logcat.txt; then
    rg 'P2P_ANDROID_' runtime-evidence/logcat.txt
    adb exec-out screencap -p > runtime-evidence/screen.png
    exit 0
  fi
  if rg -q 'P2P_ANDROID_.*FAIL|Fatal signal|FATAL EXCEPTION' runtime-evidence/logcat.txt; then
    break
  fi
  sleep 2
done
adb exec-out screencap -p > runtime-evidence/screen.png || true
rg 'P2P_ANDROID_|SSL|openssl|crypto|Keystore|JNI|Exception|Fatal' runtime-evidence/logcat.txt || true
echo 'Android APK runtime acceptance failed'
exit 1
