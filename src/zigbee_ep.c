/* Zigbee data model for the flexit-cs60-control bridge.
 *
 * A custom Zigbee end device:
 *   EP1  Basic + Identify + Fan Control (FanMode rw, reportable)
 *   EP2  Temperature Measurement  (supply air)
 *   EP4  Analog Input (Basic)     (intake air temperature °C)
 *   EP5  Analog Input (Basic)     (heat-exchanger modulation %)
 *   EP6  Analog Input (Basic)     (heating output %)
 *   EP7  Analog Value (Basic)     (temperature setpoint, read/write)
*
 * The endpoints are hand-declared (rather than via a canned HA device-type
 * macro) so EP1 can carry Fan Control and so the endpoints can share one
 * simple-descriptor type — ZB_DECLARE_SIMPLE_DESC() typedefs a struct keyed by
 * the (in,out) cluster counts, so reusing a canned single-EP macro would
 * redefine the same struct.
 *
 * EP4 carries intake air temperature on an Analog Input (not a Temperature
 * Measurement) cluster so HA names it from the cluster Description; see
 * zigbee_ep.h. Trade-off: Analog Input has no "invalid" sentinel, so unlike the
 * EP2 supply temp it holds its last reading when the source goes stale rather
 * than going "unknown".
 *
 * Values arrive through the public setters from arbitrary threads; they are
 * latched under a mutex and pushed into the ZCL attributes by publish_tick(),
 * which runs on the ZBOSS stack thread (ZCL attribute writes must happen in
 * ZBOSS context). See smarthouse-integration.md.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>

#include <zboss_api.h>
#include <zboss_api_af.h>
#include <zcl/zb_zcl_common.h>
#include <zcl/zb_zcl_basic.h>
#include <zcl/zb_zcl_identify.h>
#include <zcl/zb_zcl_temp_measurement.h>
#include <zcl/zb_zcl_fan_control.h>
#include <zcl/zb_zcl_analog_input.h>
#include <zcl/zb_zcl_analog_value.h>
#include <ha/zb_ha_device_config.h>
#include <zigbee/zigbee_error_handler.h>
#include <zigbee/zigbee_app_utils.h>
#include <zb_nrf_platform.h>

#include "zigbee_ep.h"

LOG_MODULE_REGISTER(zigbee_ep, LOG_LEVEL_INF);

/* --- Endpoint IDs --- */
#define FLEXIT_CTRL_ENDPOINT      1   /* Basic + Identify + Fan Control */
#define FLEXIT_TEMP_EP_SUPPLY     2
#define FLEXIT_AI_EP_INTAKE       4   /* Analog Input — intake air temp °C (was EP3 extract, removed) */
#define FLEXIT_AI_EP_HEAT_EXCH    5   /* Analog Input — HX modulation %  */
#define FLEXIT_AI_EP_HEATING      6   /* Analog Input — heating output % */
#define FLEXIT_AV_EP_SETPOINT     7   /* Analog Value — temp setpoint (rw) */

/* ZCL EngineeringUnits enum (BACnet-derived): 98 == "percent", 62 == "degrees C". */
#define FLEXIT_AI_UNITS_PERCENT   98
#define FLEXIT_AI_UNITS_DEGC      62
#define FLEXIT_AV_UNITS_DEGC      62

/* Analog Input intake-air temperature display range (°C). */
#define FLEXIT_AI_TEMP_MIN_C      (-40.0f)
#define FLEXIT_AI_TEMP_MAX_C      ( 80.0f)

/* Setpoint clamp (°C ×10) applied to writes from a Zigbee client. */
#define FLEXIT_SETPOINT_MIN_DC    100   /* 10.0 C */
#define FLEXIT_SETPOINT_MAX_DC    300   /* 30.0 C */

/* After a client write, ignore CS60 readback for this long so the just-written
 * value isn't transiently overwritten by the (still old) committed reading
 * before the CS60 adopts and re-broadcasts it. */
#define FLEXIT_SETPOINT_WRITE_HOLD_MS 5000

#define FLEXIT_DEVICE_VERSION     0
#define FLEXIT_INIT_BASIC_POWER_SOURCE  ZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE

/* Temperature Measurement valid range (hundredths of a degree Celsius). */
#define FLEXIT_TEMP_MIN_CC        (-4000)  /* -40.00 C */
#define FLEXIT_TEMP_MAX_CC        ( 8000)  /*  80.00 C */

/* Custom read-only label attribute added to each Temperature Measurement
 * cluster: a ZCL character string naming the endpoint ("supply"/"extract"/
 * "outdoor"). Lets a client identify which physical channel an endpoint is by
 * reading one attribute (e.g. `zcl attr read <addr> 2 0402 0104 f000`, or ZHA's
 * "Manage Zigbee device" attribute reader). 0xF000 is in the custom range; it is
 * not manufacturer-coded, so it reads back without a manufacturer code. ZHA/Z2M
 * ignore it during interview — it never auto-creates an entity.
 */
#define FLEXIT_ATTR_TEMP_LABEL_ID 0xF000

/* How often the ZBOSS-context tick flushes latched values into the clusters. */
#define FLEXIT_PUBLISH_INTERVAL   (ZB_TIME_ONE_SECOND * 2)

/* Stale-data guard: if the source feeding a temperature channel goes
 * silent for longer than this, publish the ZCL "invalid" sentinel (0x8000) so
 * HA shows the sensor as *unknown* rather than a frozen last reading. The
 * real RS485 FC10 panel broadcast refreshes well inside this window, so live
 * entities are unaffected — only an actual source dropout trips it. Tune if
 * the bus is slower than expected. Whole-device drop (bridge/mesh down) is handled
 * separately by ZHA's own availability tracking — see smarthouse-integration.md.
 */
#define FLEXIT_TEMP_STALE_MS      60000

/* FanMode enum is identical to the Flexit mode numbering for 0..3:
 *   Off(0)=Stop  Low(1)=Min  Medium(2)=Normal  High(3)=Max
 * (ON/AUTO/SMART, 4..6, are not meaningful for the CS60.)
 */
