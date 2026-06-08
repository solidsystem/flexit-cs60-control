"""ZHA (Home Assistant) quirk for the flexit-cs60-control bridge.

Scope: this quirk only adds the **writable temperature setpoint**. The firmware
models it as an Analog Value (0x000E) cluster; ZHA has no auto-discovery for
Analog Value, so it is mapped here to a settable Number entity:

    EP7  AnalogValue.present_value <-> "Supply air temperature setpoint" (°C, read/write)

Writing the Number sends an attribute write that the firmware turns into a CS60
setpoint command (coil 12 / reg 0x000C).

The two Analog Input percentage endpoints are intentionally NOT defined here:

    EP5  AnalogInput.present_value -> heat-exchanger modulation (%)
    EP6  AnalogInput.present_value -> heating output (%)

Current ZHA (zha >= 1.1) ships an unrestricted `AnalogInputSensor` that
auto-discovers any Analog Input cluster as a sensor, using the cluster's
Description (0x001C) attribute as the name and EngineeringUnits/ApplicationType
for the unit. The firmware already sets those (description "Heat exchanger" /
"Heating element", units = percent in src/zigbee_ep.c), so ZHA creates working "%"
sensors natively. Defining them here too produced a *duplicate* sensor per
endpoint, so the quirk leaves EP5/EP6 to native discovery. (Analog Value has no
such native handler, hence the Number above still needs the quirk.)

The other entities — Fan Control on EP1 and supply/outdoor Temperature on
EP2/EP4 — are discovered by ZHA normally and are unaffected by this quirk
(a v2 quirk augments standard discovery rather than replacing it).

zigpy's v2 entity validator requires each entity to carry a translation_key or a
device_class (fallback_name alone is rejected with "EntityMetadata must have a
translation_key or device_class"), so the Number below sets translation_key.
There is no translation registered for that key, so HA uses fallback_name for the
displayed name.

Install (Home Assistant):
  1. Copy this file into your ZHA custom-quirks directory, e.g.
         /config/custom_zha_quirks/flexit-cs60-control.py
     (create the directory if it does not exist).
  2. Point ZHA at that directory once, in configuration.yaml:
         zha:
           custom_quirks_path: /config/custom_zha_quirks/
  3. Restart Home Assistant. The setpoint Number appears on the flexit-cs60-control device
     (the EP5/EP6 "%" sensors come from native discovery, with or without this
     quirk). If it doesn't show after the restart, remove and re-add
     (re-interview) the device so ZHA rebuilds its entities against the
     now-applied quirk.
"""

from zigpy.quirks.v2 import QuirkBuilder, ReportingConfig
from zigpy.quirks.v2.homeassistant import UnitOfTemperature
from zigpy.zcl.clusters.general import AnalogValue

# Basic cluster ManufacturerName / ModelIdentifier reported by the firmware
# (src/zigbee_ep.c: basic_mf_name / basic_model_id).
MANUFACTURER = "SolidSystem"
MODEL = "flexit-cs60-control"

# Analog Value endpoint declared in src/zigbee_ep.c.
EP_SETPOINT = 7

# present_value is transported in deci-degrees (°C ×10); see _MULTIPLIER below.
# reportable_change is in those raw units, so 1 == a 0.1 °C change. Matched to the
# 0.1 °C step so panel-driven changes propagate at the resolution the firmware
# supports. 10 s floor, 5 min heartbeat.
_REPORTING = ReportingConfig(min_interval=10, max_interval=300, reportable_change=1)

# ZHA's config Number truncates writes to int(value / multiplier), so a plain °C
# float would be written as a whole degree. We transport the setpoint as an
# integer deci-degree count and let the multiplier scale it back to °C: HA's
# 21.5 °C -> int(21.5 / 0.1) = 215 on the wire; readback 215 -> 215 * 0.1 = 21.5.
# The firmware reinterprets present_value as deci-degrees to match
# (src/zigbee_ep.c: handle_setpoint_write / zigbee_ep_set_setpoint).
_MULTIPLIER = 0.1

# Setpoint bounds/step are in *display* units (°C); the multiplier handles the
# raw<->display conversion. Must match the firmware clamp (FLEXIT_SETPOINT_*_DC,
# 10.0–30.0 °C) in src/zigbee_ep.c.
_SETPOINT_MIN = 10.0
_SETPOINT_MAX = 30.0
_SETPOINT_STEP = 0.1

(
    QuirkBuilder(MANUFACTURER, MODEL)
    .number(
        attribute_name="present_value",
        cluster_id=AnalogValue.cluster_id,
        endpoint_id=EP_SETPOINT,
        min_value=_SETPOINT_MIN,
        max_value=_SETPOINT_MAX,
        step=_SETPOINT_STEP,
        multiplier=_MULTIPLIER,
        unit=UnitOfTemperature.CELSIUS,
        reporting_config=_REPORTING,
        translation_key="supply_air_temperature_setpoint",
        fallback_name="Supply air temperature setpoint",
    )
    .add_to_registry()
)
