// Records through the UAC host driver the way ESPHome's microphone does: starts a capture interface, fills the IN
// transfers the driver submits the way a device does, and reads the audio back out of the driver's buffer. The USB
// host library and FreeRTOS are replaced by stubs.
// Usage: test_capture DESCRIPTOR.bin full|high RATE BITS CHANNELS
// Built once against the original driver, which only knows UAC 1.0 at Full Speed, and once (-DPATCHED) against the
// UAC 2.0 copy.
// The device runs on its own clock: 0.05 % fast for the first half and 0.05 % slow for the second, so now and then a
// packet carries one frame more or less than the nominal count. Every byte it sends is numbered. The driver's
// buffer must receive exactly the bytes of the packets that completed, in order, and nothing of a packet the host
// controller skipped. A reader that stops must cost whole transfers, never part of one.

#include <stdarg.h>
#include "uac_host.c"  // unity build: the functions under test are static
#include "host_usb.h"

#define BUFFER_BYTES 16000
#define MAX_IN_FLIGHT 16
#define TRANSFERS 20000
#define READ_EVERY 7     // transfers between two reads: about 20 ms, ESPHome's read period
#define SKIP_EVERY 997   // one packet in this many transfers is skipped by the host controller

static uac_iface_t *g_iface;             // the capture interface under test
static usb_transfer_t *g_in_flight[MAX_IN_FLIGHT];
static int g_in_flight_num;
static uint32_t g_submits;               // transfers the driver submitted
static uint32_t g_rx_done_events;
static uint32_t g_frame_bytes;
static uint32_t g_frames_q16;            // the device's clock: frames per packet, 16.16
static uint32_t g_frames_accum;
static uint32_t g_frames_min = UINT32_MAX, g_frames_max;
static uint32_t g_skipped;               // packets the host controller did not read
static uint64_t g_sent;                  // bytes the device put into packets that completed
static uint64_t g_received;              // bytes the driver put into its buffer
static bool g_check_content = true;

static void fail(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    printf("FAIL: ");
    vprintf(format, args);
    printf("\n");
    va_end(args);
    exit(1);
}

// Byte n of the audio the device sends
static uint8_t stream_byte(uint64_t n) { return (uint8_t)(n * 31 + (n >> 8)); }

static void on_ring_send(const uint8_t *data, size_t size)
{
    for (size_t i = 0; g_check_content && i < size; i++) {
        if (data[i] != stream_byte(g_received + i)) {
            fail("byte %llu in the driver's buffer is 0x%02X, the device sent 0x%02X", (unsigned long long)(g_received + i),
                 data[i], stream_byte(g_received + i));
        }
    }
    g_received += size;
}

static void test_on_submit(usb_transfer_t *transfer)
{
    if (!(transfer->bEndpointAddress & UAC_EP_DIR_IN)) {
        fail("the driver submitted an OUT transfer on a capture stream");
    }
    if (g_in_flight_num >= MAX_IN_FLIGHT) {
        fail("the driver has more than %d transfers in flight", MAX_IN_FLIGHT);
    }
    g_in_flight[g_in_flight_num++] = transfer;
    g_submits++;
}

static void on_event(uac_host_device_handle_t handle, const uac_host_device_event_t event, void *arg)
{
    if (event == UAC_HOST_DEVICE_EVENT_RX_DONE) {
        g_rx_done_events++;
    }
}

// The device fills the oldest transfer in flight and the host controller completes it. skip_packet: the one packet
// the host controller did not read in its (micro)frame, or -1. Returns the bytes the transfer carried.
static uint32_t complete_one(int skip_packet)
{
    if (g_in_flight_num == 0) {
        fail("no transfer is in flight");
    }
    usb_transfer_t *xfer = g_in_flight[0];
    g_in_flight_num--;
    memmove(g_in_flight, g_in_flight + 1, g_in_flight_num * sizeof(g_in_flight[0]));
    uint32_t total = 0;
    for (int i = 0; i < xfer->num_isoc_packets; i++) {
        usb_isoc_packet_desc_t *packet = &xfer->isoc_packet_desc[i];
        g_frames_accum += g_frames_q16;
        const uint32_t frames = g_frames_accum >> 16;
        g_frames_accum &= 0xFFFF;
        const uint32_t bytes = frames * g_frame_bytes;
        if ((int)bytes > packet->num_bytes) {
            fail("the device has %" PRIu32 " bytes for a packet the driver asked %d bytes of", bytes, packet->num_bytes);
        }
        if (i == skip_packet) {
            // What the device had for this (micro)frame is lost
            packet->status = USB_TRANSFER_STATUS_SKIPPED;
            packet->actual_num_bytes = 0;
            g_skipped++;
            continue;
        }
        g_frames_min = MIN(g_frames_min, frames);
        g_frames_max = MAX(g_frames_max, frames);
        // The host controller stores each packet where the one before it would have ended at its full size
        uint8_t *dest = xfer->data_buffer + i * packet->num_bytes;
        for (uint32_t k = 0; k < bytes; k++) {
            dest[k] = stream_byte(g_sent + k);
        }
        g_sent += bytes;
        packet->status = USB_TRANSFER_STATUS_COMPLETED;
        packet->actual_num_bytes = bytes;
        total += bytes;
    }
    xfer->actual_num_bytes = total;
    xfer->status = USB_TRANSFER_STATUS_COMPLETED;
    const uint32_t submits = g_submits;
    xfer->callback(xfer);
    if (g_submits != submits + 1) {
        fail("the driver submitted %" PRIu32 " transfers after one completed", g_submits - submits);
    }
    return total;
}