BUILD_ASSERT(ZB_ZCL_FAN_CONTROL_FAN_MODE_OFF == 0 &&
	     ZB_ZCL_FAN_CONTROL_FAN_MODE_HIGH == 3,
	     "FanMode<->Flexit mode mapping assumes Off..High == 0..3");

/* ------------------------------------------------------------------------- */
/* Attribute storage                                                         */
/* ------------------------------------------------------------------------- */
struct zb_device_ctx {
	zb_zcl_basic_attrs_t    basic_attr;
	zb_zcl_identify_attrs_t identify_attr;
	zb_uint8_t              fan_mode;
	zb_uint8_t              fan_mode_seq;
};

static struct zb_device_ctx dev_ctx;

static zigbee_ep_mode_write_cb_t     mode_write_cb;
static zigbee_ep_setpoint_write_cb_t setpoint_write_cb;

/* Latched cluster values, produced by the setters and consumed by publish_tick. */
static struct {
	int16_t temp_cc[ZIGBEE_TEMP_COUNT];
	bool    temp_dirty[ZIGBEE_TEMP_COUNT];
	int64_t temp_last_ms[ZIGBEE_TEMP_COUNT]; /* uptime of last fresh value; 0 = never */
	float   analog_val[ZIGBEE_ANALOG_COUNT]; /* percentage, 0..100 */
	bool    analog_dirty[ZIGBEE_ANALOG_COUNT];
	float   setpoint_val;     /* setpoint readback, °C */
	bool    setpoint_dirty;
	int64_t setpoint_hold_until; /* skip readback latch until this uptime (ms) */
	uint8_t mode;       /* Flexit 0..3 */
	bool    mode_dirty;
} pending;
static K_MUTEX_DEFINE(pending_lock);

/* Per-channel staleness, owned by publish_tick (ZBOSS thread only). Starts true
 * because the attributes initialise to ...VALUE_UNKNOWN; the first fresh value
 * clears it, and a source dropout sets it again (publishing the sentinel once).
 */
static bool temp_stale[ZIGBEE_TEMP_COUNT];

static const uint8_t temp_ep_id[ZIGBEE_TEMP_COUNT] = {
	[ZIGBEE_TEMP_SUPPLY]  = FLEXIT_TEMP_EP_SUPPLY,
};

static const uint8_t analog_ep_id[ZIGBEE_ANALOG_COUNT] = {
	[ZIGBEE_ANALOG_HEAT_EXCHANGER] = FLEXIT_AI_EP_HEAT_EXCH,
	[ZIGBEE_ANALOG_HEATING]        = FLEXIT_AI_EP_HEATING,
	[ZIGBEE_ANALOG_INTAKE_TEMP]    = FLEXIT_AI_EP_INTAKE,
};

/* ------------------------------------------------------------------------- */
/* EP1 — Basic + Identify + Fan Control                                      */
/* ------------------------------------------------------------------------- */
ZB_ZCL_DECLARE_IDENTIFY_ATTRIB_LIST(
	ctrl_identify_attrs,
	&dev_ctx.identify_attr.identify_time);

/* ZCL character strings: leading length byte then the text. The length byte is
 * kept in its own string literal so the next char can't be eaten by the \x
 * escape (e.g. "\x09f..." would parse as 0x9f).
 */
static zb_char_t basic_mf_name[]  = "\x0b" "SolidSystem";  /* 11 chars */
static zb_char_t basic_model_id[] = "\x13" "flexit-cs60-control";  /* 19 chars */

/* Basic cluster by hand so it carries ManufacturerName + ModelIdentifier (used
 * by HA for the device name and by Z2M to match an external converter).
 */
ZB_ZCL_START_DECLARE_ATTRIB_LIST_CLUSTER_REVISION(ctrl_basic_attrs, ZB_ZCL_BASIC)
	ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_BASIC_ZCL_VERSION_ID,
		&dev_ctx.basic_attr.zcl_version)
	ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, basic_mf_name)
	ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, basic_model_id)
	ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_BASIC_POWER_SOURCE_ID,
		&dev_ctx.basic_attr.power_source)
ZB_ZCL_FINISH_DECLARE_ATTRIB_LIST;

/* The canned ZB_ZCL_DECLARE_FAN_CONTROL_ATTRIB_LIST marks FanMode read/write
 * only (ZB_ZCL_FAN_CONTROL_REPORT_ATTR_COUNT == 0). Declare it by hand so
 * FanMode is also reportable and HA gets push updates on mode changes.
 */
ZB_ZCL_START_DECLARE_ATTRIB_LIST_CLUSTER_REVISION(ctrl_fan_attrs, ZB_ZCL_FAN_CONTROL)
	ZB_ZCL_SET_ATTR_DESC_M(ZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_ID, &dev_ctx.fan_mode,
		ZB_ZCL_ATTR_TYPE_8BIT_ENUM,
		ZB_ZCL_ATTR_ACCESS_READ_WRITE | ZB_ZCL_ATTR_ACCESS_REPORTING)
	ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_SEQUENCE_ID,
		&dev_ctx.fan_mode_seq)
ZB_ZCL_FINISH_DECLARE_ATTRIB_LIST;

static zb_zcl_cluster_desc_t ctrl_clusters[] = {
	ZB_ZCL_CLUSTER_DESC(ZB_ZCL_CLUSTER_ID_BASIC,
		ZB_ZCL_ARRAY_SIZE(ctrl_basic_attrs, zb_zcl_attr_t), ctrl_basic_attrs,
		ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ZCL_MANUF_CODE_INVALID),
	ZB_ZCL_CLUSTER_DESC(ZB_ZCL_CLUSTER_ID_IDENTIFY,
		ZB_ZCL_ARRAY_SIZE(ctrl_identify_attrs, zb_zcl_attr_t), ctrl_identify_attrs,
		ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ZCL_MANUF_CODE_INVALID),
	ZB_ZCL_CLUSTER_DESC(ZB_ZCL_CLUSTER_ID_FAN_CONTROL,
		ZB_ZCL_ARRAY_SIZE(ctrl_fan_attrs, zb_zcl_attr_t), ctrl_fan_attrs,
		ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ZCL_MANUF_CODE_INVALID),
};

