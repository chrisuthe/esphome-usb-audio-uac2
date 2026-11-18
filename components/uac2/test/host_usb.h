// The USB host library, replaced by stubs that print what the driver asks of the device and answer for it.
// Included after uac_host.c by each test, which defines test_on_submit().
#pragma once

static const usb_config_desc_t *g_config;
static usb_speed_t g_speed;
static usb_device_desc_t g_dev_desc;
static const uint32_t CLOCK_RATES[] = {44100, 48000, 88200, 96000, 176400, 192000};
#define NUM_CLOCK_RATES (sizeof(CLOCK_RATES) / sizeof(CLOCK_RATES[0]))

static void put_le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

// Called for every isochronous transfer the driver submits
static void test_on_submit(usb_transfer_t *transfer);

// ---------------------------------------------- USB host library stubs ----------------------------------------------

esp_err_t usb_host_device_info(usb_device_handle_t dev_hdl, usb_device_info_t *dev_info)
{
    memset(dev_info, 0, sizeof(*dev_info));
    dev_info->speed = g_speed;
    return ESP_OK;
}

esp_err_t usb_host_get_active_config_descriptor(usb_device_handle_t dev_hdl, const usb_config_desc_t **config_desc)
{
    *config_desc = g_config;
    return ESP_OK;
}

esp_err_t usb_host_get_device_descriptor(usb_device_handle_t dev_hdl, const usb_device_desc_t **device_desc)
{
    *device_desc = &g_dev_desc;
    return ESP_OK;
}

esp_err_t usb_host_transfer_alloc(size_t data_buffer_size, int num_isoc_packets, usb_transfer_t **transfer)
{
    usb_transfer_t *xfer = calloc(1, sizeof(usb_transfer_t) + num_isoc_packets * sizeof(usb_isoc_packet_desc_t));
    *(uint8_t **)&xfer->data_buffer = calloc(1, data_buffer_size);
    *(size_t *)&xfer->data_buffer_size = data_buffer_size;
    *(int *)&xfer->num_isoc_packets = num_isoc_packets;
    *transfer = xfer;
    return ESP_OK;
}

esp_err_t usb_host_transfer_free(usb_transfer_t *transfer) { return ESP_OK; }

// The clock sources of the stubbed device: the rate each runs at (0 stands for the rate it powers up with), and how
// many reads of the validity control answer "not valid" after a rate change
#define DEV_CLOCK_POWER_ON_HZ 44100
static uint32_t g_dev_clock_hz[256];
static int g_dev_clock_settle_reads;
static int g_dev_clock_unsettled;
// One mark per clock or interface request, for tests of their order: G read rate, S set rate, v validity read that
// says "not valid", V one that says "valid", I0/I1/... SET_INTERFACE with that alternate setting
static char g_requests[512];

static void note_request(const char *mark)
{
    strncat(g_requests, mark, sizeof(g_requests) - strlen(g_requests) - 1);
}

