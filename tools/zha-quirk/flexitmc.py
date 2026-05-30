"""ZHA (Home Assistant) quirk for the flexitMC bridge.

ZHA does not auto-create sensor entities from the generic Analog Input cluster
(0x000C): its built-in Analog Input handling is gated to specific manufacturers
(e.g. LUMI), so a custom device's Analog Input endpoints are discovered but show
no entity. This quirk (zigpy "v2" QuirkBuilder) matches the flexitMC end device
by its Basic ManufacturerName / ModelIdentifier and exposes the two Analog Input
endpoints as percentage sensors:

    EP5  present_value -> "Heat exchanger" (rotary HX modulation, %)
    EP6  present_value -> "Heating"        (heater output, %)

No device_class is set, so each appears as a plain numeric "%" sensor (no
humidity icon/long-term-stats mislabel); state_class=measurement keeps them
graphable. Reporting is configured to match the firmware's reportable
present_value attribute.

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
  3. Restart Home Assistant. The two "%" sensors appear on the flexitMC device.
     If they don't show after the restart, remove and re-add (re-interview) the
     device so ZHA rebuilds its entities against the now-applied quirk.

present_value is a float; suggested_display_precision=0 renders it as a whole
percent ("14" rather than "14.0"). Override per entity in the HA UI if desired.
"""

from zigpy.quirks.v2 import QuirkBuilder, ReportingConfig, SensorStateClass
from zigpy.quirks.v2.homeassistant import PERCENTAGE
from zigpy.zcl.clusters.general import AnalogInput

# Basic cluster ManufacturerName / ModelIdentifier reported by the firmware
# (src/zigbee_ep.c: basic_mf_name / basic_model_id).
MANUFACTURER = "SolidSystem"
MODEL = "flexitMC"

# Analog Input endpoints declared in src/zigbee_ep.c.
EP_HEAT_EXCHANGER = 5
EP_HEATING = 6

# Report on a >=1 % change, with a 10 s floor and a 5 min heartbeat.
_REPORTING = ReportingConfig(min_interval=10, max_interval=300, reportable_change=1)

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
    .add_to_registry()
)