ZB_DECLARE_SIMPLE_DESC(3, 0);
static zb_af_simple_desc_3_0_t simple_desc_ctrl = {
	FLEXIT_CTRL_ENDPOINT, ZB_AF_HA_PROFILE_ID, ZB_HA_THERMOSTAT_DEVICE_ID,
	FLEXIT_DEVICE_VERSION, 0, 3, 0,
	{ ZB_ZCL_CLUSTER_ID_BASIC, ZB_ZCL_CLUSTER_ID_IDENTIFY, ZB_ZCL_CLUSTER_ID_FAN_CONTROL },
};

ZBOSS_DEVICE_DECLARE_REPORTING_CTX(reporting_ctrl, 1 /* FanMode */);
ZB_AF_DECLARE_ENDPOINT_DESC(ctrl_ep, FLEXIT_CTRL_ENDPOINT, ZB_AF_HA_PROFILE_ID,
	0, NULL,
	ZB_ZCL_ARRAY_SIZE(ctrl_clusters, zb_zcl_cluster_desc_t), ctrl_clusters,
	(zb_af_simple_desc_1_1_t *)&simple_desc_ctrl, 1, reporting_ctrl, 0, NULL);

/* ------------------------------------------------------------------------- */
/* EP2 — Temperature Measurement (supply air)                                */
/* ------------------------------------------------------------------------- */
ZB_DECLARE_SIMPLE_DESC(1, 0);

/* `label` is a ZCL character string: a length-prefix byte then the text, the
 * length byte in its own literal so the next char can't be eaten by the \x
 * escape (e.g. "\x06" "supply"). See basic_mf_name above for the same idiom.
 * The attribute list is hand-declared (rather than via
 * ZB_ZCL_DECLARE_TEMP_MEASUREMENT_ATTRIB_LIST) so it can carry the extra
 * FLEXIT_ATTR_TEMP_LABEL_ID string alongside the four standard attributes.
 */
