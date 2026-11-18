#!/bin/sh
# Offline tests of the driver copy. Builds it natively against stubs of the USB host library and FreeRTOS, replays
# captured descriptors through it and compares what it prints with expected/. RECORD=1 rewrites expected/.
# Runs inside the ESPHome image, on the volume docker.sh fills; see docker.sh.
set -e
cd "$(dirname "$0")"
# Only the headers and usb_helpers.c of the USB host library are used. Without USB=, take them from a firmware build
# in the volume, and without one fetch the release that expected/ was recorded with.
USB_COMMIT=babe3b1523e7b986952b9cbdb4f6caa8e4e65497 # espressif/usb 1.4.1
if [ -z "$USB" ]; then
  USB=$(ls -d /config/.esphome/build/*/managed_components/espressif__usb 2>/dev/null | head -1)
  if [ -z "$USB" ]; then
    if [ ! -f /config/esp-usb/host/usb/src/usb_helpers.c ]; then
      echo "Fetching the USB host library from espressif/esp-usb"
      rm -rf /config/esp-usb
      git init -q /config/esp-usb
      git -C /config/esp-usb fetch -q --depth 1 https://github.com/espressif/esp-usb "$USB_COMMIT"
      git -C /config/esp-usb checkout -q FETCH_HEAD
    fi
    USB=/config/esp-usb/host/usb
  fi
fi
[ -f "$USB/src/usb_helpers.c" ] || { echo "No USB host library at '$USB'"; exit 1; }
FLAGS="-std=gnu17 -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Istubs \
  -I$USB/include -I../usb_host_uac/include \
  -DUSB_HOST_UAC_VER_MAJOR=1 -DUSB_HOST_UAC_VER_MINOR=5 -DUSB_HOST_UAC_VER_PATCH=0"

build() { # NAME SOURCE DRIVER_DIR [-DPATCHED]
  gcc $FLAGS $4 -I"$3" "$2" "$USB/src/usb_helpers.c" -lm -o "/tmp/$1" 2> "/tmp/$1.warn" || { cat "/tmp/$1.warn"; exit 1; }
}
replay() { # NAME ARGS...
  name=$1; shift
  /tmp/parse_new "$@" > "/tmp/$name.txt"
  if [ -n "$RECORD" ]; then mkdir -p expected; cp "/tmp/$name.txt" "expected/$name.txt"; fi
  diff "expected/$name.txt" "/tmp/$name.txt" > /dev/null || { echo "FAIL: $name differs from expected/$name.txt"; exit 1; }
  echo "ok: $name"
}

build parse_orig test_parse.c orig
build parse_new test_parse.c ../usb_host_uac -DPATCHED
[ "$(grep -c warning: /tmp/parse_new.warn)" -le "$(grep -c warning: /tmp/parse_orig.warn)" ] \
  || { echo "FAIL: the driver copy compiles with more warnings than the original"; cat /tmp/parse_new.warn; exit 1; }

replay ca7_high_48000 descriptors/0bda_4f25.bin high 48000 16 2
replay ca7_high_44100 descriptors/0bda_4f25.bin high 44100 16 2
replay modi_high_48000 descriptors/30be_100c.bin high 48000 16 2
replay modi_full_48000 descriptors/30be_100c.bin full 48000 16 2
replay realtek_high_48000 descriptors/0b05_1b9b.bin high 48000 16 2
replay webcam_full descriptors/046d_085b.bin full

# A UAC 1.0 device must come out of the driver copy exactly as it does out of the original
/tmp/parse_orig descriptors/046d_085b.bin full > /tmp/webcam_orig.txt
grep -v -e "UAC version" -e "clock 0, feedback" /tmp/webcam_full.txt | diff /tmp/webcam_orig.txt - > /dev/null \
  || { echo "FAIL: the webcam (UAC 1.0) differs between the original and the driver copy"; exit 1; }
echo "ok: webcam identical to the original driver"

# A Feature Unit that claims controls of 0 bytes must be skipped, not divided by. The input is the webcam's
# descriptor with that one byte changed.
/tmp/parse_new descriptors/046d_085b_fu_control_size_0.bin full > /tmp/fu_zero.txt 2>&1 \
  || { echo "FAIL: a Feature Unit with bControlSize 0 stops the driver copy (exit $?)"; exit 1; }
grep -q -F "Invalid Feature Unit descriptor" /tmp/fu_zero.txt && grep -q "feature unit 0 vol 0x00 mute 0x00" /tmp/fu_zero.txt \
  || { echo "FAIL: a Feature Unit with bControlSize 0 is not skipped"; exit 1; }
echo "ok: Feature Unit with bControlSize 0 is skipped"

# Playback through the driver's write path
build stream_orig test_stream.c orig
build stream_new test_stream.c ../usb_host_uac -DPATCHED
[ "$(grep -c warning: /tmp/stream_new.warn)" -le "$(grep -c warning: /tmp/stream_orig.warn)" ] \
  || { echo "FAIL: test_stream compiles with more warnings against the driver copy"; cat /tmp/stream_new.warn; exit 1; }
capture() { # FILE COMMAND...
  file=$1; shift
  "$@" > "$file" || { tail -5 "$file"; echo "FAIL: $*"; exit 1; }
}
expect() { # FILE TEXT...
  file=$1; shift
  for text in "$@"; do grep -q -F -- "$text" "$file" || { echo "FAIL: $file lacks: $text"; exit 1; }; done
}

# A UAC 1.0 speaker must get the same requests and the same packets from the driver copy as from the original
for rate in 44100 48000; do
  capture /tmp/uac1_orig_$rate.txt /tmp/stream_orig @uac1 full $rate 16 2 sizes
  capture /tmp/uac1_new_$rate.txt /tmp/stream_new @uac1 full $rate 16 2 sizes
  diff /tmp/uac1_orig_$rate.txt /tmp/uac1_new_$rate.txt > /dev/null \
    || { echo "FAIL: the UAC 1.0 speaker at $rate differs between the original and the driver copy"; exit 1; }
  expect /tmp/uac1_new_$rate.txt "start result: OK"
  echo "ok: UAC 1.0 speaker $rate identical to the original driver"
done
expect /tmp/uac1_new_44100.txt "sizes: 120000 packets in 40000 transfers, 21168000 bytes" "176 bytes: 108000" "180 bytes: 12000"
expect /tmp/uac1_new_48000.txt "sizes: 120000 packets in 40000 transfers, 23040000 bytes"
capture /tmp/ca7_sizes.txt /tmp/stream_new descriptors/0bda_4f25.bin high 44100 16 2 sizes
expect /tmp/ca7_sizes.txt "sizes: 120000 packets in 5000 transfers, 2646000 bytes" "20 bytes: 58500" "24 bytes: 61500"
echo "ok: CA7 High-Speed 44100 sizes"

# The driver's playback totals must match what it really took, with and without feedback, at both speeds
progress() { # TEXT ARGS...
  text=$1; shift
  capture /tmp/progress.txt /tmp/stream_new "$@"
  expect /tmp/progress.txt "$text" "PASS"
  echo "ok: progress $* ($(grep 'progress:' /tmp/progress.txt | sed 's/^ *progress: //'))"
}
progress "feedback moving" descriptors/30be_100c.bin high 48000 16 2 progress
progress "feedback moving" descriptors/30be_100c.bin high 44100 16 2 progress
progress "feedback moving" descriptors/30be_100c.bin full 48000 16 2 progress
progress "wrapped yes" descriptors/30be_100c.bin full 44100 16 2 progress wrap
progress "feedback none, wrapped no, capture interfaces refused 2" descriptors/0bda_4f25.bin high 48000 16 2 progress
progress "wrapped yes" descriptors/0bda_4f25.bin high 44100 16 2 progress wrap
progress "feedback none" @uac1 full 44100 16 2 progress

# A UAC 2.0 stream start: read the clock, change its rate only when it differs, wait until the device calls it valid
capture /tmp/clock.txt /tmp/stream_new descriptors/30be_100c.bin high 48000 16 2 clock
expect /tmp/clock.txt "PASS"
echo "ok: clock requests at a stream start"
capture /tmp/clock_novalid.txt /tmp/stream_new descriptors/30be_100c.bin high 48000 16 2 clock novalid
expect /tmp/clock_novalid.txt "PASS"
echo "ok: clock requests for a clock without a validity control"

# Packets the host controller skipped: the driver names each run of them and where in its transfer it sat
capture /tmp/skips.txt /tmp/stream_new descriptors/0bda_4f25.bin high 48000 16 2 skips
expect /tmp/skips.txt "Playback: skipped packets 0 to 1 of a transfer of 24" \
  "Playback: skipped packets 10 to 12 of a transfer of 24" \
  "Playback: skipped packets 0 to 0 of a transfer of 24" \
  "Playback: skipped packets 20 to 20 of a transfer of 24" \
  "Playback: skipped packets 23 to 23 of a transfer of 24" \
  "of 7992 packets, 8 were skipped and 0 failed" "PASS"
[ "$(grep -c 'Playback: skipped packets' /tmp/skips.txt)" = 5 ] \
  || { echo "FAIL: the driver names $(grep -c 'Playback: skipped packets' /tmp/skips.txt) runs of skipped packets, 5 were made"; exit 1; }
echo "ok: positions of skipped packets"

# Recording through the driver's read path. A UAC 1.0 microphone must come out of the driver copy as it does out of
# the original; a UAC 2.0 one at High Speed has only the copy to go through.
build capture_orig test_capture.c orig
build capture_new test_capture.c ../usb_host_uac -DPATCHED
capture /tmp/mic1_orig.txt /tmp/capture_orig descriptors/046d_085b.bin full 16000 16 2
capture /tmp/mic1_new.txt /tmp/capture_new descriptors/046d_085b.bin full 16000 16 2
diff /tmp/mic1_orig.txt /tmp/mic1_new.txt > /dev/null \
  || { echo "FAIL: the webcam's microphone differs between the original and the driver copy"; exit 1; }
expect /tmp/mic1_new.txt "PASS"
echo "ok: UAC 1.0 microphone identical to the original driver ($(grep 'capture:' /tmp/mic1_new.txt | sed 's/^ *capture: //'))"
for rate in 44100 48000; do
  capture /tmp/ca7_capture_$rate.txt /tmp/capture_new descriptors/0bda_4f25.bin high $rate 16 2
  expect /tmp/ca7_capture_$rate.txt "PASS"
  echo "ok: CA7 capture at High Speed, $rate Hz ($(grep 'capture:' /tmp/ca7_capture_$rate.txt | sed 's/^ *capture: //'))"
done

# The counting in ESPHome's speaker (the local usb_audio copy), against a model of the driver
mkdir -p /tmp/inc/esphome/components
ln -sfn "$(cd ../../usb_audio && pwd)" /tmp/inc/esphome/components/usb_audio
g++ -std=gnu++20 -Wall -Wno-unused-function -DHOST_TEST_OWN_DELAY -Iesphome_stubs -I/tmp/inc -Istubs -I"$USB/include" \
  -I../usb_host_uac/include test_speaker.cpp ../../usb_audio/speaker/usb_audio_speaker.cpp -o /tmp/test_speaker \
  2> /tmp/test_speaker.warn || { cat /tmp/test_speaker.warn; exit 1; }
capture /tmp/speaker.txt /tmp/test_speaker
expect /tmp/speaker.txt "PASS"
grep '^ok:' /tmp/speaker.txt | sed 's/^ok: /ok: speaker: /'

# STOCK=1 also runs the speaker tests against ESPHome's unchanged usb_audio in this image and prints what it does.
# It is not part of the result: 2026.11.0b4's speaker fails the volume change test.
if [ -n "$STOCK" ]; then
  mkdir -p /tmp/inc_stock/esphome/components
  ln -sfn /esphome/esphome/components/usb_audio /tmp/inc_stock/esphome/components/usb_audio
  g++ -std=gnu++20 -Wall -Wno-unused-function -DHOST_TEST_OWN_DELAY -DSTOCK_SPEAKER -Iesphome_stubs -I/tmp/inc_stock \
    -Istubs -I"$USB/include" -I../usb_host_uac/include test_speaker.cpp \
    /esphome/esphome/components/usb_audio/speaker/usb_audio_speaker.cpp -o /tmp/test_speaker_stock \
    2> /tmp/test_speaker_stock.warn || { cat /tmp/test_speaker_stock.warn; exit 1; }
  echo "stock ESPHome speaker: $(/tmp/test_speaker_stock | tail -1)"
fi

echo "ALL PASSED"
