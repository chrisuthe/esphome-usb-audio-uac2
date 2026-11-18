// Replays a raw configuration descriptor through the UAC host driver's own parsing and stream start, with the
// USB host library replaced by stubs that print what the driver asks of the device.
// Usage: test_parse DESCRIPTOR.bin full|high [RATE BITS CHANNELS]
// Built once against the original driver and once (-DPATCHED) against the UAC 2.0 copy.

#include "uac_host.c"  // unity build: the functions under test are static
#include "host_usb.h"

static void test_on_submit(usb_transfer_t *transfer)
{
    printf("    submit: endpoint 0x%02X, %d packet(s), %d bytes\n", transfer->bEndpointAddress, transfer->num_isoc_packets,
           transfer->num_bytes);
}

// -------------------------------------------------------- Test --------------------------------------------------------

static void print_interface(const uac_iface_t *iface)
{
    printf("  interface %d: %s, %d alternate setting(s)\n", iface->dev_info.iface_num,
           iface->dev_info.type == UAC_STREAM_TX ? "speaker" : "microphone", iface->dev_info.iface_alt_num);
    for (int i = 0; i < iface->dev_info.iface_alt_num; i++) {
        const uac_iface_alt_t *alt = &iface->iface_alt[i];
        const uac_host_dev_alt_param_t *param = &alt->dev_alt_param;
        printf("    alt %d: format %d, %d ch, %d-bit in %d byte(s), endpoint 0x%02X mps %d attr 0x%02X interval %d, terminal %d, "
               "feature unit %d vol 0x%02X mute 0x%02X, ep freq ctrl %d\n",
               alt->alt_idx, param->format, param->channels, param->bit_resolution, param->subframe_size, alt->ep_addr, alt->ep_mps,
               alt->ep_attr, alt->interval, alt->connected_terminal, alt->feature_unit, alt->vol_ch_map, alt->mute_ch_map,
               alt->freq_ctrl_supported);
        printf("           rates:");
        if (param->sample_freq_type == 0) {
            printf(" range %" PRIu32 "-%" PRIu32, param->sample_freq_lower, param->sample_freq_upper);
        }
        for (int j = 0; j < param->sample_freq_type && j < UAC_FREQ_NUM_MAX; j++) {
            printf(" %" PRIu32, param->sample_freq[j]);
        }
        printf("\n");
#ifdef PATCHED
        printf("           clock %d, feedback endpoint 0x%02X mps %d interval %d\n", alt->clock_id, alt->fb_ep_addr, alt->fb_ep_mps,
               alt->fb_interval);
#endif
    }
}

static void start_stream(uac_iface_t *iface, uint32_t rate, uint8_t bits, uint8_t channels)
{
    printf("  start %" PRIu32 " Hz, %d-bit, %d ch:\n", rate, bits, channels);
    iface->state = UAC_INTERFACE_STATE_IDLE;
    iface->ringbuf = (RingbufHandle_t)1;
    const uac_host_stream_config_t stream_config = {.channels = channels, .bit_resolution = bits, .sample_freq = rate};
    esp_err_t err = uac_host_device_start(iface, &stream_config);
    printf("  start result: %s (0x%X)\n", err == ESP_OK ? "OK" : "FAILED", err);
    if (err != ESP_OK) {
        return;
    }
    printf("  streaming alt %d, nominal packet %" PRIu32 " bytes\n", iface->iface_alt[iface->cur_alt].alt_idx, iface->packet_size);
    // Volume and mute, as ESPHome sets them once the stream runs
    printf("  volume range %d..%d step %d (1/256 dB)\n", iface->vol_min_db, iface->vol_max_db, iface->vol_res_db);
    if (iface->iface_alt[iface->cur_alt].vol_ch_map &&
            uac_cs_request_get_volume_range(iface, &iface->vol_min_db, &iface->vol_max_db, &iface->vol_res_db) == ESP_OK) {
        printf("  volume range %d..%d step %d (1/256 dB) after query\n", iface->vol_min_db, iface->vol_max_db, iface->vol_res_db);
        printf("  set volume 50%%: %s\n", uac_host_device_set_volume(iface, 50) == ESP_OK ? "OK" : "FAILED");
    }
    printf("  set mute off: 0x%X\n", uac_host_device_set_mute(iface, false));
#ifdef PATCHED
    if (iface->fb_xfer) {
        // The device asks for 48010 Hz: 16.16 frames per microframe on High-Speed, 10.14 frames per frame in 3 bytes on Full-Speed
        usb_transfer_t *fb_xfer = iface->fb_xfer;
        const bool high_speed = g_speed == USB_SPEED_HIGH;
        const uint32_t asked = high_speed ? (uint32_t)(48010.0 / 8000 * 65536) : (uint32_t)(48010.0 / 1000 * 16384);
        fb_xfer->data_buffer[0] = asked;
        fb_xfer->data_buffer[1] = asked >> 8;
        fb_xfer->data_buffer[2] = asked >> 16;
        fb_xfer->data_buffer[3] = asked >> 24;
        fb_xfer->status = USB_TRANSFER_STATUS_COMPLETED;
        fb_xfer->isoc_packet_desc[0].status = USB_TRANSFER_STATUS_COMPLETED;
        fb_xfer->isoc_packet_desc[0].actual_num_bytes = high_speed ? 4 : 3;
        stream_fb_xfer_done(fb_xfer);
        printf("  feedback taken: %.4f frames per packet\n", iface->fb_frames_q16 / 65536.0);
    }
    usb_transfer_t *xfer = iface->free_xfer_list[0];
    uint32_t bytes = 0;
    int sizes[8] = {0};
    const int packets = 24000;
    for (int i = 0; i < packets / iface->packet_num; i++) {
        bytes += _uac_fill_tx_xfer_packets(iface, xfer);
        for (int j = 0; j < iface->packet_num; j++) {
            int extra = (int)(xfer->isoc_packet_desc[j].num_bytes / iface->packet_frame_size) - (int)(iface->packet_size / iface->packet_frame_size);
            sizes[extra + 2 < 0 ? 0 : (extra + 2 > 7 ? 7 : extra + 2)]++;
        }
    }
    printf("  %d packets carry %" PRIu32 " frames: %d of nominal size, %d one frame longer, %d one frame shorter\n", packets,
           bytes / iface->packet_frame_size, sizes[2], sizes[3], sizes[1]);
#endif
    printf("  stop: 0x%X\n", uac_host_device_stop(iface));
}