#define FLEXIT_TEMP_EP(name, ep_id, label)                                       \
	static zb_int16_t  name##_value = ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_UNKNOWN; \
	static zb_int16_t  name##_min   = FLEXIT_TEMP_MIN_CC;                         \
	static zb_int16_t  name##_max   = FLEXIT_TEMP_MAX_CC;                         \
	static zb_uint16_t name##_tol   = 0;                                         \
	static zb_char_t   name##_label[] = label;                                   \
	ZB_ZCL_START_DECLARE_ATTRIB_LIST_CLUSTER_REVISION(name##_attrs, ZB_ZCL_TEMP_MEASUREMENT) \
		ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID, &name##_value)     \
		ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_TEMP_MEASUREMENT_MIN_VALUE_ID, &name##_min)   \
		ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_TEMP_MEASUREMENT_MAX_VALUE_ID, &name##_max)   \
		ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_TEMP_MEASUREMENT_TOLERANCE_ID, &name##_tol)   \
		ZB_ZCL_SET_ATTR_DESC_M(FLEXIT_ATTR_TEMP_LABEL_ID, name##_label,               \
			ZB_ZCL_ATTR_TYPE_CHAR_STRING, ZB_ZCL_ATTR_ACCESS_READ_ONLY)          \
	ZB_ZCL_FINISH_DECLARE_ATTRIB_LIST;                                           \
	static zb_zcl_cluster_desc_t name##_clusters[] = {                          \
		ZB_ZCL_CLUSTER_DESC(ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,             \
			ZB_ZCL_ARRAY_SIZE(name##_attrs, zb_zcl_attr_t), name##_attrs,  \
			ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ZCL_MANUF_CODE_INVALID),        \
	};                                                                          \
	static zb_af_simple_desc_1_0_t simple_desc_##name = {                        \
		ep_id, ZB_AF_HA_PROFILE_ID, ZB_HA_TEMPERATURE_SENSOR_DEVICE_ID,     \
		FLEXIT_DEVICE_VERSION, 0, 1, 0, { ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT } }; \
	ZBOSS_DEVICE_DECLARE_REPORTING_CTX(reporting_##name,                         \
		ZB_ZCL_TEMP_MEASUREMENT_REPORT_ATTR_COUNT);                         \
	ZB_AF_DECLARE_ENDPOINT_DESC(name##_ep, ep_id, ZB_AF_HA_PROFILE_ID, 0, NULL,  \
		ZB_ZCL_ARRAY_SIZE(name##_clusters, zb_zcl_cluster_desc_t), name##_clusters, \
		(zb_af_simple_desc_1_1_t *)&simple_desc_##name,                     \
		ZB_ZCL_TEMP_MEASUREMENT_REPORT_ATTR_COUNT, reporting_##name, 0, NULL)

FLEXIT_TEMP_EP(supply,    FLEXIT_TEMP_EP_SUPPLY,     "\x06" "supply");

/* ------------------------------------------------------------------------- */
/* EP4/EP5/EP6 — Analog Input (Basic), one cluster each                       */
/*                                                                            */
/* A generic read-only numeric sensor: ZHA/Z2M expose PresentValue (float) as */
/* a sensor entity, named from Description and unit-tagged via EngineeringUnits*/
/* (98 = percent, 62 = °C) / ApplicationType. PresentValue + StatusFlags are  */
/* reportable (ZB_ZCL_ANALOG_INPUT_REPORT_ATTR_COUNT == 2), so HA gets pushed  */
/* updates on the same publish tick as the temps. The canned attrib-list macro*/
/* carries the full mandatory+optional set; values are seeded in              */
/* app_clusters_attr_init() and refreshed by publish_tick().                  */
/*                                                                            */
/* min/max bound the displayed range; resolution drives HA's display precision*/
/* (1.0 -> 0 decimals for %, 0.1 -> 1 decimal for °C). units is the BACnet    */
/* EngineeringUnits value; app_type is the BACnet application type.            */
/*                                                                            */
/* ZHA's AnalogInputSensor picks unit + device_class from ApplicationType when */
/* that attribute is present (the temperature app-type group forces both °C   */
/* AND device_class=temperature). When ApplicationType is *absent* it instead  */
/* derives only the unit from EngineeringUnits and leaves device_class null.   */
/* The percentage endpoints keep ApplicationType (percentage maps to a null    */
/* device_class anyway); the intake-temp endpoint uses FLEXIT_AI_EP_NO_APPTYPE */
/* below to get °C with a null device_class (matching the % sensors' shape).   */
/*                                                                            */
/* Why null device_class is wanted here: with device_class=temperature HA     */
/* treats the entity as a generic temperature sensor and names it just         */
/* "Temperature", discarding the endpoint's own name. With device_class null   */
/* HA falls back to the cluster Description, so the entity reads as            */
/* "Intake air temperature" — the label we actually want — while still         */
/* showing °C.                                                                 */
/* ------------------------------------------------------------------------- */
#define FLEXIT_AI_EP(name, ep_id, label, minval, maxval, res, units, app_type)   \
	static zb_char_t   name##_desc[]   = label;                                  \
	static zb_single_t name##_present  = 0.0f;                                   \
	static zb_single_t name##_min       = (minval);                              \
	static zb_single_t name##_max       = (maxval);                              \
	static zb_single_t name##_resolution = (res);                                \
	static zb_bool_t   name##_oos      = ZB_FALSE;                               \
	static zb_uint8_t  name##_reliab   = 0;                                      \
	static zb_uint8_t  name##_status   = ZB_ZCL_ANALOG_INPUT_STATUS_FLAG_NORMAL; \
	static zb_uint16_t name##_units    = (units);                               \
	static zb_uint32_t name##_app_type = (app_type);                            \
	ZB_ZCL_DECLARE_ANALOG_INPUT_ATTRIB_LIST(name##_attrs,                        \
		name##_desc, &name##_max, &name##_min, &name##_oos, &name##_present, \
		&name##_reliab, &name##_resolution, &name##_status, &name##_units,  \
		&name##_app_type);                                                  \
	static zb_zcl_cluster_desc_t name##_clusters[] = {                          \
		ZB_ZCL_CLUSTER_DESC(ZB_ZCL_CLUSTER_ID_ANALOG_INPUT,                \
			ZB_ZCL_ARRAY_SIZE(name##_attrs, zb_zcl_attr_t), name##_attrs,  \
			ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ZCL_MANUF_CODE_INVALID),        \
	};                                                                          \
	static zb_af_simple_desc_1_0_t simple_desc_##name = {                        \
		ep_id, ZB_AF_HA_PROFILE_ID, ZB_HA_SIMPLE_SENSOR_DEVICE_ID,         \
		FLEXIT_DEVICE_VERSION, 0, 1, 0, { ZB_ZCL_CLUSTER_ID_ANALOG_INPUT } }; \
	ZBOSS_DEVICE_DECLARE_REPORTING_CTX(reporting_##name,                         \
		ZB_ZCL_ANALOG_INPUT_REPORT_ATTR_COUNT);                            \
	ZB_AF_DECLARE_ENDPOINT_DESC(name##_ep, ep_id, ZB_AF_HA_PROFILE_ID, 0, NULL,  \
		ZB_ZCL_ARRAY_SIZE(name##_clusters, zb_zcl_cluster_desc_t), name##_clusters, \
		(zb_af_simple_desc_1_1_t *)&simple_desc_##name,                     \
		ZB_ZCL_ANALOG_INPUT_REPORT_ATTR_COUNT, reporting_##name, 0, NULL)

/* As FLEXIT_AI_EP, but the ApplicationType (0x0100) attribute is deliberately
 * omitted from the attribute list. With no ApplicationType to report, ZHA's
 * AnalogInputSensor falls back to deriving the unit from EngineeringUnits and
 * leaves device_class null — used for the intake temperature so it reads °C
 * with a null device_class instead of device_class=temperature. Null
 * device_class is what we want here: it stops HA from presenting the entity as
 * a generic temperature sensor named just "Temperature", so HA uses the cluster
 * Description and the entity reads as "Intake air temperature" (still in °C).
 * The attribute list is otherwise identical to the canned
 * ZB_ZCL_DECLARE_ANALOG_INPUT_ATTRIB_LIST (DESCRIPTION..ENGINEERING_UNITS), just
 * without the trailing APPLICATION_TYPE.
 */
#define FLEXIT_AI_EP_NO_APPTYPE(name, ep_id, label, minval, maxval, res, units)   \
	static zb_char_t   name##_desc[]   = label;                                  \
	static zb_single_t name##_present  = 0.0f;                                   \
	static zb_single_t name##_min       = (minval);                              \
	static zb_single_t name##_max       = (maxval);                              \
	static zb_single_t name##_resolution = (res);                                \
	static zb_bool_t   name##_oos      = ZB_FALSE;                               \
	static zb_uint8_t  name##_reliab   = 0;                                      \
	static zb_uint8_t  name##_status   = ZB_ZCL_ANALOG_INPUT_STATUS_FLAG_NORMAL; \
	static zb_uint16_t name##_units    = (units);                               \
	ZB_ZCL_START_DECLARE_ATTRIB_LIST_CLUSTER_REVISION(name##_attrs, ZB_ZCL_ANALOG_INPUT) \
		ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_ANALOG_INPUT_DESCRIPTION_ID, name##_desc)       \
		ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_ANALOG_INPUT_MAX_PRESENT_VALUE_ID, &name##_max) \
		ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_ANALOG_INPUT_MIN_PRESENT_VALUE_ID, &name##_min) \
		ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_ANALOG_INPUT_OUT_OF_SERVICE_ID, &name##_oos)    \
		ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID, &name##_present) \
		ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_ANALOG_INPUT_RELIABILITY_ID, &name##_reliab)    \
		ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_ANALOG_INPUT_RESOLUTION_ID, &name##_resolution) \
		ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_ANALOG_INPUT_STATUS_FLAGS_ID, &name##_status)   \
		ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_ANALOG_INPUT_ENGINEERING_UNITS_ID, &name##_units) \
	ZB_ZCL_FINISH_DECLARE_ATTRIB_LIST;                                           \
	static zb_zcl_cluster_desc_t name##_clusters[] = {                          \
		ZB_ZCL_CLUSTER_DESC(ZB_ZCL_CLUSTER_ID_ANALOG_INPUT,                \
			ZB_ZCL_ARRAY_SIZE(name##_attrs, zb_zcl_attr_t), name##_attrs,  \
			ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ZCL_MANUF_CODE_INVALID),        \
	};                                                                          \
	static zb_af_simple_desc_1_0_t simple_desc_##name = {                        \
		ep_id, ZB_AF_HA_PROFILE_ID, ZB_HA_SIMPLE_SENSOR_DEVICE_ID,         \
		FLEXIT_DEVICE_VERSION, 0, 1, 0, { ZB_ZCL_CLUSTER_ID_ANALOG_INPUT } }; \
	ZBOSS_DEVICE_DECLARE_REPORTING_CTX(reporting_##name,                         \
		ZB_ZCL_ANALOG_INPUT_REPORT_ATTR_COUNT);                            \
	ZB_AF_DECLARE_ENDPOINT_DESC(name##_ep, ep_id, ZB_AF_HA_PROFILE_ID, 0, NULL,  \
		ZB_ZCL_ARRAY_SIZE(name##_clusters, zb_zcl_cluster_desc_t), name##_clusters, \
		(zb_af_simple_desc_1_1_t *)&simple_desc_##name,                     \
		ZB_ZCL_ANALOG_INPUT_REPORT_ATTR_COUNT, reporting_##name, 0, NULL)

/* Intake air temperature (°C). No ApplicationType (see FLEXIT_AI_EP_NO_APPTYPE)
 * so ZHA shows °C from EngineeringUnits with a null device_class.
 */
FLEXIT_AI_EP_NO_APPTYPE(intake, FLEXIT_AI_EP_INTAKE, "\x16" "Intake air temperature", /* 22 */
	FLEXIT_AI_TEMP_MIN_C, FLEXIT_AI_TEMP_MAX_C, 0.1f, FLEXIT_AI_UNITS_DEGC);
FLEXIT_AI_EP(heat_exch, FLEXIT_AI_EP_HEAT_EXCH, "\x0e" "Heat exchanger",   /* 14 */
	0.0f, 100.0f, 1.0f, FLEXIT_AI_UNITS_PERCENT, ZB_ZCL_AI_PERCENTAGE_OTHER);
FLEXIT_AI_EP(heating,   FLEXIT_AI_EP_HEATING,   "\x0f" "Heating element",  /* 15 */
	0.0f, 100.0f, 1.0f, FLEXIT_AI_UNITS_PERCENT, ZB_ZCL_AI_PERCENTAGE_OTHER);

/* ------------------------------------------------------------------------- */
/* EP7 — Analog Value (Basic): writable temperature setpoint                  */
/*                                                                            */
/* PresentValue is read/write (float °C) so a Zigbee client can both read the */
/* CS60's current setpoint and push a new one; the write fires zcl_device_cb  */
/* -> setpoint_write_cb -> flexit_slave_queue_setpoint (coil 12 / reg 0x000C).*/
/* PresentValue is marked reportable (overriding the canned RW-only descriptor)*/
/* so panel-driven changes push to HA. EngineeringUnits = 62 (°C). ZHA does    */
/* not auto-expose Analog Value, so tools/zha-quirk/flexit-cs60-control.py maps it to a   */
/* settable Number entity.                                                     */
/* ------------------------------------------------------------------------- */
static zb_char_t   av_setpoint_desc[]    = "\x08" "setpoint";
static zb_single_t av_setpoint_present   = 0.0f;   /* °C */
static zb_bool_t   av_setpoint_oos       = ZB_FALSE;
static zb_uint8_t  av_setpoint_reliab    = 0;
static zb_single_t av_setpoint_relinq    = 0.0f;
static zb_uint8_t  av_setpoint_status    = ZB_ZCL_ANALOG_VALUE_STATUS_FLAG_NORMAL;
static zb_uint16_t av_setpoint_units     = FLEXIT_AV_UNITS_DEGC;
static zb_uint32_t av_setpoint_app_type  = ZB_ZCL_AV_TEMPERATURE_OTHER;

ZB_ZCL_START_DECLARE_ATTRIB_LIST_CLUSTER_REVISION(av_setpoint_attrs, ZB_ZCL_ANALOG_VALUE)
	ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_ANALOG_VALUE_DESCRIPTION_ID, av_setpoint_desc)
	ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_ANALOG_VALUE_OUT_OF_SERVICE_ID, &av_setpoint_oos)
	ZB_ZCL_SET_ATTR_DESC_M(ZB_ZCL_ATTR_ANALOG_VALUE_PRESENT_VALUE_ID, &av_setpoint_present,
		ZB_ZCL_ATTR_TYPE_SINGLE,
		ZB_ZCL_ATTR_ACCESS_READ_WRITE | ZB_ZCL_ATTR_ACCESS_REPORTING)
	ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_ANALOG_VALUE_RELIABILITY_ID, &av_setpoint_reliab)
	ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_ANALOG_VALUE_RELINQUISH_DEFAULT_ID, &av_setpoint_relinq)
	ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_ANALOG_VALUE_STATUS_FLAGS_ID, &av_setpoint_status)
	ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_ANALOG_VALUE_ENGINEERING_UNITS_ID, &av_setpoint_units)
	ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_ANALOG_VALUE_APPLICATION_TYPE_ID, &av_setpoint_app_type)
