// Plays through the UAC host driver the way ESPHome's speaker does: writes into the driver's buffer and completes the
// transfers it submits, with the USB host library and FreeRTOS replaced by stubs.
// Usage: test_stream DESCRIPTOR.bin|@uac1 full|high RATE BITS CHANNELS sizes|progress|clock|skips [wrap|novalid]
//   sizes     prints how many packets of each size the driver sent, for comparing two builds of the driver
//   progress  (driver copy only) checks the totals of uac_host_device_get_tx_progress() against what the driver
//             really took, while the writer starves now and then and the device's feedback moves;
//             wrap starts the totals just below 2^32, so they wrap during the run
//   clock     (driver copy only) checks which clock requests a UAC 2.0 stream start makes and in which order;
//             novalid takes the validity control away from the device's clock sources
//   skips     (driver copy only) completes some transfers with packets the host controller skipped; the driver
//             must say where in the transfer each run of them sat
// @uac1 is the hand-built UAC 1.0 speaker in uac1_speaker_desc.h.
// Built once against the original driver and once (-DPATCHED) against the UAC 2.0 copy.

#include <stdarg.h>
#include "uac_host.c"  // unity build: the functions under test are static
#include "host_usb.h"
#include "uac1_speaker_desc.h"

#define BUFFER_BYTES 6400  // ESPHome's default speaker buffer
#define MAX_IN_FLIGHT 16
#define SIZES_PACKETS 120000
#define MAX_PACKET_BYTES 1024

static uac_iface_t *g_iface;             // the playback interface under test
static uint32_t g_submits;               // data transfers the driver submitted
static usb_transfer_t *g_in_flight[MAX_IN_FLIGHT];
static int g_in_flight_num;
static usb_transfer_t *g_fb_xfer;        // the feedback transfer the driver waits on, if the stream has one
static uint8_t g_zeros[BUFFER_BYTES];
// What the first SIZES_PACKETS packets carried
static uint32_t g_packets, g_packet_transfers, g_packet_bytes, g_packet_hash = 2166136261u;
static uint32_t g_size_count[MAX_PACKET_BYTES + 1];

#ifdef PATCHED
static bool g_progress_mode;
static bool g_wrap;                      // start the driver's totals just below 2^32
static uint32_t g_rate;
static uint32_t g_tick;
static uint32_t g_count;                 // bytes in the driver's buffer, counted the way ESPHome's speaker counts them
static uac_host_tx_progress_t g_seen;    // the driver's totals when last read
static uint32_t g_submits_seen;          // g_submits at that moment
static uint32_t g_expected_next;         // what the driver last said its next transfer needs
static bool g_expected_valid;            // false from a feedback change until the driver says it again
static uint32_t g_taken, g_next_checks, g_waits;

static void fail(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    printf("FAIL: tick %" PRIu32 ": ", g_tick);
    vprintf(format, args);
    printf("\n");
    va_end(args);
    exit(1);
}

static uac_host_tx_progress_t read_progress(void)
{
    uac_host_tx_progress_t progress;
    const esp_err_t err = uac_host_device_get_tx_progress(g_iface, &progress);
    if (err != ESP_OK) {
        fail("uac_host_device_get_tx_progress returned 0x%X", err);
    }
    return progress;
}

// Compares the driver's totals with what the stubs saw it do. tx_done: called for the driver's TX_DONE event.
static void check(bool tx_done)
{
    const uac_host_tx_progress_t progress = read_progress();
    const uint32_t transfers = progress.transfers_taken - g_seen.transfers_taken;
    const uint32_t bytes = progress.bytes_taken - g_seen.bytes_taken;
    const uint32_t submits = g_submits - g_submits_seen;
    if (transfers != submits) {
        fail("transfers_taken grew by %" PRIu32 " while the driver submitted %" PRIu32 " transfers", transfers, submits);
    }
    if (tx_done && submits == 0) {
        // The driver left its transfer waiting for more data
        if (bytes != 0) {
            fail("bytes_taken grew by %" PRIu32 " for a transfer left waiting", bytes);
        }
        g_waits++;
    }
    g_count -= bytes;
    g_taken += transfers;
    g_seen = progress;
    g_submits_seen = g_submits;
    if (g_count != g_ring_fill) {
        fail("the count says %" PRIu32 " bytes, the driver's buffer holds %zu", g_count, g_ring_fill);
    }
    if (tx_done) {
        g_expected_next = progress.next_transfer_bytes;
        g_expected_valid = true;
    }
}
#endif

