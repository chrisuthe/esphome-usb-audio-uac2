"""Swaps ESPHome's usb_audio onto the UAC 2.0-capable copy of espressif/usb_host_uac kept in this directory."""

from pathlib import Path

from esphome.components import esp32
from esphome.components.esp32 import VARIANT_ESP32P4, VARIANT_ESP32S31
import esphome.config_validation as cv
from esphome.const import CONF_FRAMEWORK
from esphome.core import CORE
from esphome.coroutine import CoroPriority, coroutine_with_priority
from esphome.types import ConfigType

DEPENDENCIES = ["usb_audio"]

CONF_SPEAKER_INTERFACE = "speaker_interface"

CONFIG_SCHEMA = cv.Schema(
    {
        # usb_audio plays on the first playback interface a device announces; this names another one
        cv.Optional(CONF_SPEAKER_INTERFACE): cv.int_range(min=0, max=255),
    }
)

# Largest configuration descriptor a device may have. usb_host sets 1024, and UAC 2.0 devices often need more.
CONTROL_TRANSFER_MAX_SIZE = 4096
# UAC 2.0 devices list more sample rates than the 4 the driver keeps by default
SAMPLE_RATES_MAX = 8
# The chips whose USB host port is High-Speed
HIGH_SPEED_VARIANTS = (VARIANT_ESP32P4, VARIANT_ESP32S31)
FIFO_BIAS_PREFIX = "CONFIG_USB_HOST_HW_BUFFER_BIAS_"


def _user_set_fifo_bias() -> bool:
    options = CORE.config["esp32"].get(CONF_FRAMEWORK, {}).get("sdkconfig_options", {})
    return any(option.startswith(FIFO_BIAS_PREFIX) for option in options)


# After usb_audio and usb_host, whose own driver version and buffer size these replace
@coroutine_with_priority(CoroPriority.LATE)
async def to_code(config: ConfigType) -> None:
    esp32.add_idf_component(
        name="espressif/usb_host_uac", path=str(Path(__file__).parent / "usb_host_uac")
    )
    esp32.add_idf_sdkconfig_option(
        "CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE", CONTROL_TRANSFER_MAX_SIZE
    )
    esp32.add_idf_sdkconfig_option("CONFIG_UAC_FREQ_NUM_MAX", SAMPLE_RATES_MAX)
    if (speaker_interface := config.get(CONF_SPEAKER_INTERFACE)) is not None:
        esp32.add_idf_sdkconfig_option("CONFIG_UAC_TX_INTERFACE", speaker_interface)
    # A UAC 2.0 playback endpoint is sized for the device's fastest sample rate and can exceed the 512 bytes that a
    # High-Speed controller's default FIFO split gives periodic OUT transfers. usb_host sizes the FIFO only when
    # the configured format does not fit that split, and 48 kHz stereo does. On a Full-Speed chip the default is
    # too small for the format itself, so usb_host already hands the spare FIFO to the audio streams.
    if esp32.get_esp32_variant() in HIGH_SPEED_VARIANTS and not _user_set_fifo_bias():
        esp32.add_idf_sdkconfig_option("CONFIG_USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT", True)
