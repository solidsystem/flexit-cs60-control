/* Zigbee data model for the flexitMC bridge — Phase 2.
 *
 * A custom Zigbee end device:
 *   EP1  Basic + Identify + Fan Control (FanMode rw, reportable)
 *   EP2  Temperature Measurement  (supply air)
 *   EP3  Temperature Measurement  (extract air)
 *   EP4  Temperature Measurement  (outdoor air)
 *
 * The endpoints are hand-declared (rather than via a canned HA device-type
 * macro) so EP1 can carry Fan Control and so the three temperature endpoints
 * can share one simple-descriptor type — ZB_DECLARE_SIMPLE_DESC() typedefs a
 * struct keyed by the (in,out) cluster counts, so reusing a canned single-EP
 * macro three times would redefine the same struct.
 *
 * Values arrive through the public setters from arbitrary threads; they are
 * latched under a mutex and pushed into the ZCL attributes by publish_tick(),
 * which runs on the ZBOSS stack thread (ZCL attribute writes must happen in
 * ZBOSS context). See smarthouse-integration.md.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zboss_api.h>
#include <zboss_api_af.h>
#include <zcl/zb_zcl_common.h>
#include <zcl/zb_zcl_basic.h>
#include <zcl/zb_zcl_identify.h>
#include <zcl/zb_zcl_temp_measurement.h>
#include <zcl/zb_zcl_fan_control.h>
#include <ha/zb_ha_device_config.h>
#include <zigbee/zigbee_error_handler.h>
#include <zigbee/zigbee_app_utils.h>
#include <zb_nrf_platform.h>

#include "zigbee_ep.h"

LOG_MODULE_REGISTER(zigbee_ep, LOG_LEVEL_INF);

/* --- Endpoint IDs --- */
#define FLEXIT_CTRL_ENDPOINT      1   /* Basic + Identify + Fan Control */
#define FLEXIT_TEMP_EP_SUPPLY     2
#define FLEXIT_TEMP_EP_EXTRACT    3
#define FLEXIT_TEMP_EP_OUTDOOR    4

#define FLEXIT_DEVICE_VERSION     0
#define FLEXIT_INIT_BASIC_POWER_SOURCE  ZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE

/* Temperature Measurement valid range (hundredths of a degree Celsius). */
#define FLEXIT_TEMP_MIN_CC        (-4000)  /* -40.00 C */
#define FLEXIT_TEMP_MAX_CC        ( 8000)  /*  80.00 C */

/* How often the ZBOSS-context tick flushes latched values into the clusters. */
#define FLEXIT_PUBLISH_INTERVAL   (ZB_TIME_ONE_SECOND * 2)

/* Phase 2: synthesise cluster values so the data path is verifiable with no
 * CS60/RS485 bus connected. Phase 4 sets this to 0 and feeds the setters from
 * the RS485 decode instead.
 */
#define FLEXIT_SYNTHETIC_DATA     1
#define FLEXIT_SYNTH_INTERVAL_S   5

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

static zigbee_ep_mode_write_cb_t mode_write_cb;

/* Latched cluster values, produced by the setters and consumed by publish_tick. */
static struct {
	int16_t temp_cc[ZIGBEE_TEMP_COUNT];
	bool    temp_dirty[ZIGBEE_TEMP_COUNT];
	uint8_t mode;       /* Flexit 0..3 */
	bool    mode_dirty;
} pending;
static K_MUTEX_DEFINE(pending_lock);

static const uint8_t temp_ep_id[ZIGBEE_TEMP_COUNT] = {
	[ZIGBEE_TEMP_SUPPLY]  = FLEXIT_TEMP_EP_SUPPLY,
	[ZIGBEE_TEMP_EXTRACT] = FLEXIT_TEMP_EP_EXTRACT,
	[ZIGBEE_TEMP_OUTDOOR] = FLEXIT_TEMP_EP_OUTDOOR,
};

/* ------------------------------------------------------------------------- */
/* EP1 — Basic + Identify + Fan Control                                      */
/* ------------------------------------------------------------------------- */
ZB_ZCL_DECLARE_IDENTIFY_ATTRIB_LIST(
	ctrl_identify_attrs,
	&dev_ctx.identify_attr.identify_time);

ZB_ZCL_DECLARE_BASIC_ATTRIB_LIST(
	ctrl_basic_attrs,
	&dev_ctx.basic_attr.zcl_version,
	&dev_ctx.basic_attr.power_source);

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
/* EP2..EP4 — Temperature Measurement (one cluster each)                     */
/* ------------------------------------------------------------------------- */
ZB_DECLARE_SIMPLE_DESC(1, 0);