// One read, as ESPHome's microphone task makes them. Returns the bytes it got.
static uint32_t stream_read(void)
{
    static uint8_t buffer[BUFFER_BYTES];
    // The driver stores the count through a size_t pointer, which is wider than uint32_t on the test's host
    union {
        uint32_t count;
        size_t wide;
    } read = {.wide = 0};
    const esp_err_t err = uac_host_device_read(g_iface, buffer, sizeof(buffer), &read.count, 0);
    if (err != ESP_OK && read.wide != 0) {
        fail("a read returned 0x%X with %zu bytes", err, read.wide);
    }
    return (uint32_t)read.wide;
}

// Starts the first capture interface that accepts the format, set up the way ESPHome's hub opens one
static uac_iface_t *start_microphone(uac_device_t *uac_device, uint32_t rate, uint8_t bits, uint8_t channels)
{
    esp_err_t err = ESP_ERR_NOT_FOUND;
    int offset = 0;
    const usb_intf_desc_t *intf = GET_NEXT_INTERFACE_DESC(g_config, g_config->wTotalLength, offset);
    for (; intf != NULL; intf = GET_NEXT_INTERFACE_DESC(intf, g_config->wTotalLength, offset)) {
        if (intf->bInterfaceClass != USB_CLASS_AUDIO || intf->bInterfaceSubClass != UAC_SUBCLASS_AUDIOSTREAMING ||
                intf->bAlternateSetting != 0) {
            continue;
        }
        uac_iface_t *iface = NULL;
        if (uac_host_interface_add(uac_device, intf->bInterfaceNumber, &iface) != ESP_OK || iface->dev_info.type != UAC_STREAM_RX) {
            continue;
        }
        iface->state = UAC_INTERFACE_STATE_IDLE;
        iface->ringbuf = (RingbufHandle_t)1;
        iface->ringbuf_size = BUFFER_BYTES;
        iface->ringbuf_threshold = BUFFER_BYTES / 4;  // the driver's default
        iface->user_cb = on_event;
        const uac_host_stream_config_t stream_config = {.channels = channels, .bit_resolution = bits, .sample_freq = rate};
        err = uac_host_device_start(iface, &stream_config);
        if (err == ESP_OK) {
            const uac_iface_alt_t *alt = &iface->iface_alt[iface->cur_alt];
            printf("  interface %d alt %d: %d ch, %d-bit, endpoint 0x%02X mps %d interval %d\n", iface->dev_info.iface_num,
                   alt->alt_idx, alt->dev_alt_param.channels, alt->dev_alt_param.bit_resolution, alt->ep_addr, alt->ep_mps,
                   alt->interval);
            printf("  start result: OK (0x0)\n");
            return iface;
        }
    }
    printf("  start result: FAILED (0x%X)\n", err);
    exit(1);
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        printf("usage: test_capture DESCRIPTOR.bin full|high RATE BITS CHANNELS\n");
        return 2;
    }
    static uint8_t raw[8192];
    FILE *file = fopen(argv[1], "rb");
    const size_t len = fread(raw, 1, sizeof(raw), file);
    fclose(file);
    g_config = (const usb_config_desc_t *)raw;
    g_speed = strcmp(argv[2], "high") == 0 ? USB_SPEED_HIGH : USB_SPEED_FULL;
    const uint32_t rate = atoi(argv[3]);
    printf("%s: %zu bytes, %s speed\n", argv[1], len, argv[2]);

    s_uac_driver = calloc(1, sizeof(uac_driver_t));
    STAILQ_INIT(&s_uac_driver->uac_devices_tailq);
    STAILQ_INIT(&s_uac_driver->uac_ifaces_tailq);
    s_uac_driver->client_handle = (usb_host_client_handle_t)1;

    uac_device_t *uac_device = NULL;
    const esp_err_t err = _uac_host_device_add(1, (usb_device_handle_t)1, g_config, &uac_device);
    if (err != ESP_OK) {
        printf("  device rejected: 0x%X\n", err);
        return 1;
    }
    g_ring_on_send = on_ring_send;
    g_iface = start_microphone(uac_device, rate, atoi(argv[4]), atoi(argv[5]));

    // What the stream start must have put on the bus: every transfer, each spanning the same time at any packet rate
    const uac_iface_alt_t *alt = &g_iface->iface_alt[g_iface->cur_alt];
    const uint32_t packets_per_ms = (g_speed == USB_SPEED_HIGH && alt->interval >= 1 && alt->interval <= 4) ? 8 >> (alt->interval - 1) : 1;
    const int packets = CONFIG_UAC_NUM_PACKETS_PER_URB * packets_per_ms;
    if (g_submits != CONFIG_UAC_NUM_ISOC_URBS) {
        fail("the stream start submitted %" PRIu32 " transfers, %d were expected", g_submits, CONFIG_UAC_NUM_ISOC_URBS);
    }
    for (int i = 0; i < g_in_flight_num; i++) {
        const usb_transfer_t *xfer = g_in_flight[i];
        if (xfer->num_isoc_packets != packets || xfer->num_bytes != packets * alt->ep_mps || xfer->bEndpointAddress != alt->ep_addr) {
            fail("transfer %d has %d packets and %d bytes on endpoint 0x%02X; %d packets of %d bytes on 0x%02X were expected", i,
                 xfer->num_isoc_packets, xfer->num_bytes, xfer->bEndpointAddress, packets, alt->ep_mps, alt->ep_addr);
        }
        for (int j = 0; j < packets; j++) {
            if (xfer->isoc_packet_desc[j].num_bytes != alt->ep_mps) {
                fail("packet %d of transfer %d asks for %d bytes, the endpoint's packets hold %d", j, i,
                     xfer->isoc_packet_desc[j].num_bytes, alt->ep_mps);
            }
        }
    }
