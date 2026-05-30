"""ZHA quirk for the flexitMC bridge — make the custom endpoint-label attribute
readable in the ZHA UI.

The firmware adds a read-only character-string attribute **0xF000** to the
Temperature Measurement cluster (0x0402) on endpoints 2/3/4, returning
"supply" / "extract" / "outdoor". zigpy has no definition for that attribute, so
ZHA's "Manage Zigbee device" reader doesn't list it in the attribute dropdown
(and that dropdown has no free-text entry). This quirk teaches zigpy the
attribute by replacing the Temperature Measurement cluster on those endpoints
with a subclass that declares it.

It is an **attribute-only** quirk: it calls no `.sensor()` / `.number()` / etc.,
so it creates **no entity** and therefore does NOT duplicate the temperature
sensors (that was why the earlier naming quirk was dropped). It only makes
0xF000 selectable/readable in the UI, exposed under the name `endpoint_label`.

Install:
  1. Set a custom-quirks path in configuration.yaml (then restart HA):
       zha:
         custom_quirks_path: /config/custom_zha_quirks
  2. Drop this file in that folder and restart HA.
  3. Read it: device -> Manage Zigbee device -> endpoint 2/3/4 ->
     Temperature Measurement -> attribute `endpoint_label` -> Read.
     (Or with zha-toolkit: attribute 61440 / 0xF000, no manufacturer code.)

API note: written against zigpy v2 (HA 2026.x). If imports or kwargs differ on
your zigpy version, adjust `ZCLAttributeDef` / `replaces()` accordingly.
"""

import zigpy.types as t
from zigpy.quirks import CustomCluster
from zigpy.quirks.v2 import QuirkBuilder
from zigpy.zcl.clusters.measurement import TemperatureMeasurement
from zigpy.zcl.foundation import ZCLAttributeDef

FLEXIT_LABEL_ATTR_ID = 0xF000


class FlexitTemperatureMeasurement(CustomCluster, TemperatureMeasurement):
    """Temperature Measurement + the firmware's read-only endpoint label."""

    class AttributeDefs(TemperatureMeasurement.AttributeDefs):
        endpoint_label = ZCLAttributeDef(
            id=FLEXIT_LABEL_ATTR_ID,
            type=t.CharacterString,
            access="r",
            is_manufacturer_specific=False,
        )


(
    QuirkBuilder("SolidSystem", "flexitMC")
    .replaces(FlexitTemperatureMeasurement, endpoint_id=2)
    .replaces(FlexitTemperatureMeasurement, endpoint_id=3)
    .replaces(FlexitTemperatureMeasurement, endpoint_id=4)
    .add_to_registry()
)