static void test_on_submit(usb_transfer_t *transfer)
{
    if (transfer->bEndpointAddress & UAC_EP_DIR_IN) {
        g_fb_xfer = transfer;
        return;
    }
    g_submits++;
#ifdef PATCHED
    if (g_progress_mode) {
        // The driver has already added this transfer to its totals and worked out what the one after it needs
        if (g_expected_valid) {
            if ((uint32_t)transfer->num_bytes != g_expected_next) {
                fail("the driver took a transfer of %d bytes after saying the next one needs %" PRIu32, transfer->num_bytes,
                     g_expected_next);
            }
            g_next_checks++;
        }
        g_expected_next = read_progress().next_transfer_bytes;
        g_expected_valid = true;
    }
#endif
    assert(g_in_flight_num < MAX_IN_FLIGHT);
    g_in_flight[g_in_flight_num++] = transfer;
    if (g_packets < SIZES_PACKETS) {
        g_packet_transfers++;
    }
    for (int i = 0; i < transfer->num_isoc_packets && g_packets < SIZES_PACKETS; i++) {
        const uint32_t size = transfer->isoc_packet_desc[i].num_bytes;
        assert(size <= MAX_PACKET_BYTES);
        g_packets++;
        g_packet_bytes += size;
        g_size_count[size]++;
        // FNV-1a over the two bytes of each size, so the order of the sizes counts as well
        g_packet_hash = (g_packet_hash ^ (size & 0xFF)) * 16777619u;
        g_packet_hash = (g_packet_hash ^ (size >> 8)) * 16777619u;
    }
}

static void on_event(uac_host_device_handle_t handle, const uac_host_device_event_t event, void *arg)
{
#ifdef PATCHED
    if (g_progress_mode && event == UAC_HOST_DEVICE_EVENT_TX_DONE) {
        check(true);
    }
#endif
}

// One write of silence, as ESPHome's speaker makes them
static void stream_write(uint32_t bytes)
{
#ifdef PATCHED
    // Counted first, as the speaker does: the driver takes transfers inside the write
    g_count += bytes;
#endif
    const esp_err_t err = uac_host_device_write(g_iface, g_zeros, bytes, 0);
    if (err != ESP_OK) {
        printf("FAIL: a write of %" PRIu32 " bytes returned 0x%X\n", bytes, err);
        exit(1);
    }
}

// Completes the oldest data transfer in flight, which makes the driver try to take its next one
static bool complete_one(void)
{
    if (g_in_flight_num == 0) {
        return false;
    }
    usb_transfer_t *xfer = g_in_flight[0];
    g_in_flight_num--;
    memmove(g_in_flight, g_in_flight + 1, g_in_flight_num * sizeof(g_in_flight[0]));
    xfer->status = USB_TRANSFER_STATUS_COMPLETED;
    xfer->callback(xfer);
    return true;
}

// Starts the first playback interface that accepts the format, set up the way ESPHome's hub opens one
static uac_iface_t *start_speaker(uac_device_t *uac_device, uint32_t rate, uint8_t bits, uint8_t channels)
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
        if (uac_host_interface_add(uac_device, intf->bInterfaceNumber, &iface) != ESP_OK || iface->dev_info.type != UAC_STREAM_TX) {
            continue;
        }
        iface->state = UAC_INTERFACE_STATE_IDLE;
        iface->ringbuf = (RingbufHandle_t)1;
        iface->ringbuf_size = BUFFER_BYTES;
        // The hub sets the threshold to the speaker's buffer size, so the driver raises TX_DONE after every transfer it takes
        iface->ringbuf_threshold = BUFFER_BYTES;
        iface->user_cb = on_event;
#ifdef PATCHED
        if (g_wrap) {
            iface->tx_progress.bytes_taken = 0xFFFF0000;
            iface->tx_progress.transfers_taken = 0xFFFFFFF0;
        }
#endif
        const uac_host_stream_config_t stream_config = {.channels = channels, .bit_resolution = bits, .sample_freq = rate};
        err = uac_host_device_start(iface, &stream_config);
        if (err == ESP_OK) {
            const uac_iface_alt_t *alt = &iface->iface_alt[iface->cur_alt];
            printf("  interface %d alt %d: %d ch, %d-bit, endpoint 0x%02X mps %d, feature unit %d, ep freq ctrl %d\n",
                   iface->dev_info.iface_num, alt->alt_idx, alt->dev_alt_param.channels, alt->dev_alt_param.bit_resolution,
                   alt->ep_addr, alt->ep_mps, alt->feature_unit, alt->freq_ctrl_supported);
            printf("  start result: OK (0x0)\n");
            return iface;
        }
    }
    printf("  start result: FAILED (0x%X)\n", err);
    exit(1);
}