#ifdef PATCHED
    // The stubbed clock keeps 0 until the driver sets a rate, and runs at its power-on rate until then
    const uint32_t clock_hz = g_dev_clock_hz[alt->clock_id] ? g_dev_clock_hz[alt->clock_id] : DEV_CLOCK_POWER_ON_HZ;
    if (uac_device->bcd_adc == UAC2_BCD_ADC && clock_hz != rate) {
        fail("clock %d runs at %" PRIu32 " Hz after the stream start", alt->clock_id, clock_hz);
    }
#endif

    g_frame_bytes = alt->dev_alt_param.channels * alt->dev_alt_param.subframe_size;
    const uint32_t packets_per_sec = 1000 * packets_per_ms;
    uint32_t read_bytes = 0;
    for (int i = 0; i < TRANSFERS; i++) {
        const double clock = i < TRANSFERS / 2 ? 1.0005 : 0.9995;
        g_frames_q16 = (uint32_t)(rate * clock * 65536 / packets_per_sec);
        complete_one(i % SKIP_EVERY == SKIP_EVERY - 1 ? i % packets : -1);
        if (i % READ_EVERY == READ_EVERY - 1) {
            read_bytes += stream_read();
        }
    }
    read_bytes += stream_read();
    if (g_received != g_sent) {
        fail("the device sent %llu bytes in completed packets, the driver stored %llu", (unsigned long long)g_sent,
             (unsigned long long)g_received);
    }
    if (read_bytes != g_received || g_ring_fill != 0) {
        fail("the reader got %" PRIu32 " of %llu bytes and %zu are left in the buffer", read_bytes,
             (unsigned long long)g_received, g_ring_fill);
    }
    // No RX_DONE event is expected here: the reader polls, as ESPHome's does, and keeps the buffer below the threshold
    if (g_skipped == 0 || g_frames_min == g_frames_max) {
        fail("too little was checked: %" PRIu32 " skipped packets, %" PRIu32 " to %" PRIu32 " frames in a packet", g_skipped,
             g_frames_min, g_frames_max);
    }
    printf("  capture: %d transfers of %d packets, %" PRIu32 " to %" PRIu32 " frames in a packet, %" PRIu32 " packets skipped, %llu"
           " bytes sent and read\n", TRANSFERS, packets, g_frames_min, g_frames_max, g_skipped, (unsigned long long)g_sent);

    // The reader stops. The driver must ask for a read, then drop whole transfers and never overfill its buffer.
    g_check_content = false;
    const uint32_t events = g_rx_done_events;
    uint32_t dropped_transfers = 0;
    for (int i = 0; i < 400; i++) {
        const uint64_t before = g_received;
        const uint32_t carried = complete_one(-1);
        const uint64_t stored = g_received - before;
        if (stored != 0 && stored != carried) {
            fail("the driver stored %llu bytes of a transfer of %" PRIu32, (unsigned long long)stored, carried);
        }
        dropped_transfers += stored == 0;
        if (g_ring_fill > BUFFER_BYTES) {
            fail("the driver's buffer of %d bytes holds %zu", BUFFER_BYTES, g_ring_fill);
        }
    }
    if (dropped_transfers == 0 || g_rx_done_events == events) {
        fail("too little was checked: %" PRIu32 " transfers dropped, %" PRIu32 " RX_DONE events while the reader was stopped",
             dropped_transfers, g_rx_done_events - events);
    }
    // The reader comes back, and recording goes on
    stream_read();
    const uint64_t before = g_received;
    if (complete_one(-1) == 0 || g_received == before) {
        fail("the driver stored nothing after the reader came back");
    }
    printf("  reader stopped: %" PRIu32 " whole transfers dropped, buffer never above %d bytes, recording went on after a read\n",
           dropped_transfers, BUFFER_BYTES);
    printf("  stop: 0x%X\n", uac_host_device_stop(g_iface));
    printf("PASS\n");
    return 0;
}