// Answers the class requests the way a UAC 2.0 device with 6 sample rates and a -60..0 dB volume would
esp_err_t usb_host_transfer_submit_control(usb_host_client_handle_t client_hdl, usb_transfer_t *transfer)
{
    const usb_setup_packet_t *setup = (const usb_setup_packet_t *)transfer->data_buffer;
    uint8_t *data = transfer->data_buffer + USB_SETUP_PACKET_SIZE;
    printf("    control: bmRequestType 0x%02X bRequest 0x%02X wValue 0x%04X wIndex 0x%04X wLength %d", setup->bmRequestType,
           setup->bRequest, setup->wValue, setup->wIndex, setup->wLength);
    int reply = 0;
    if (setup->bmRequestType & 0x80) {
        if (setup->bmRequestType == 0xA1 && setup->bRequest == 0x02 && (setup->wValue >> 8) == 0x01) {
            // Sample rate RANGE of a clock source
            uint8_t answer[2 + 12 * NUM_CLOCK_RATES] = {NUM_CLOCK_RATES, 0};
            for (size_t i = 0; i < NUM_CLOCK_RATES; i++) {
                put_le32(answer + 2 + 12 * i, CLOCK_RATES[i]);
                put_le32(answer + 6 + 12 * i, CLOCK_RATES[i]);
            }
            reply = MIN((int)sizeof(answer), setup->wLength);
            memcpy(data, answer, reply);
        } else if (setup->bRequest == 0x02 && setup->bmRequestType == 0xA1) {
            // Volume RANGE of a feature unit: -60 dB to 0 dB in 1 dB steps
            const uint8_t answer[8] = {1, 0, 0x00, 0xC4, 0x00, 0x00, 0x00, 0x01};
            reply = MIN((int)sizeof(answer), setup->wLength);
            memcpy(data, answer, reply);
        } else if (setup->bmRequestType == 0xA1 && setup->bRequest == 0x01 && (setup->wValue >> 8) == 0x01 && setup->wLength == 4) {
            // Current rate of a clock source
            const uint32_t hz = g_dev_clock_hz[setup->wIndex >> 8];
            put_le32(data, hz ? hz : DEV_CLOCK_POWER_ON_HZ);
            reply = 4;
            note_request("G");
        } else if (setup->bmRequestType == 0xA1 && setup->bRequest == 0x01 && (setup->wValue >> 8) == 0x02 && setup->wLength == 1) {
            // Validity of a clock source
            data[0] = g_dev_clock_unsettled > 0 ? 0 : 1;
            if (g_dev_clock_unsettled > 0) {
                g_dev_clock_unsettled--;
            }
            reply = 1;
            note_request(data[0] ? "V" : "v");
        } else {
            reply = setup->wLength;
            memset(data, 0, reply);
        }
        printf(" -> %d bytes\n", reply);
        transfer->actual_num_bytes = USB_SETUP_PACKET_SIZE + reply;
    } else {
        printf(" data");
        for (int i = 0; i < setup->wLength; i++) {
            printf(" %02X", data[i]);
        }
        printf("\n");
        if (setup->bmRequestType == 0x21 && setup->bRequest == 0x01 && (setup->wValue >> 8) == 0x01 && setup->wLength == 4) {
            // A new rate for a clock source, which then takes a moment to become valid again
            g_dev_clock_hz[setup->wIndex >> 8] = data[0] | data[1] << 8 | data[2] << 16 | (uint32_t)data[3] << 24;
            g_dev_clock_unsettled = g_dev_clock_settle_reads;
            note_request("S");
        } else if (setup->bmRequestType == 0x01 && setup->bRequest == 0x0B) {
            const char mark[3] = {'I', (char)('0' + setup->wValue % 10), 0};
            note_request(mark);
        }
        transfer->actual_num_bytes = transfer->num_bytes;
    }
    transfer->status = USB_TRANSFER_STATUS_COMPLETED;
    transfer->callback(transfer);
    return ESP_OK;
}

esp_err_t usb_host_transfer_submit(usb_transfer_t *transfer)
{
    test_on_submit(transfer);
    return ESP_OK;
}

esp_err_t usb_host_interface_claim(usb_host_client_handle_t client_hdl, usb_device_handle_t dev_hdl, uint8_t bInterfaceNumber,
                                   uint8_t bAlternateSetting)
{
    printf("    claim: interface %d alt %d\n", bInterfaceNumber, bAlternateSetting);
    return ESP_OK;
}

esp_err_t usb_host_interface_release(usb_host_client_handle_t client_hdl, usb_device_handle_t dev_hdl, uint8_t bInterfaceNumber) { return ESP_OK; }
esp_err_t usb_host_client_register(const usb_host_client_config_t *client_config, usb_host_client_handle_t *client_hdl_ret) { return ESP_OK; }
esp_err_t usb_host_client_deregister(usb_host_client_handle_t client_hdl) { return ESP_OK; }
esp_err_t usb_host_client_handle_events(usb_host_client_handle_t client_hdl, TickType_t timeout_ticks) { return ESP_OK; }
esp_err_t usb_host_client_unblock(usb_host_client_handle_t client_hdl) { return ESP_OK; }
esp_err_t usb_host_device_open(usb_host_client_handle_t client_hdl, uint8_t dev_addr, usb_device_handle_t *dev_hdl_ret) { return ESP_OK; }
esp_err_t usb_host_device_close(usb_host_client_handle_t client_hdl, usb_device_handle_t dev_hdl) { return ESP_OK; }
esp_err_t usb_host_endpoint_halt(usb_device_handle_t dev_hdl, uint8_t bEndpointAddress) { return ESP_OK; }
esp_err_t usb_host_endpoint_flush(usb_device_handle_t dev_hdl, uint8_t bEndpointAddress) { return ESP_OK; }
esp_err_t usb_host_endpoint_clear(usb_device_handle_t dev_hdl, uint8_t bEndpointAddress) { return ESP_OK; }
esp_err_t usb_host_device_addr_list_fill(int list_len, uint8_t *dev_addr_list, int *num_dev_ret) { *num_dev_ret = 0; return ESP_OK; }