ZB_ZCL_FINISH_DECLARE_ATTRIB_LIST;

static zb_zcl_cluster_desc_t av_setpoint_clusters[] = {
	ZB_ZCL_CLUSTER_DESC(ZB_ZCL_CLUSTER_ID_ANALOG_VALUE,
		ZB_ZCL_ARRAY_SIZE(av_setpoint_attrs, zb_zcl_attr_t), av_setpoint_attrs,
		ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ZCL_MANUF_CODE_INVALID),
};
static zb_af_simple_desc_1_0_t simple_desc_av_setpoint = {
	FLEXIT_AV_EP_SETPOINT, ZB_AF_HA_PROFILE_ID, ZB_HA_SIMPLE_SENSOR_DEVICE_ID,
	FLEXIT_DEVICE_VERSION, 0, 1, 0, { ZB_ZCL_CLUSTER_ID_ANALOG_VALUE } };
ZBOSS_DEVICE_DECLARE_REPORTING_CTX(reporting_av_setpoint, 1 /* PresentValue */);
ZB_AF_DECLARE_ENDPOINT_DESC(av_setpoint_ep, FLEXIT_AV_EP_SETPOINT, ZB_AF_HA_PROFILE_ID, 0, NULL,
	ZB_ZCL_ARRAY_SIZE(av_setpoint_clusters, zb_zcl_cluster_desc_t), av_setpoint_clusters,
	(zb_af_simple_desc_1_1_t *)&simple_desc_av_setpoint, 1, reporting_av_setpoint, 0, NULL);