static void run_sizes(void)
{
    while (g_packets < SIZES_PACKETS) {
        while (g_ring_fill + 1600 <= BUFFER_BYTES) {
            stream_write(1600);
        }
        complete_one();
    }
    printf("  sizes: %" PRIu32 " packets in %" PRIu32 " transfers, %" PRIu32 " bytes, hash 0x%08" PRIX32 "\n", g_packets,
           g_packet_transfers, g_packet_bytes, g_packet_hash);
    for (int size = 0; size <= MAX_PACKET_BYTES; size++) {
        if (g_size_count[size]) {
            printf("    %d bytes: %" PRIu32 "\n", size, g_size_count[size]);
        }
    }
}

#ifdef PATCHED
// Completes the feedback transfer with a rate that is `factor` times the nominal one
static void send_feedback(double factor)
{
    usb_transfer_t *fb_xfer = g_fb_xfer;
    const bool high_speed = g_speed == USB_SPEED_HIGH;
    // 16.16 frames per microframe in 4 bytes on High-Speed, 10.14 frames per frame in 3 bytes on Full-Speed
    const double rate = factor * g_rate;
    const uint32_t value = high_speed ? (uint32_t)(rate / 8000 * 65536) : (uint32_t)(rate / 1000 * 16384);
    put_le32(fb_xfer->data_buffer, value);
    fb_xfer->status = USB_TRANSFER_STATUS_COMPLETED;
    fb_xfer->isoc_packet_desc[0].status = USB_TRANSFER_STATUS_COMPLETED;
    fb_xfer->isoc_packet_desc[0].actual_num_bytes = high_speed ? 4 : 3;
    g_expected_valid = false;
    fb_xfer->callback(fb_xfer);
}

// One transfer time: the device's feedback may move, the writer fills the buffer unless it starves, one transfer completes
static void tick(void)
{
    static int step, direction = 1;
    if (g_iface->fb_xfer && g_tick % 97 == 0) {
        // A triangle wave between 0.9998 and 1.0002 of the nominal rate, every value of which the driver must take
        const uint32_t reads = g_iface->stat_fb_reads;
        send_feedback(1.0 + step * 0.00001);
        if (g_iface->stat_fb_reads != reads + 1) {
            fail("the driver left out a feedback value of %.5f times the nominal rate", 1.0 + step * 0.00001);
        }
        step += direction;
        if (step == 20 || step == -20) {
            direction = -direction;
        }
    }
    if (g_iface->fb_xfer && g_tick == 1000) {
        // A value far from the nominal rate must be left out
        const uint32_t before = g_iface->fb_frames_q16;
        send_feedback(2.0);
        if (g_iface->fb_frames_q16 != before) {
            fail("the driver took a feedback value of twice the nominal rate");
        }
    }
    if (g_tick % 5000 >= 40) {
        const uint32_t chunk = 100 * g_iface->packet_frame_size;
        while (g_count + chunk <= BUFFER_BYTES) {
            stream_write(chunk);
            check(false);
        }
    }
    complete_one();
    check(false);
    g_tick++;
}

// Every capture interface must refuse to report playback totals
static uint32_t count_refused_capture(uac_device_t *uac_device)
{
    uint32_t refused = 0;
    int offset = 0;
    const usb_intf_desc_t *intf = GET_NEXT_INTERFACE_DESC(g_config, g_config->wTotalLength, offset);
    for (; intf != NULL; intf = GET_NEXT_INTERFACE_DESC(intf, g_config->wTotalLength, offset)) {
        uac_iface_t *iface = NULL;
        if (intf->bInterfaceClass != USB_CLASS_AUDIO || intf->bInterfaceSubClass != UAC_SUBCLASS_AUDIOSTREAMING ||
                intf->bAlternateSetting != 0 || uac_host_interface_add(uac_device, intf->bInterfaceNumber, &iface) != ESP_OK ||
                iface->dev_info.type != UAC_STREAM_RX) {
            continue;
        }
        uac_host_tx_progress_t progress;
        const esp_err_t err = uac_host_device_get_tx_progress(iface, &progress);
        if (err != ESP_ERR_NOT_SUPPORTED) {
            fail("capture interface %d returned 0x%X for its playback totals", intf->bInterfaceNumber, err);
        }
        refused++;
    }
    return refused;
}

