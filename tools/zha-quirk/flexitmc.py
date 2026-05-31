"""ZHA (Home Assistant) quirk for the flexitMC bridge.

ZHA does not auto-create sensor entities from the generic Analog Input cluster
(0x000C): its built-in Analog Input handling is gated to specific manufacturers
(e.g. LUMI), so a custom device's Analog Input endpoints are discovered but show
no entity. This quirk (zigpy "v2" QuirkBuilder) matches the flexitMC end device
by its Basic ManufacturerName / ModelIdentifier and exposes the two Analog Input
endpoints as percentage sensors:

    EP5  AnalogInput.present_value -> "Heat exchanger" (rotary HX modulation, %)
    EP6  AnalogInput.present_value -> "Heating"        (heater output, %)

It also exposes the writable temperature setpoint, which the firmware models as
an Analog Value (0x000E) cluster — ZHA does not auto-expose that either, so it
is mapped here to a settable Number entity:

    EP7  AnalogValue.present_value <-> "Setpoint"      (°C, read/write)

Writing the Number sends an attribute write that the firmware turns into a CS60
setpoint command (coil 12 / reg 0x000C). No device_class is set on the sensors,
so each appears as a plain numeric "%" sensor (no humidity icon/long-term-stats
mislabel); state_class=measurement keeps them graphable. Reporting is configured
to match the firmware's reportable present_value attributes.

The other entities — Fan Control on EP1 and supply/outdoor Temperature on
EP2/EP4 — are discovered by ZHA normally and are unaffected by this quirk
(a v2 quirk augments standard discovery rather than replacing it).

Install (Home Assistant):
  1. Copy this file into your ZHA custom-quirks directory, e.g.
         /config/custom_zha_quirks/flexitmc.py
     (create the directory if it does not exist).
  2. Point ZHA at that directory once, in configuration.yaml:
         zha:
           custom_quirks_path: /config/custom_zha_quirks/
  3. Restart Home Assistant. The "%" sensors and the setpoint Number appear on
     the flexitMC device. If they don't show after the restart, remove and
     re-add (re-interview) the device so ZHA rebuilds its entities against the
     now-applied quirk.

present_value is a float; suggested_display_precision=0 renders the percentages
as whole numbers ("14" rather than "14.0"). Override per entity in the HA UI if
desired.
"""

from zigpy.quirks.v2 import QuirkBuilder, ReportingConfig, SensorStateClass
from zigpy.quirks.v2.homeassistant import PERCENTAGE, UnitOfTemperature
from zigpy.zcl.clusters.general import AnalogInput, AnalogValue

# Basic cluster ManufacturerName / ModelIdentifier reported by the firmware
# (src/zigbee_ep.c: basic_mf_name / basic_model_id).
MANUFACTURER = "SolidSystem"
MODEL = "flexitMC"

# Analog endpoints declared in src/zigbee_ep.c.
EP_HEAT_EXCHANGER = 5
EP_HEATING = 6
EP_SETPOINT = 7

# Report on a >=1 % change, with a 10 s floor and a 5 min heartbeat.
_REPORTING = ReportingConfig(min_interval=10, max_interval=300, reportable_change=1)

# Setpoint bounds/step must match the firmware clamp (FLEXIT_SETPOINT_*_DC,
# 10.0–30.0 °C) in src/zigbee_ep.c.
_SETPOINT_MIN = 10.0
_SETPOINT_MAX = 30.0
_SETPOINT_STEP = 0.5

(
    QuirkBuilder(MANUFACTURER, MODEL)
    .sensor(
        attribute_name="present_value",
        cluster_id=AnalogInput.cluster_id,
        endpoint_id=EP_HEAT_EXCHANGER,
        state_class=SensorStateClass.MEASUREMENT,
        unit=PERCENTAGE,
        suggested_display_precision=0,
        reporting_config=_REPORTING,
        fallback_name="Heat exchanger",
    )
    .sensor(
        attribute_name="present_value",
        cluster_id=AnalogInput.cluster_id,
        endpoint_id=EP_HEATING,
        state_class=SensorStateClass.MEASUREMENT,
        unit=PERCENTAGE,
        suggested_display_precision=0,
        reporting_config=_REPORTING,
        fallback_name="Heating",
    )
    .number(
        attribute_name="present_value",
        cluster_id=AnalogValue.cluster_id,
        endpoint_id=EP_SETPOINT,
        min_value=_SETPOINT_MIN,
        max_value=_SETPOINT_MAX,
        step=_SETPOINT_STEP,
        unit=UnitOfTemperature.CELSIUS,
        reporting_config=_REPORTING,
        fallback_name="Setpoint",
    )
    .add_to_registry()
)