/* No N-EP convenience macro past 4, so declare the 6-endpoint list by hand
 * (same expansion as ZBOSS_DECLARE_DEVICE_CTX_n_EP). 6 app endpoints is within
 * ZB_MAX_EP_NUMBER (8); an end device adds no Green Power endpoint.
 */
ZB_AF_START_DECLARE_ENDPOINT_LIST(ep_list_flexit_ctx)
	&ctrl_ep,
	&supply_ep,
	&intake_ep,
	&heat_exch_ep,
	&heating_ep,
	&av_setpoint_ep,
ZB_AF_FINISH_DECLARE_ENDPOINT_LIST;
ZBOSS_DECLARE_DEVICE_CTX(flexit_ctx, ep_list_flexit_ctx,
	ZB_ZCL_ARRAY_SIZE(ep_list_flexit_ctx, zb_af_endpoint_desc_t *));

/* ------------------------------------------------------------------------- */
/* Attribute init                                                            */
/* ------------------------------------------------------------------------- */
static void app_clusters_attr_init(void)
{
	dev_ctx.basic_attr.zcl_version  = ZB_ZCL_VERSION;
	dev_ctx.basic_attr.power_source = FLEXIT_INIT_BASIC_POWER_SOURCE;

	dev_ctx.identify_attr.identify_time =
		ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE;

	/* Attributes init to ...VALUE_UNKNOWN, so each channel starts stale; the
	 * first fresh setter value flips it and writes the real reading.
	 */
	for (int i = 0; i < ZIGBEE_TEMP_COUNT; i++) {
		temp_stale[i] = true;
	}

	/* Start at Off; advertise the Low/Medium/High sequence to clients. */
	dev_ctx.fan_mode     = ZB_ZCL_FAN_CONTROL_FAN_MODE_OFF;
	dev_ctx.fan_mode_seq = ZB_ZCL_FAN_CONTROL_FAN_MODE_SEQUENCE_LOW_MED_HIGH;
}

/* ------------------------------------------------------------------------- */
/* Fan Control write handling (Zigbee client -> Flexit)                      */
/* ------------------------------------------------------------------------- */
static void handle_fan_mode_write(zb_uint8_t fan_mode)
{
	if (fan_mode > ZB_ZCL_FAN_CONTROL_FAN_MODE_HIGH) {
		LOG_WRN("FanMode write %u unsupported (only Off/Low/Medium/High)",
			fan_mode);
		return;
	}

	uint8_t flexit_mode = fan_mode; /* identity map, 0..3 */

	if (mode_write_cb != NULL) {
		LOG_INF("FanMode write -> Flexit mode %u", flexit_mode);
		mode_write_cb(flexit_mode);
	} else {
		LOG_INF("FanMode write -> Flexit mode %u (stub: no CMD_MODE sink registered)",
			flexit_mode);
	}
}

/* ------------------------------------------------------------------------- */
/* Setpoint write handling (Zigbee client -> Flexit)                         */
/* ------------------------------------------------------------------------- */
static void handle_setpoint_write(float celsius)
{
	/* Round °C to tenths and clamp to the unit's range. */
	int32_t dc = (int32_t)(celsius * 10.0f + (celsius >= 0.0f ? 0.5f : -0.5f));
	if (dc < FLEXIT_SETPOINT_MIN_DC) {
		dc = FLEXIT_SETPOINT_MIN_DC;
	} else if (dc > FLEXIT_SETPOINT_MAX_DC) {
		dc = FLEXIT_SETPOINT_MAX_DC;
	}

	/* Hold off readback so the committed value (still old until the CS60
	 * adopts ours) doesn't transiently overwrite the just-written value.
	 */
	k_mutex_lock(&pending_lock, K_FOREVER);
	pending.setpoint_hold_until = k_uptime_get() + FLEXIT_SETPOINT_WRITE_HOLD_MS;
	k_mutex_unlock(&pending_lock);

	if (setpoint_write_cb != NULL) {
		LOG_INF("Setpoint write -> %d.%d C", (int)dc / 10, (int)dc % 10);
		setpoint_write_cb((int16_t)dc);
	} else {
		LOG_INF("Setpoint write -> %d.%d C (stub: no sink registered)",
			(int)dc / 10, (int)dc % 10);
	}
}