static void run_progress(uac_device_t *uac_device)
{
    uac_host_tx_progress_t progress;
    if (uac_host_device_get_tx_progress(g_iface, NULL) != ESP_ERR_INVALID_ARG ||
            uac_host_device_get_tx_progress(NULL, &progress) != ESP_ERR_INVALID_ARG) {
        fail("a NULL argument was not refused");
    }
    const uint32_t refused = count_refused_capture(uac_device);

    const uac_host_tx_progress_t start = read_progress();
    if (g_wrap && (start.bytes_taken != 0xFFFF0000 || start.transfers_taken != 0xFFFFFFF0)) {
        fail("starting the stream changed the totals");
    }
    if (start.next_transfer_bytes == 0) {
        fail("no size for the first transfer after the stream started");
    }
    g_seen = start;
    g_expected_next = start.next_transfer_bytes;
    g_expected_valid = true;
    while (g_taken < 100000) {
        tick();
    }

    // A suspend and resume, as between two tracks: the totals carry on, and the driver says what its first transfer needs
    const uac_host_tx_progress_t before = read_progress();
    if (uac_host_device_suspend(g_iface) != ESP_OK) {
        fail("suspend failed");
    }
    g_in_flight_num = 0;
    if (uac_host_device_resume(g_iface) != ESP_OK) {
        fail("resume failed");
    }
    const uac_host_tx_progress_t after = read_progress();
    if (after.bytes_taken != before.bytes_taken || after.transfers_taken != before.transfers_taken) {
        fail("a suspend and resume changed the totals");
    }
    if (g_ring_fill != 0 || after.next_transfer_bytes == 0) {
        fail("after a resume the buffer holds %zu bytes and the next transfer needs %" PRIu32, g_ring_fill,
             after.next_transfer_bytes);
    }
    g_count = 0;
    g_seen = after;
    g_submits_seen = g_submits;
    g_expected_next = after.next_transfer_bytes;
    g_expected_valid = true;
    for (int i = 0; i < 1000; i++) {
        tick();
    }

    const bool wrapped = read_progress().bytes_taken < start.bytes_taken;
    if (g_next_checks < 90000 || g_waits < 100 || (g_wrap && !wrapped)) {
        fail("too little was checked: %" PRIu32 " next-size checks, %" PRIu32 " waits, wrapped %d", g_next_checks, g_waits,
             wrapped);
    }
    printf("  progress: %" PRIu32 " transfers, %" PRIu32 " next-size checks, %" PRIu32 " waits, feedback %s, wrapped %s, "
           "capture interfaces refused %" PRIu32 "\n", g_taken, g_next_checks, g_waits, g_iface->fb_xfer ? "moving" : "none",
           wrapped ? "yes" : "no", refused);
    printf("PASS\n");
}

static void expect_requests(const char *when, const char *expected)
{
    if (strcmp(g_requests, expected) != 0) {
        fail("%s the requests were %s, expected %s", when, g_requests, expected);
    }
    printf("  %s: %s\n", when, expected);
    g_requests[0] = 0;
}

static void restart_stream(void)
{
    if (uac_host_device_suspend(g_iface) != ESP_OK || uac_host_device_resume(g_iface) != ESP_OK) {
        fail("the stream did not restart");
    }
}

// How a UAC 2.0 stream starts, in the order other hosts use: deselect the interface (alternate setting 0), bring the
// clock to the rate, select the streaming alternate setting.
// - A Schiit Modi+ plays nothing on a stream started without the deselect: every first stream after it connected was
//   silent, and every stream after a suspend, which sends one, played (hardware, 2026-11-13).
// - The clock's rate is read first and set only when it differs, and after a change the driver waits until the device
//   calls the clock valid.
static void run_clock(bool has_validity)
{
    // main() started the stream at a rate the device was not running at, with a clock that needs 3 reads to settle
    expect_requests("first start", has_validity ? "I0GSvvvVI1" : "I0GSI1");

    // The first I0 is the suspend
    restart_stream();
    expect_requests("restart at the same rate", "I0I0GI1");

    // The device is back at another rate and its clock never settles: the stream still starts, after a bounded wait
    g_dev_clock_hz[g_iface->iface_alt[g_iface->cur_alt].clock_id] = DEV_CLOCK_POWER_ON_HZ;
    g_dev_clock_settle_reads = 1000000;
    restart_stream();
    char expected[160] = "I0I0GS";
    if (has_validity) {
        memset(expected + 6, 'v', 100);
    }
    strcat(expected, "I1");
    expect_requests("a clock that never settles", expected);
    printf("PASS\n");
}