#define FLEXIT_TEMP_EP(name, ep_id)                                              \
	static zb_int16_t  name##_value = ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_UNKNOWN; \
	static zb_int16_t  name##_min   = FLEXIT_TEMP_MIN_CC;                         \
	static zb_int16_t  name##_max   = FLEXIT_TEMP_MAX_CC;                         \
	static zb_uint16_t name##_tol   = 0;                                         \
	ZB_ZCL_DECLARE_TEMP_MEASUREMENT_ATTRIB_LIST(name##_attrs, &name##_value,      \
		&name##_min, &name##_max, &name##_tol);                              \
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

FLEXIT_TEMP_EP(supply,  FLEXIT_TEMP_EP_SUPPLY);
FLEXIT_TEMP_EP(extract, FLEXIT_TEMP_EP_EXTRACT);
FLEXIT_TEMP_EP(outdoor, FLEXIT_TEMP_EP_OUTDOOR);

ZBOSS_DECLARE_DEVICE_CTX_4_EP(flexit_ctx, ctrl_ep, supply_ep, extract_ep, outdoor_ep);

/* ------------------------------------------------------------------------- */
/* Attribute init                                                            */
/* ------------------------------------------------------------------------- */
static void app_clusters_attr_init(void)
{
	dev_ctx.basic_attr.zcl_version  = ZB_ZCL_VERSION;
	dev_ctx.basic_attr.power_source = FLEXIT_INIT_BASIC_POWER_SOURCE;

	dev_ctx.identify_attr.identify_time =
		ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE;

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
	uint8_t mode;
	bool    mode_dirty;

	k_mutex_lock(&pending_lock, K_FOREVER);
	for (int i = 0; i < ZIGBEE_TEMP_COUNT; i++) {
		cc[i] = pending.temp_cc[i];
		temp_dirty[i] = pending.temp_dirty[i];
		pending.temp_dirty[i] = false;
	}
	mode = pending.mode;
	mode_dirty = pending.mode_dirty;
	pending.mode_dirty = false;
	k_mutex_unlock(&pending_lock);

	for (int i = 0; i < ZIGBEE_TEMP_COUNT; i++) {
		if (temp_dirty[i]) {
			zb_zcl_set_attr_val(temp_ep_id[i],
				ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
				ZB_ZCL_CLUSTER_SERVER_ROLE,
				ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
				(zb_uint8_t *)&cc[i], ZB_FALSE);
		}
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

/* ------------------------------------------------------------------------- */
/* Synthetic data generator (Phase 2 only)                                   */
/* ------------------------------------------------------------------------- */
#if FLEXIT_SYNTHETIC_DATA
static void synth_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(synth_work, synth_work_fn);

static void synth_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	static uint32_t tick;

	/* Supply air sweeps 18.00 -> 24.00 C; extract trails +2 C; outdoor sweeps
	 * a colder 5.00 -> 8.00 C — enough movement to exercise reporting.
	 */
	int16_t supply = (int16_t)(1800 + (tick % 61) * 10);
	zigbee_ep_set_temperature(ZIGBEE_TEMP_SUPPLY, supply);
	zigbee_ep_set_temperature(ZIGBEE_TEMP_EXTRACT, (int16_t)(supply + 200));
	zigbee_ep_set_temperature(ZIGBEE_TEMP_OUTDOOR, (int16_t)(500 + (tick % 31) * 10));

	/* Cycle Stop/Min/Normal/Max slowly so writes remain observable between. */
	zigbee_ep_set_mode((uint8_t)((tick / 6) % 4));

	tick++;
	k_work_reschedule(&synth_work, K_SECONDS(FLEXIT_SYNTH_INTERVAL_S));
}
#endif /* FLEXIT_SYNTHETIC_DATA */

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

#if FLEXIT_SYNTHETIC_DATA
	k_work_schedule(&synth_work, K_SECONDS(FLEXIT_SYNTH_INTERVAL_S));
	LOG_INF("Zigbee data model started (EP1 fan + EP%d/%d/%d temps, synthetic feed)",
		FLEXIT_TEMP_EP_SUPPLY, FLEXIT_TEMP_EP_EXTRACT, FLEXIT_TEMP_EP_OUTDOOR);
#else
	LOG_INF("Zigbee data model started (EP1 fan + EP%d/%d/%d temps)",
		FLEXIT_TEMP_EP_SUPPLY, FLEXIT_TEMP_EP_EXTRACT, FLEXIT_TEMP_EP_OUTDOOR);
#endif

	return 0;
}
