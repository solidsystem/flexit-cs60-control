"""ZHA quirk for the flexitMC3 Flexit CS60 bridge.

Cosmetic only: name the three Temperature Measurement endpoints
supply / extract / outdoor. Without it the device still works — ZHA exposes one
fan + three temperature sensors with generic names ("Temperature", "..._2/3"),
which you can also just rename in the HA UI.

UNTESTED against a live ZHA — this uses the zigpy v2 QuirkBuilder API (recent
zigpy / HA 2024.x+). Import paths and method kwargs have shifted across versions;
adjust to match yours. Drop into your custom-quirks directory (configured via the
ZHA integration's "custom quirks path") and restart HA.

Endpoints (see README.md): EP1 fan (0x0202), EP2/3/4 temperature (0x0402),
measured_value in 0.01 C (hence divisor 100).
"""

from zigpy.quirks.v2 import QuirkBuilder
from zigpy.quirks.v2.homeassistant import UnitOfTemperature
from zigpy.quirks.v2.homeassistant.sensor import SensorDeviceClass, SensorStateClass
from zigpy.zcl.clusters.measurement import TemperatureMeasurement

_TEMP_ENDPOINTS = {
    2: ("supply_air_temperature", "Supply air temperature"),
    3: ("extract_air_temperature", "Extract air temperature"),
    4: ("outdoor_air_temperature", "Outdoor air temperature"),
}

builder = QuirkBuilder("SolidSystem", "flexitMC3")

for ep_id, (translation_key, fallback_name) in _TEMP_ENDPOINTS.items():
    builder = builder.sensor(
        TemperatureMeasurement.AttributeDefs.measured_value.name,
        TemperatureMeasurement.cluster_id,
        endpoint_id=ep_id,
        divisor=100,
        unit=UnitOfTemperature.CELSIUS,
        device_class=SensorDeviceClass.TEMPERATURE,
        state_class=SensorStateClass.MEASUREMENT,
        translation_key=translation_key,
        fallback_name=fallback_name,
    )

# The fan (Fan Control on EP1) is auto-exposed by ZHA; no override needed.
builder.add_to_registry()
