#!/bin/sh
# Tests and builds this repository in Docker. The sources are copied into a named volume, which keeps ESPHome's
# toolchain and build directories between runs; nothing on the host is mounted.
#   docker.sh                     copy, then run the offline tests (run.sh)
#   docker.sh record              the same while rewriting expected/, then copy expected/ back
#   docker.sh build NAME [OPTS]   copy, then compile examples/NAME.yaml with esphome OPTS and summarise the log
#                                 for another chip: docker.sh build minimal -s variant esp32s3
# UAC2_VOLUME names the volume (default esphome-uac2). Its download cache is a second volume, named with -cache
# at the end.
# An example that reads secrets takes them from secrets.yaml. Put one in the repository root (git ignores it); it is
# copied into the volume with the sources.
# The offline tests compile against the headers of the USB host library. They take them from USB=/path/in/the/volume
# (a copy of host/usb from espressif/esp-usb), else from a firmware build in the volume, else they fetch them from
# GitHub on the first run.
set -e
cd "$(dirname "$0")/../../.."
IMAGE=esphome/esphome:2026.11.0b4
VOLUME=${UAC2_VOLUME:-esphome-uac2}
export MSYS_NO_PATHCONV=1
in_volume() { docker run --rm -i --entrypoint sh -v "$VOLUME:/config" "$IMAGE" -c "$1"; }

tar cf - components $(ls secrets.yaml 2>/dev/null) -C examples $(cd examples && ls *.yaml 2>/dev/null) \
  | in_volume 'rm -rf /config/components/uac2 /config/components/usb_audio && tar xf - -C /config'

case "${1:-test}" in
test) in_volume "USB='$USB' sh /config/components/uac2/test/run.sh" ;;
record)
  in_volume "USB='$USB' RECORD=1 sh /config/components/uac2/test/run.sh"
  in_volume 'tar cf - -C /config/components/uac2/test expected' | tar xf - -C components/uac2/test ;;
build)
  NAME=$2; shift 2; LOG=$(mktemp)
  docker run --rm -v "$VOLUME:/config" -v "$VOLUME-cache:/root/.cache" "$IMAGE" "$@" compile "/config/$NAME.yaml" \
    > "$LOG" 2>&1 && echo "build: OK" || echo "build: FAILED"
  grep -E 'Successfully compiled|error:|^ERROR' "$LOG" | tail -20 || true
  echo "warnings in our files: $(grep -c -E '(uac_host\.c|uac_descriptors\.c|usb_audio[a-z_]*\.(cpp|h)):[0-9]+:[0-9]+: warning' "$LOG" || true)"
  # The build directory is named after the device in the YAML; the newest firmware is the one just built
  in_volume 'B=$(ls -t /config/.esphome/build/*/build/firmware.factory.bin | head -1); B=${B%/build/firmware.factory.bin}
    ls -l "$B/build/firmware.factory.bin"; sh /config/components/uac2/test/check_ram.sh "$B"' || true
  echo "log: $LOG" ;;
esac
