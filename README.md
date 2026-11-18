# USB Audio Class 2.0 for ESPHome

ESPHome's `usb_audio` works with USB Audio Class 1.0 devices only, because the Espressif driver it builds on
(`espressif/usb_host_uac`) refuses anything else. The two external components here make it play to and record from
UAC 2.0 devices on an ESP32 with a USB host port, including DACs that run on their own clock and steer the host
through a feedback endpoint.

This is experimental. It has run on one board with two DACs, and it is published so that others can try their own
boards and devices: [Reporting what you find](#reporting-what-you-find) says what helps.

The aim is for this repository to become unnecessary: its parts are being offered to Espressif and to ESPHome
(see [Upstream](#upstream)). Until they are released there, this is a way to use them.

## Status

| | Run on hardware |
| --- | --- |
| Chip | ESP32-S31 (ESP32-S31-Function-CoreBoard-1), High-Speed USB host port |
| Software | ESPHome 2026.11.0b4, ESP-IDF 6.1.0, `espressif/usb` 1.4.1 |
| Devices | Schiit Modi+ (30BE:100C, asynchronous with a feedback endpoint), CUBILUX CA7 (0BDA:4F25) |
| Playback | CA7: 44.1, 48 and 96 kHz at 16 bits, 48 kHz at 24 bits (see [Formats tested](#formats-tested)). Modi+: 48 kHz, 16-bit |
| Recording | 48 kHz, 16-bit, stereo, from the CA7's line input |

- **Playback** has had the most use, nearly all of it at 48 kHz, 16-bit: the longest run was 3 hours 39 minutes of
  music with no packet lost. The other formats had runs of about two minutes each. Playback has one known fault
  that stops it (see [Known issues](#known-issues)).
- **Recording** has had little: the CA7's line input, sent to a server through Sendspin's source role and heard on
  another player, for 45 minutes, 32 of them while the CA7 was also playing. Sendspin's source dropped three chunks
  of audio in that time, about one every 18 minutes. Nothing counts lost recording packets yet.
- **Not run on hardware:** the ESP32-P4, Full-Speed ports (ESP32-S2, ESP32-S3), ESP-IDF 5.x, devices behind a hub,
  and the formats listed as untested under [Formats tested](#formats-tested).
  [minimal.yaml](examples/minimal.yaml) compiles for the ESP32-P4 and the ESP32-S3 with ESP-IDF 5.5.6, which is all
  that is known about them.

UAC 1.0 devices go through the same code as in the original driver; the offline tests compare the two on a UAC 1.0
speaker and a webcam microphone.

## Use

Start from a configuration that already works on your board, with ESPHome 2026.11 or later, and add:

```yaml
external_components:
  - source: github://chrisuthe/esphome-usb-audio-uac2
    components: [uac2, usb_audio]

esp32:
  framework:
    type: esp-idf
    # ESPHome hides the USB driver's own lines unless this is INFO: which interface it starts, the rate it sets on
    # the device's clock, and its warnings about skipped packets
    log_level: INFO

usb_audio:

uac2:
  # Only for a device with several playback interfaces, when the first one is not the output you want
  # speaker_interface: 4

speaker:
  - platform: usb_audio
    sample_rate: 48000
    bits_per_sample: 16bit
    num_channels: 2

# Only to record from the device
microphone:
  - platform: usb_audio
    sample_rate: 48000
    bits_per_sample: 16bit
    num_channels: 2
```

The device must offer the exact format of the `speaker` and of the `microphone`; on a mismatch the log lists the
formats it does offer. Put a `resampler` speaker in front of the `speaker` for sources at other rates.

The `usb_audio` here replaces the one in your ESPHome. It is a copy of ESPHome 2026.11.0b4's, so it lacks whatever
later ESPHome versions change in theirs.

[examples](examples) has three configurations:

- [minimal.yaml](examples/minimal.yaml): the block above as a whole file, for any chip. It has no network and
  nothing that plays, so it is a starting point and a build check.
- [esp32-s31-function-coreboard.yaml](examples/esp32-s31-function-coreboard.yaml): the configuration playback was
  tested with, a Sendspin player.
- [esp32-s31-function-coreboard-line-in.yaml](examples/esp32-s31-function-coreboard-line-in.yaml): the same with the
  device's input offered to the server as a Sendspin source.

`uac2` has one option:

- **speaker_interface** (optional, 0 to 255): the AudioStreaming interface number to play on. `usb_audio` opens the
  first playback interface a device announces. The CUBILUX CA7 has two, and its headphone jack is interface 4. The
  log lists a device's interfaces when it connects.

There is no such option for recording yet: `usb_audio` opens the first recording interface a device announces.

## Which devices

Not every UAC 2.0 device. The driver follows the specification and has no per-device workarounds, so a device that
needs one (Linux carries a long list of them) may not work. Apart from the two DACs above, a device should play when
all of this holds:

- It offers mono or stereo PCM at a rate from 8 to 96 kHz, and the `speaker` is configured with exactly one of the
  formats it offers. A device that only offers more channels, DSD or faster rates has nothing the speaker can select.
- It is adaptive or synchronous, or it is asynchronous with a feedback endpoint of its own. A device that expects the
  host to take the rate from its recording stream (implicit feedback) is sent the nominal rate and will drift.
- Its clock source is fixed, or it is the first input of its clock selector. The driver does not ask the device which
  input is selected and does not switch it.

The same limits on format and clock apply to recording.

## Formats tested

ESPHome sets the outer limits, and in 2026.11.0b4 they are narrower than the `usb_audio` options suggest:

- The `usb_audio` speaker takes 8 to 96 kHz, 8 to 32 bits, mono or stereo.
- The `resampler` speaker stops at 48 kHz.
- A `speaker_source` media player only accepts a speaker that takes 16-bit audio, so a 24- or 32-bit USB speaker
  needs a `resampler` speaker in front of it.
- Sendspin's media source takes 16 to 96 kHz and 16 or 24 bits.

Together: 24 and 32 bits are possible up to 48 kHz only, rates above 48 kHz at 16 bits only, and 96 kHz at 24 bits
cannot be configured.

These were played on the CUBILUX CA7's headphone jack, as a Sendspin player on the ESP32-S31 with a Music Assistant
server. Each run was listened to and checked in the log, for 80 to 140 seconds unless the table says otherwise.
No run had a skipped packet, a failed packet or a stall.

| On USB | Stream from the server | Resampler speaker | Result |
| --- | --- | --- | --- |
| 48 kHz, 16-bit | 48 kHz, 16-bit | passes it through | clean; longest run 3 h 39 min |
| 48 kHz, 24-bit | 48 kHz, 16-bit | widens to 24 bits | clean |
| 48 kHz, 24-bit | 48 kHz, 24-bit | passes it through | clean |
| 96 kHz, 16-bit | 96 kHz, 16-bit | none | clean |
| 44.1 kHz, 16-bit | 44.1 kHz, 16-bit | none | clean |
| 44.1 kHz, 16-bit | 48 kHz, 16-bit | converts the rate | plays, but Sendspin lost sync once in 100 s |
| 44.1 kHz, 24-bit | 48 kHz, 16-bit | converts the rate and widens | plays, but Sendspin lost sync 4 times in 90 s |

What follows from it:

- **For 44.1 kHz, let the server deliver 44.1 kHz.** Converting 48 to 44.1 kHz on the board loses sync
  (see [Known issues](#known-issues)).
- **Check the log when a format stays silent.** The CA7 does not offer 88.2 kHz, and the log said so:
  `Speaker interface 4 does not support requested format`, followed by the formats it does offer (44.1, 48, 96,
  176.4, 192, 352.8 and 384 kHz at 16 bits). A UAC 2.0 device's rates are not in its configuration descriptor; it
  answers a request for them, so only a real connection shows the list.
- **A refused format has an after-effect.** With no speaker to play to, Sendspin's player overran its buffer and
  closed its connection several times, and the server then took two minutes to reconnect to the next, working
  firmware.

Untested: 32 bits (the CA7 has no 32-bit setting; the Modi+ does and comes next), every format on the Modi+ other
than 48 kHz, 16-bit, and mono, 8 bits and rates below 44.1 kHz, which neither DAC offers.

For a real 24-bit stream at 48 kHz, the player has to ask the server for 24 bits; these are the parts of
[the example](examples/esp32-s31-function-coreboard.yaml) that change:

```yaml
speaker:
  - platform: usb_audio
    id: usb_speaker
    sample_rate: 48000
    bits_per_sample: 24bit
    num_channels: 2

  - platform: resampler
    id: media_resampling_speaker
    output_speaker: usb_speaker
    sample_rate: 48000
    bits_per_sample: 24

media_source:
  - platform: sendspin
    id: sendspin_source
    bits_per_sample: 24
```

For 96 kHz (or 44.1 kHz), the server has to deliver that rate and the `resampler` speaker is left out:

```yaml
speaker:
  - platform: usb_audio
    id: usb_speaker
    sample_rate: 96000
    bits_per_sample: 16bit
    num_channels: 2

media_source:
  - platform: sendspin
    id: sendspin_source
    sample_rate: 96000

media_player:
  - platform: speaker_source
    id: usb_media_player
    name: "Player"
    media_pipeline:
      speaker: usb_speaker
      format: FLAC
      num_channels: 2
      sample_rate: 96000
      sources:
        - sendspin_source
```

## Reporting what you find

Reports from other boards and devices are the point of publishing this, whether it worked or not. Please open an
issue on this repository with:

- the chip or board, and the ESPHome version;
- the USB device: its name and its vendor and product ID;
- the log from the moment the device connects until a few seconds into playing or recording, with `log_level: INFO`
  as in the block above;
- what you heard: clean audio, clicks, silence, and after how long if it changed.

A device's configuration descriptor is enough to add it to the offline tests, also when it does not work.
[dump_desc.py](components/uac2/test/dump_desc.py) writes it to a file on a computer the device is plugged into
(it needs Python with `pyusb` and `libusb-package`, and has been used on Windows). Attach the file to the issue.

## What the components change

**`uac2`** replaces the USB driver that `usb_audio` builds with the copy in
[components/uac2/usb_host_uac](components/uac2/usb_host_uac). That copy is `espressif/usb_host_uac` 1.5.0 with
Espressif's later fixes (esp-usb `master` at 1f3d4ce) and these changes:

- UAC 2.0 descriptors: the AudioControl header, clock sources behind selectors and multipliers, AudioStreaming
  formats, Feature Units.
- The sample rate is set on the clock source: the driver reads the rates it offers, changes the rate only when it
  differs, and waits until the device calls the clock valid.
- Volume and mute through the UAC 2.0 requests.
- Packet pacing from the bus speed and the endpoint's `bInterval`. The original assumes 1000 packets a second; a
  High-Speed endpoint takes up to 8000.
- A feedback endpoint for asynchronous devices. The driver sends as many frames per packet as the device asks for,
  kept in 16.16 fixed point.
- `uac_host_device_get_tx_progress()`: what playback has really taken from the buffer, and what the next transfer
  needs.
- A crash in the failure path of `uac_host_device_open()` is fixed
  ([esp-usb issue 650](https://github.com/espressif/esp-usb/issues/650)).
- Once a second the driver logs a warning if playback packets were skipped or failed, if a transfer had to wait for
  data, or if the device's feedback moved far from the nominal rate.
- It sets `CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE` to 4096 (UAC 2.0 configuration descriptors are long) and keeps
  eight sample rates for each alternate setting.
- On the chips with a High-Speed port (ESP32-P4, ESP32-S31) it selects the USB FIFO split that favours playback
  (`CONFIG_USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT`). A UAC 2.0 playback endpoint is sized for the device's fastest
  sample rate and can exceed the 512 bytes the default split gives it. A split set in the configuration's own
  `sdkconfig_options` is left alone. On Full-Speed chips ESPHome's `usb_host` already sizes the FIFO for the audio
  streams.

Recording goes through the original driver's receive path. What is new for it is the descriptor parsing and the
sample rate request above.

**`usb_audio`** is ESPHome 2026.11.0b4's component with a changed speaker:

- It counts what is in the driver's buffer from the driver's own reports. The stock speaker assumes a fixed transfer
  size, which no longer holds once a device's feedback changes the packet sizes.
- A volume change while playing no longer stops the audio for good. The stock speaker counts a write that is still
  waiting as written, and after a failed write its count wraps.

The microphone is unchanged.

### The lost packets at High Speed

The USB host library halts an isochronous channel after every transfer and enables it again in its interrupt
handler, which has one microframe (125 µs) for that at High Speed. The handler runs from flash, and on the ESP32-S31
it is sometimes too slow: the next transfer then loses its first packet. Here that cost 61 packets a minute.
The details are in [esp-usb issue 654](https://github.com/espressif/esp-usb/issues/654), the fix for the library in
[esp-usb pull request 656](https://github.com/espressif/esp-usb/pull/656).

Until that is released, the driver copy places the handler's code in RAM with its own linker fragment
(`CONFIG_UAC_USB_INTERRUPT_PATH_IN_RAM`, on by default, about 11 KB of RAM). With it, no packet was lost in
4,387,645 transfers. The fragment names the library as ESP-IDF 6 builds it (`libespressif__usb.a`,
`libesp_hal_usb.a`). Where the USB host library is part of ESP-IDF itself (ESP-IDF 5.x), it has no effect; a
High-Speed port on such a build (ESP32-P4) is untested and may lose packets.
[check_ram.sh](components/uac2/test/check_ram.sh) reads a build's symbol table and says where the code ended up.

## Known issues

- **Playback can stop for good.** The USB host controller stops sending in the middle of a stream, and nothing gets
  through to the device until the board is restarted. With the Modi+ this happened about every 75 minutes of music on
  average, sometimes after a few minutes; with the CA7 once in more than 15 hours. The cause is not known, and the
  RAM placement above does not remove it.
- **Converting 48 kHz to 44.1 kHz on the board loses sync.** With a `resampler` speaker converting the rate in front
  of the USB speaker, Sendspin lost sync every 10 to 70 seconds, each time about 80 to 90 ms off. With the server
  delivering 44.1 kHz and no `resampler` speaker there was no loss. Where in the resampler path it arises is not
  known.
- The USB speaker's buffer is ESPHome's 6,400 bytes for every format: 33 ms of audio at 48 kHz, 16-bit, but 17 ms at
  96 kHz. The driver's once-a-second warning then counts more transfers that waited for data (7 in 140 seconds at
  96 kHz, none at 48 kHz after the start). `buffer_size` on the speaker raises it; a larger value is untested.
- Nothing reports recording packets the host controller skipped, and the recording interface cannot be chosen.
- A feedback endpoint that asks to be read less often than every 32 (micro)frames (`bInterval` above 6) is ignored,
  and the device is sent the nominal rate.
- Two playback reports that arrive out of order can put an older transfer size back until the driver's next attempt
  corrects it.
- A writer that is held up for a whole stop and start of the stream can write into the restarted stream early.

## Tests

[components/uac2/test](components/uac2/test) builds the driver and the speaker natively against stand-ins for the
USB host library, FreeRTOS and ESPHome, and replays descriptors captured from four devices through them. It needs
Docker:

```sh
sh components/uac2/test/docker.sh
```

It ends with `ALL PASSED`. On its first run it fetches the headers of Espressif's USB host library from GitHub. The
header of [docker.sh](components/uac2/test/docker.sh) says how it builds a firmware from an example, for example
`sh components/uac2/test/docker.sh build minimal -s variant esp32s3`.

## Upstream

- UAC 2.0 in Espressif's driver has been asked for since 2025:
  [esp-usb issue 173](https://github.com/espressif/esp-usb/issues/173).
- The crash in `uac_host_device_open()`: [esp-usb issue 650](https://github.com/espressif/esp-usb/issues/650).
- The lost packets: [esp-usb issue 654](https://github.com/espressif/esp-usb/issues/654) and
  [pull request 656](https://github.com/espressif/esp-usb/pull/656).
- ESPHome takes the driver from Espressif's component registry, so its `usb_audio` can only follow once Espressif
  has released the driver side.

## Licence

- [components/uac2](components/uac2) is under the Apache License 2.0. The driver in it is Espressif's, with the
  changes listed above; the files changed are `uac_host.c`, `include/usb/uac_host.h`, `Kconfig`, `CMakeLists.txt`,
  and `linker.lf` is new.
- [components/usb_audio](components/usb_audio) is ESPHome's, with the changes listed above, under ESPHome's licence:
  GPLv3 for the C++ files and MIT for the Python files.