/* Global ZCL device callback — fires after the stack applies a write. */
static void zcl_device_cb(zb_bufid_t bufid)
{
	zb_zcl_device_callback_param_t *p =
		ZB_BUF_GET_PARAM(bufid, zb_zcl_device_callback_param_t);

	p->status = RET_OK;

	if (p->device_cb_id == ZB_ZCL_SET_ATTR_VALUE_CB_ID) {
		zb_uint16_t cluster_id = p->cb_param.set_attr_value_param.cluster_id;
		zb_uint16_t attr_id    = p->cb_param.set_attr_value_param.attr_id;

		if (cluster_id == ZB_ZCL_CLUSTER_ID_FAN_CONTROL &&
		    attr_id == ZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_ID) {
			handle_fan_mode_write(
				p->cb_param.set_attr_value_param.values.data8);
		} else if (cluster_id == ZB_ZCL_CLUSTER_ID_ANALOG_VALUE &&
			   attr_id == ZB_ZCL_ATTR_ANALOG_VALUE_PRESENT_VALUE_ID) {
			/* SINGLE (float) arrives as the raw 32-bit IEEE-754 word. */
			zb_uint32_t raw = p->cb_param.set_attr_value_param.values.data32;
			float celsius;
			memcpy(&celsius, &raw, sizeof(celsius));
			handle_setpoint_write(celsius);
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Publish latched values into the clusters (ZBOSS stack thread)             */
/* ------------------------------------------------------------------------- */
static void publish_tick(zb_uint8_t param)
{
	ARG_UNUSED(param);

	int16_t cc[ZIGBEE_TEMP_COUNT];
	bool    temp_dirty[ZIGBEE_TEMP_COUNT];
	int64_t temp_last_ms[ZIGBEE_TEMP_COUNT];
	float   analog_val[ZIGBEE_ANALOG_COUNT];
	bool    analog_dirty[ZIGBEE_ANALOG_COUNT];
	float   setpoint_val;
	bool    setpoint_dirty;
	uint8_t mode;
	bool    mode_dirty;

	k_mutex_lock(&pending_lock, K_FOREVER);
	for (int i = 0; i < ZIGBEE_TEMP_COUNT; i++) {
		cc[i] = pending.temp_cc[i];
		temp_dirty[i] = pending.temp_dirty[i];
		pending.temp_dirty[i] = false;
		temp_last_ms[i] = pending.temp_last_ms[i];
	}
	for (int i = 0; i < ZIGBEE_ANALOG_COUNT; i++) {
		analog_val[i] = pending.analog_val[i];
		analog_dirty[i] = pending.analog_dirty[i];
		pending.analog_dirty[i] = false;
	}
	setpoint_val = pending.setpoint_val;
	setpoint_dirty = pending.setpoint_dirty;
	pending.setpoint_dirty = false;
	mode = pending.mode;
	mode_dirty = pending.mode_dirty;
	pending.mode_dirty = false;
	k_mutex_unlock(&pending_lock);

	int64_t now = k_uptime_get();
	for (int i = 0; i < ZIGBEE_TEMP_COUNT; i++) {
		if (temp_dirty[i]) {
			/* Fresh value — publish it and clear any prior stale state. */
			zb_zcl_set_attr_val(temp_ep_id[i],
				ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
				ZB_ZCL_CLUSTER_SERVER_ROLE,
				ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
				(zb_uint8_t *)&cc[i], ZB_FALSE);
			temp_stale[i] = false;
		} else if (!temp_stale[i] && temp_last_ms[i] != 0 &&
			   (now - temp_last_ms[i]) > FLEXIT_TEMP_STALE_MS) {
			/* Source went silent — publish the invalid sentinel once so HA
			 * shows "unknown" rather than a frozen last reading.
			 */
			zb_int16_t unknown =
				ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_UNKNOWN;
			zb_zcl_set_attr_val(temp_ep_id[i],
				ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
				ZB_ZCL_CLUSTER_SERVER_ROLE,
				ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
				(zb_uint8_t *)&unknown, ZB_FALSE);
			temp_stale[i] = true;
			LOG_WRN("EP%u temperature source stale (>%d ms) — published invalid",
				temp_ep_id[i], FLEXIT_TEMP_STALE_MS);
		}
	}

	for (int i = 0; i < ZIGBEE_ANALOG_COUNT; i++) {
		if (analog_dirty[i]) {
			zb_zcl_set_attr_val(analog_ep_id[i],
				ZB_ZCL_CLUSTER_ID_ANALOG_INPUT,
				ZB_ZCL_CLUSTER_SERVER_ROLE,
				ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID,
				(zb_uint8_t *)&analog_val[i], ZB_FALSE);
		}
	}

	if (setpoint_dirty) {
		zb_zcl_set_attr_val(FLEXIT_AV_EP_SETPOINT,
			ZB_ZCL_CLUSTER_ID_ANALOG_VALUE,
			ZB_ZCL_CLUSTER_SERVER_ROLE,
			ZB_ZCL_ATTR_ANALOG_VALUE_PRESENT_VALUE_ID,
			(zb_uint8_t *)&setpoint_val, ZB_FALSE);
	}

	if (mode_dirty) {
		zb_uint8_t fan_mode = mode; /* identity map, 0..3 */
		zb_zcl_set_attr_val(FLEXIT_CTRL_ENDPOINT,
			ZB_ZCL_CLUSTER_ID_FAN_CONTROL,
			ZB_ZCL_CLUSTER_SERVER_ROLE,
			ZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_ID,
			&fan_mode, ZB_FALSE);
	}

	ZB_SCHEDULE_APP_ALARM(publish_tick, 0, FLEXIT_PUBLISH_INTERVAL);
}

/* ------------------------------------------------------------------------- */
/* Public setters (thread-safe)                                              */
/* ------------------------------------------------------------------------- */
void zigbee_ep_set_temperature(enum zigbee_temp_channel ch, int16_t centi_celsius)
{
	if (ch >= ZIGBEE_TEMP_COUNT) {
		return;
	}
	k_mutex_lock(&pending_lock, K_FOREVER);
	pending.temp_cc[ch] = centi_celsius;
	pending.temp_dirty[ch] = true;
	pending.temp_last_ms[ch] = k_uptime_get();
	k_mutex_unlock(&pending_lock);
}

void zigbee_ep_set_percent(enum zigbee_analog_channel ch, uint16_t percent)
{
	if (ch >= ZIGBEE_ANALOG_COUNT) {
		return;
	}
	k_mutex_lock(&pending_lock, K_FOREVER);
	pending.analog_val[ch] = (float)percent;
	pending.analog_dirty[ch] = true;
	k_mutex_unlock(&pending_lock);
}

void zigbee_ep_set_intake_temp(int16_t value_dc)
{
	k_mutex_lock(&pending_lock, K_FOREVER);
	pending.analog_val[ZIGBEE_ANALOG_INTAKE_TEMP] = (float)value_dc / 10.0f;
	pending.analog_dirty[ZIGBEE_ANALOG_INTAKE_TEMP] = true;
	k_mutex_unlock(&pending_lock);
}

void zigbee_ep_set_mode(uint8_t flexit_mode)
{
	if (flexit_mode > 3) {
		return;
	}
	k_mutex_lock(&pending_lock, K_FOREVER);
	pending.mode = flexit_mode;
	pending.mode_dirty = true;
	k_mutex_unlock(&pending_lock);
}

void zigbee_ep_set_mode_write_handler(zigbee_ep_mode_write_cb_t cb)
{
	mode_write_cb = cb;
}

void zigbee_ep_set_setpoint(int16_t value_dc)
{
	k_mutex_lock(&pending_lock, K_FOREVER);
	/* Skip readback during the post-write hold so we don't clobber a value
	 * the client just set with the CS60's not-yet-updated committed reading.
	 */
	if (k_uptime_get() >= pending.setpoint_hold_until) {
		pending.setpoint_val = (float)value_dc / 10.0f;
		pending.setpoint_dirty = true;
	}
	k_mutex_unlock(&pending_lock);
}

void zigbee_ep_set_setpoint_write_handler(zigbee_ep_setpoint_write_cb_t cb)
{
	setpoint_write_cb = cb;
}

/* ------------------------------------------------------------------------- */
/* Factory reset (re-pairing)                                                */
/*                                                                           */
/* zb_bdb_reset_via_local_action() leaves the network and wipes the ZBOSS    */
/* persistent data, then raises ZB_ZDO_SIGNAL_LEAVE. For an end device the   */
/* default signal handler reacts to that leave with a rejoin attempt, not a  */
/* fresh steering — so to actually become joinable again we reboot. A boot   */
/* with cleared NVRAM is a DEVICE_FIRST_START, which the default handler      */
/* turns into BDB network steering (the join/pairing scan).                  */
/* ------------------------------------------------------------------------- */
static atomic_t factory_reset_pending = ATOMIC_INIT(0);

static void reboot_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	LOG_WRN("Zigbee factory reset: rebooting to begin fresh commissioning");
	sys_reboot(SYS_REBOOT_COLD);
}
static K_WORK_DELAYABLE_DEFINE(reboot_work, reboot_work_fn);

void zigbee_ep_factory_reset(void)
{
	if (atomic_set(&factory_reset_pending, 1) == 1) {
		LOG_INF("Zigbee factory reset already in progress");
		return;
	}

	LOG_WRN("Zigbee factory reset requested — leaving network and clearing NVRAM");

	/* ZB_SCHEDULE_APP_CALLBACK resolves to the thread-safe
	 * zigbee_schedule_callback() in the Zephyr build, so this is safe to
	 * call from the BLE command thread. The reset runs in ZBOSS context.
	 */
	zb_ret_t ret = ZB_SCHEDULE_APP_CALLBACK(zb_bdb_reset_via_local_action, 0);
	if (ret != RET_OK) {
		LOG_ERR("Failed to schedule Zigbee factory reset (ret %d)", ret);
		atomic_set(&factory_reset_pending, 0);
		return;
	}

	/* Fallback: reboot even if the LEAVE signal never arrives (e.g. the
	 * device was not joined). The LEAVE handler expedites this once the
	 * leave is actually processed.
	 */
	k_work_reschedule(&reboot_work, K_SECONDS(5));
}

/* ------------------------------------------------------------------------- */
/* ZBOSS signal handling                                                     */
/* ------------------------------------------------------------------------- */
void zboss_signal_handler(zb_bufid_t bufid)
{
	static bool publish_started;

	zb_zdo_app_signal_hdr_t *sig_hdr = NULL;
	zb_zdo_app_signal_type_t sig = zb_get_app_signal(bufid, &sig_hdr);

	/* Default handling: join/rejoin, steering, etc. */
	ZB_ERROR_CHECK(zigbee_default_signal_handler(bufid));

	/* Once a requested factory reset has produced the network-leave, the
	 * NVRAM is cleared; reboot promptly so the next boot is a clean
	 * DEVICE_FIRST_START and steers for a new coordinator. (k_work_reschedule
	 * shortens the fallback timer armed in zigbee_ep_factory_reset.)
	 */
	if (sig == ZB_ZDO_SIGNAL_LEAVE &&
	    atomic_get(&factory_reset_pending)) {
		k_work_reschedule(&reboot_work, K_SECONDS(1));
	}

	/* Once the stack has started, drive the periodic publish tick from the
	 * ZBOSS thread. Attribute writes before joining are local-only; reports
	 * begin once the device is joined and reporting is configured.
	 */
	if (!publish_started &&
	    (sig == ZB_BDB_SIGNAL_DEVICE_FIRST_START ||
	     sig == ZB_BDB_SIGNAL_DEVICE_REBOOT)) {
		publish_started = true;
		ZB_SCHEDULE_APP_ALARM(publish_tick, 0, FLEXIT_PUBLISH_INTERVAL);
		LOG_INF("Zigbee publish tick started");
	}

	if (bufid) {
		zb_buf_free(bufid);
	}
}

/* ------------------------------------------------------------------------- */
/* Init                                                                      */
/* ------------------------------------------------------------------------- */
int zigbee_ep_init(void)
{
	ZB_AF_REGISTER_DEVICE_CTX(&flexit_ctx);
	ZB_ZCL_REGISTER_DEVICE_CB(zcl_device_cb);

	app_clusters_attr_init();

	zigbee_enable();

	LOG_INF("Zigbee data model started (EP1 fan + EP%d temp + EP%d/%d/%d analog + EP%d setpoint)",
		FLEXIT_TEMP_EP_SUPPLY,
		FLEXIT_AI_EP_INTAKE, FLEXIT_AI_EP_HEAT_EXCH, FLEXIT_AI_EP_HEATING,
		FLEXIT_AV_EP_SETPOINT);

	return 0;
}
