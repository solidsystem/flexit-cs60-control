// Zigbee2MQTT external converter for the flexitMC Flexit CS60 bridge.
//
// UNTESTED against a live Zigbee2MQTT — this is a starting point derived from the
// device's Zigbee signature (see README.md). Adjust for your Z2M version; the
// modernExtend / fan helpers have changed across releases.
//
// Install: copy to <z2m-data>/external_converters/ (Z2M >= 1.30 auto-loads that
// folder) or reference it from configuration.yaml per the Z2M docs, then restart.
//
// Device: one node, four endpoints
//   EP1 hvacFanCtrl (0x0202)  -> fan (Off/Low/Medium/High = Stop/Min/Normal/Max)
//   EP2/3/4 msTemperatureMeasurement (0x0402) -> supply / extract / outdoor temps

const m = require('zigbee-herdsman-converters/lib/modernExtend');
const fz = require('zigbee-herdsman-converters/converters/fromZigbee');
const tz = require('zigbee-herdsman-converters/converters/toZigbee');
const exposes = require('zigbee-herdsman-converters/lib/exposes');
const e = exposes.presets;

const definition = {
    zigbeeModel: ['flexitMC'],
    model: 'flexitMC',
    vendor: 'SolidSystem',
    description: 'Flexit CS60 ventilation bridge (XIAO nRF52840)',
    // Named endpoints so the three temperatures are distinguishable.
    endpoint: () => ({fan: 1, supply: 2, extract: 3, outdoor: 4}),
    meta: {multiEndpoint: true},
    extend: [
        // Auto-binds + configures reporting for msTemperatureMeasurement on each.
        m.temperature({endpointNames: ['supply', 'extract', 'outdoor']}),
    ],
    // Fan Control: read current FanMode, write to change it.
    fromZigbee: [fz.fan],
    toZigbee: [tz.fan_mode],
    exposes: [
        e.fan().withModes(['off', 'low', 'medium', 'high']).withEndpoint('fan'),
    ],
    configure: async (device, coordinatorEndpoint) => {
        // Bind Fan Control so the coordinator receives FanMode reports.
        const ep1 = device.getEndpoint(1);
        await ep1.bind('hvacFanCtrl', coordinatorEndpoint);
        try {
            await ep1.configureReporting('hvacFanCtrl', [{
                attribute: 'fanMode',
                minimumReportInterval: 0,
                maximumReportInterval: 3600,
                reportableChange: 0,
            }]);
        } catch (e) {
            // Some stacks report fanMode as non-reportable; reads still work.
        }
    },
};

module.exports = definition;
