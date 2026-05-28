"""ESPHome external component: ARIA Voice Bridge client.

Connects ESP32-S3 satellite to the KIS Voice Bridge WebSocket
for end-to-end Grok Realtime voice processing.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.const import CONF_ID, CONF_TRIGGER_ID
from esphome.components import microphone

DEPENDENCIES = ["microphone"]
AUTO_LOAD = []

aria_bridge_ns = cg.esphome_ns.namespace("aria_bridge")
ARIABridge = aria_bridge_ns.class_("ARIABridge", cg.Component)

# v21: ESPHome Trigger<> classes — YAML wires light effects to each phase entry.
ListeningStartTrigger = aria_bridge_ns.class_("ListeningStartTrigger", automation.Trigger.template())
ThinkingStartTrigger = aria_bridge_ns.class_("ThinkingStartTrigger", automation.Trigger.template())
RespondingStartTrigger = aria_bridge_ns.class_("RespondingStartTrigger", automation.Trigger.template())
ErrorTrigger = aria_bridge_ns.class_("ErrorTrigger", automation.Trigger.template())
IdleTrigger = aria_bridge_ns.class_("IdleTrigger", automation.Trigger.template())

CONF_BRIDGE_URL = "bridge_url"
CONF_MICROPHONE_ID = "microphone_id"
CONF_SPEAKER_ID = "speaker_id"
CONF_SAMPLE_RATE = "sample_rate"
CONF_ON_LISTENING_START = "on_listening_start"
CONF_ON_THINKING_START = "on_thinking_start"
CONF_ON_RESPONDING_START = "on_responding_start"
CONF_ON_ERROR = "on_error"
CONF_ON_IDLE = "on_idle"
# v23: event-log POST + pre-wake audio upload destinations + satellite_id stamp
CONF_EVENT_POST_URL = "event_post_url"
CONF_PREWAKE_POST_URL = "prewake_post_url"
CONF_PREWAKE_SECONDS = "prewake_seconds"
CONF_SATELLITE_ID = "satellite_id"


def _speaker_schema():
    from esphome.components import speaker
    return cv.use_id(speaker.Speaker)


CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(ARIABridge),
    cv.Required(CONF_BRIDGE_URL): cv.string,
    cv.Required(CONF_MICROPHONE_ID): cv.use_id(microphone.Microphone),
    cv.Optional(CONF_SPEAKER_ID): _speaker_schema(),
    cv.Optional(CONF_SAMPLE_RATE, default=16000): cv.int_,
    cv.Optional(CONF_EVENT_POST_URL, default=""): cv.string,
    cv.Optional(CONF_PREWAKE_POST_URL, default=""): cv.string,
    cv.Optional(CONF_PREWAKE_SECONDS, default=2): cv.int_range(min=0, max=10),
    cv.Optional(CONF_SATELLITE_ID, default="satellite1-kis"): cv.string,
    # v21: each trigger fires when the phase is ENTERED (not while it's active).
    cv.Optional(CONF_ON_LISTENING_START): automation.validate_automation({
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ListeningStartTrigger),
    }),
    cv.Optional(CONF_ON_THINKING_START): automation.validate_automation({
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ThinkingStartTrigger),
    }),
    cv.Optional(CONF_ON_RESPONDING_START): automation.validate_automation({
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(RespondingStartTrigger),
    }),
    cv.Optional(CONF_ON_ERROR): automation.validate_automation({
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ErrorTrigger),
    }),
    cv.Optional(CONF_ON_IDLE): automation.validate_automation({
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(IdleTrigger),
    }),
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_bridge_url(config[CONF_BRIDGE_URL]))
    cg.add(var.set_sample_rate(config[CONF_SAMPLE_RATE]))
    cg.add(var.set_event_post_url(config[CONF_EVENT_POST_URL]))
    cg.add(var.set_prewake_post_url(config[CONF_PREWAKE_POST_URL]))
    cg.add(var.set_prewake_seconds(config[CONF_PREWAKE_SECONDS]))
    cg.add(var.set_satellite_id(config[CONF_SATELLITE_ID]))

    mic = await cg.get_variable(config[CONF_MICROPHONE_ID])
    cg.add(var.set_microphone(mic))

    if CONF_SPEAKER_ID in config:
        spk = await cg.get_variable(config[CONF_SPEAKER_ID])
        cg.add(var.set_speaker(spk))

    # v21: register trigger automations — each Trigger ctor subscribes to its CallbackManager.
    for key in (CONF_ON_LISTENING_START, CONF_ON_THINKING_START, CONF_ON_RESPONDING_START,
                CONF_ON_ERROR, CONF_ON_IDLE):
        for conf in config.get(key, []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
            await automation.build_automation(trigger, [], conf)