int main(int argc, char **argv)
{
    FILE *file = fopen(argv[1], "rb");
    static uint8_t raw[8192];
    const size_t len = fread(raw, 1, sizeof(raw), file);
    fclose(file);
    g_config = (const usb_config_desc_t *)raw;
    g_speed = strcmp(argv[2], "high") == 0 ? USB_SPEED_HIGH : USB_SPEED_FULL;
    printf("%s: %zu bytes, %s speed\n", argv[1], len, argv[2]);

    s_uac_driver = calloc(1, sizeof(uac_driver_t));
    STAILQ_INIT(&s_uac_driver->uac_devices_tailq);
    STAILQ_INIT(&s_uac_driver->uac_ifaces_tailq);
    s_uac_driver->client_handle = (usb_host_client_handle_t)1;

    uac_device_t *uac_device = NULL;
    esp_err_t err = _uac_host_device_add(1, (usb_device_handle_t)1, g_config, &uac_device);
    if (err != ESP_OK) {
        printf("  device rejected: 0x%X\n", err);
        return 0;
    }
#ifdef PATCHED
    printf("  UAC version 0x%04X, control interface %d\n", uac_device->bcd_adc, uac_device->ctrl_iface_num);
#endif

    int offset = 0;
    const usb_intf_desc_t *intf = GET_NEXT_INTERFACE_DESC(g_config, g_config->wTotalLength, offset);
    while (intf != NULL) {
        if (intf->bInterfaceClass == USB_CLASS_AUDIO && intf->bInterfaceSubClass == UAC_SUBCLASS_AUDIOSTREAMING &&
                intf->bAlternateSetting == 0) {
            uac_iface_t *iface = NULL;
            err = uac_host_interface_add(uac_device, intf->bInterfaceNumber, &iface);
            if (err != ESP_OK) {
                printf("  interface %d rejected: 0x%X\n", intf->bInterfaceNumber, err);
            } else {
                print_interface(iface);
                if (iface->iface_alt[0].vol_ch_map) {
                    // What uac_host_device_open() and a later volume change send
                    int16_t vol_min = 0, vol_max = 0, vol_res = 0;
                    err = uac_cs_request_get_volume_range(iface, &vol_min, &vol_max, &vol_res);
                    printf("  volume range: 0x%X, %d..%d step %d (1/256 dB)\n", err, vol_min, vol_max, vol_res);
                    printf("  set volume -30 dB: 0x%X\n", uac_cs_request_set_volume(iface, -30 * 256));
                }
                if (argc > 5 && iface->dev_info.type == UAC_STREAM_TX) {
                    start_stream(iface, atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));
                }
            }
        }
        intf = GET_NEXT_INTERFACE_DESC(intf, g_config->wTotalLength, offset);
    }
    return 0;
}
