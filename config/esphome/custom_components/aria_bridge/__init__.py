"""ESPHome external component: ARIA Voice Bridge client.

Connects ESP32-S3 satellite to the KIS Voice Bridge WebSocket
for end-to-end Grok Realtime voice processing.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_URL
from esphome.components import microphone, speaker

DEPENDENCIES = ["microphone", "speaker", "micro_wake_word"]
AUTO_LOAD = []

aria_bridge_ns = cg.esphome_ns.namespace("aria_bridge")
ARIABridge = aria_bridge_ns.class_("ARIABridge", cg.Component)

CONF_BRIDGE_URL = "bridge_url"
CONF_MICROPHONE_ID = "microphone_id"
CONF_SPEAKER_ID = "speaker_id"
CONF_SAMPLE_RATE = "sample_rate"

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(ARIABridge),
    cv.Required(CONF_BRIDGE_URL): cv.string,
    cv.Required(CONF_MICROPHONE_ID): cv.use_id(microphone.Microphone),
    cv.Required(CONF_SPEAKER_ID): cv.use_id(speaker.Speaker),
    cv.Optional(CONF_SAMPLE_RATE, default=16000): cv.int_,
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_bridge_url(config[CONF_BRIDGE_URL]))
    cg.add(var.set_sample_rate(config[CONF_SAMPLE_RATE]))

    mic = await cg.get_variable(config[CONF_MICROPHONE_ID])
    cg.add(var.set_microphone(mic))

    spk = await cg.get_variable(config[CONF_SPEAKER_ID])
    cg.add(var.set_speaker(spk))
