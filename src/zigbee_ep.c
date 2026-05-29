/* Zigbee end-device endpoint for the flexitMC bridge.
 *
 * Phase 1 skeleton: a Home Assistant Temperature Sensor end device (Basic +
 * Identify + Temperature Measurement) that joins a Zigbee network and runs
 * concurrently with the existing BLE stack (debug + DFU). The CS60 mode
 * (Fan Control cluster) and additional temperatures are added in Phase 2.
 *
 * See smarthouse-integration.md for the data model and rationale.
 */

/* Opt in to the HA Temperature Sensor device type before the ZBOSS headers are
 * pulled in — the ZB_HA_DECLARE_TEMPERATURE_SENSOR_* macros are otherwise gated
 * out (only ZB_ALL_DEVICE_SUPPORT enables every device).
 */
#define ZB_HA_DEFINE_DEVICE_TEMPERATURE_SENSOR

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zboss_api.h>
#include <zigbee/zigbee_error_handler.h>
#include <zigbee/zigbee_app_utils.h>
#include <zb_nrf_platform.h>
#include <ha/zb_ha_temperature_sensor.h>

#include "zigbee_ep.h"

LOG_MODULE_REGISTER(zigbee_ep, LOG_LEVEL_INF);

/* Endpoint that hosts the Temperature Measurement cluster. */
#define FLEXIT_TEMP_ENDPOINT            10

/* Mains-powered bridge — report DC power source to the Basic cluster. */
#define FLEXIT_INIT_BASIC_POWER_SOURCE  ZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE

/* Application attribute storage. */
struct zb_device_ctx {
	zb_zcl_basic_attrs_t basic_attr;
	zb_zcl_identify_attrs_t identify_attr;
	zb_int16_t temp_value;
	zb_int16_t temp_min;
	zb_int16_t temp_max;
	zb_uint16_t temp_tolerance;
};

static struct zb_device_ctx dev_ctx;

ZB_ZCL_DECLARE_IDENTIFY_ATTRIB_LIST(
	identify_attr_list,
	&dev_ctx.identify_attr.identify_time);

ZB_ZCL_DECLARE_BASIC_ATTRIB_LIST(
	basic_attr_list,
	&dev_ctx.basic_attr.zcl_version,
	&dev_ctx.basic_attr.power_source);

ZB_ZCL_DECLARE_TEMP_MEASUREMENT_ATTRIB_LIST(
	temp_attr_list,
	&dev_ctx.temp_value,
	&dev_ctx.temp_min,
	&dev_ctx.temp_max,
	&dev_ctx.temp_tolerance);

ZB_HA_DECLARE_TEMPERATURE_SENSOR_CLUSTER_LIST(
	flexit_clusters,
	basic_attr_list,
	identify_attr_list,
	temp_attr_list);

ZB_HA_DECLARE_TEMPERATURE_SENSOR_EP(
	flexit_ep,
	FLEXIT_TEMP_ENDPOINT,
	flexit_clusters);

ZB_HA_DECLARE_TEMPERATURE_SENSOR_CTX(
	flexit_ctx,
	flexit_ep);

static void app_clusters_attr_init(void)
{
	dev_ctx.basic_attr.zcl_version = ZB_ZCL_VERSION;
	dev_ctx.basic_attr.power_source = FLEXIT_INIT_BASIC_POWER_SOURCE;

	dev_ctx.identify_attr.identify_time =
		ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE;

	/* No reading yet — report the ZCL "unknown" sentinel until the CS60
	 * decode feeds real values in Phase 2.
	 */
	dev_ctx.temp_value = ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_UNKNOWN;
	dev_ctx.temp_min = ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_UNKNOWN;
	dev_ctx.temp_max = ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_UNKNOWN;
	dev_ctx.temp_tolerance = 0;
}

void zigbee_ep_set_temperature(int16_t centi_celsius)
{
	zb_zcl_status_t status = zb_zcl_set_attr_val(
		FLEXIT_TEMP_ENDPOINT,
		ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
		ZB_ZCL_CLUSTER_SERVER_ROLE,
		ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
		(zb_uint8_t *)&centi_celsius,
		ZB_FALSE);

	if (status != ZB_ZCL_STATUS_SUCCESS) {
		LOG_WRN("Failed to set temperature attribute: %d", status);
	}
}

/* ZBOSS calls this from its stack thread for every network signal. */
void zboss_signal_handler(zb_bufid_t bufid)
{
	/* Default handling: join/rejoin, steering, etc. */
	ZB_ERROR_CHECK(zigbee_default_signal_handler(bufid));

	if (bufid) {
		zb_buf_free(bufid);
	}
}

int zigbee_ep_init(void)
{
	/* Register endpoints with the application framework. */
	ZB_AF_REGISTER_DEVICE_CTX(&flexit_ctx);

	app_clusters_attr_init();

	/* Start the ZBOSS default stack thread; it will attempt to join a
	 * network and then drive zboss_signal_handler().
	 */
	zigbee_enable();

	LOG_INF("Zigbee end device started (temperature-sensor skeleton, ep %d)",
		FLEXIT_TEMP_ENDPOINT);

	return 0;
}