// Completes the oldest data transfer in flight with its packets `first` to `last`, and `also`, marked as skipped by
// the host controller. Sets every packet's status, because the driver hands the same transfers in again.
static void complete_one_skipped(int first, int last, int also)
{
    if (g_in_flight_num == 0) {
        fail("no transfer in flight to complete");
    }
    usb_transfer_t *xfer = g_in_flight[0];
    for (int i = 0; i < xfer->num_isoc_packets; i++) {
        const bool skipped = (i >= first && i <= last) || i == also;
        xfer->isoc_packet_desc[i].status = skipped ? USB_TRANSFER_STATUS_SKIPPED : USB_TRANSFER_STATUS_COMPLETED;
    }
    complete_one();
}

// One report's worth of transfers, four of them with skipped packets: at the start, in the middle, at the start and
// further on, and at the end. The driver must name each run of skipped packets; run.sh checks its lines.
static void run_skips(void)
{
    for (g_tick = 1; g_tick <= 1000 / CONFIG_UAC_NUM_PACKETS_PER_URB; g_tick++) {
        while (g_ring_fill + 1600 <= BUFFER_BYTES) {
            stream_write(1600);
        }
        switch (g_tick) {
        case 5:
            complete_one_skipped(0, 1, -1);
            break;
        case 9:
            complete_one_skipped(10, 12, -1);
            break;
        case 12:
            complete_one_skipped(0, 0, 20);
            break;
        case 15:
            complete_one_skipped(23, 23, -1);
            break;
        default:
            complete_one_skipped(-1, -2, -1);
            break;
        }
    }
    printf("PASS\n");
}
#endif

int main(int argc, char **argv)
{
    if (argc < 7) {
        printf("usage: test_stream DESCRIPTOR.bin|@uac1 full|high RATE BITS CHANNELS sizes|progress [wrap]\n");
        return 2;
    }
    static uint8_t raw[8192];
    size_t len = sizeof(UAC1_SPEAKER_DESC);
    if (strcmp(argv[1], "@uac1") == 0) {
        memcpy(raw, UAC1_SPEAKER_DESC, len);
    } else {
        FILE *file = fopen(argv[1], "rb");
        if (file == NULL) {
            printf("cannot read %s\n", argv[1]);
            return 2;
        }
        len = fread(raw, 1, sizeof(raw), file);
        fclose(file);
    }
#ifdef PATCHED
    if (argc > 7 && strcmp(argv[7], "novalid") == 0) {
        // Take the validity control (bmControls bits 3:2) away from every clock source
        for (size_t i = 0; i + 5 < len && raw[i] != 0; i += raw[i]) {
            if (raw[i + 1] == 0x24 && raw[i + 2] == UAC2_AC_CLOCK_SOURCE) {
                raw[i + 5] &= ~0x0C;
            }
        }
    }
#endif
    g_config = (const usb_config_desc_t *)raw;
    g_speed = strcmp(argv[2], "high") == 0 ? USB_SPEED_HIGH : USB_SPEED_FULL;
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
    g_ring_capacity = BUFFER_BYTES + 4;  // the hub gives the driver's buffer 4 bytes more than the speaker fills
#ifdef PATCHED
    g_progress_mode = strcmp(argv[6], "progress") == 0;
    g_wrap = argc > 7 && strcmp(argv[7], "wrap") == 0;
    g_rate = atoi(argv[3]);
    const bool clock_mode = strcmp(argv[6], "clock") == 0;
    const bool has_validity = !(argc > 7 && strcmp(argv[7], "novalid") == 0);
    if (clock_mode) {
        g_dev_clock_settle_reads = 3;
        g_requests[0] = 0;
    }
#endif
    g_iface = start_speaker(uac_device, atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));

    if (strcmp(argv[6], "sizes") == 0) {
        run_sizes();
        return 0;
    }
#ifdef PATCHED
    if (g_progress_mode) {
        run_progress(uac_device);
        return 0;
    }
    if (clock_mode) {
        run_clock(has_validity);
        return 0;
    }
    if (strcmp(argv[6], "skips") == 0) {
        run_skips();
        return 0;
    }
#endif
    printf("unknown mode %s\n", argv[6]);
    return 2;
}
