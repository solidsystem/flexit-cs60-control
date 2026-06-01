cmake_minimum_required(VERSION 3.20.0)

# BLE pairing passkey is injected at build time from the environment so it is
# never committed to source. The build fails (here, before the rest of the
# configure) if the variable is unset. The value must be a 6-digit number (the
# BLE passkey format; tools/ble-client reads the same variable). It is later
# passed to the app as a string literal and parsed in src/ble_transport.c so a
# value with leading zeros is not read as octal.
if(NOT DEFINED ENV{FLEXIT_CS60_CONTROL_BLE_KEY})
    message(FATAL_ERROR
        "FLEXIT_CS60_CONTROL_BLE_KEY is not set. Export the 6-digit BLE pairing "
        "passkey before building, e.g.\n"
        "    export FLEXIT_CS60_CONTROL_BLE_KEY=444999")
endif()
set(ble_passkey "$ENV{FLEXIT_CS60_CONTROL_BLE_KEY}")
if(NOT ble_passkey MATCHES "^[0-9][0-9][0-9][0-9][0-9][0-9]$")
    message(FATAL_ERROR
        "FLEXIT_CS60_CONTROL_BLE_KEY must be a 6-digit number (got '${ble_passkey}').")
endif()

# Zigbee was removed from the main NCS in v3.0 and now ships as the out-of-tree
# ncs-zigbee add-on. Register it as an extra Zephyr module so the app image can
# pull in the ZBOSS stack. Override on the command line with
# -DZEPHYR_EXTRA_MODULES=<path> if the add-on lives elsewhere.
if(NOT DEFINED ZEPHYR_EXTRA_MODULES)
    set(ZEPHYR_EXTRA_MODULES "$ENV{HOME}/ncs/v3.3.0/ncs-zigbee")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(flexit-cs60-control)

# Bake the validated passkey (checked at the top of this file) into the app as a
# string literal; src/ble_transport.c parses it. zephyr_compile_definitions is
# only available after find_package(Zephyr), hence the split from the check.
zephyr_compile_definitions(FLEXIT_BLE_PASSKEY="${ble_passkey}")

# ZBOSS's built-in "include all ZCL clusters" set (ncs-zigbee zb_vendor.h) omits
# Analog Input, so its server (zcl_analog_input.c) is not compiled and its
# init_server symbol is missing at link. Define the support flag image-wide so
# the cluster source is built and the API header is pulled in. Used by the
# percentage Analog Input endpoints (EP5/EP6) in src/zigbee_ep.c.
zephyr_compile_definitions(ZB_ZCL_SUPPORT_CLUSTER_ANALOG_INPUT)

# Likewise Analog Value (0x000E) is omitted from the built-in set; it backs the
# writable temperature-setpoint endpoint (EP7) in src/zigbee_ep.c.
zephyr_compile_definitions(ZB_ZCL_SUPPORT_CLUSTER_ANALOG_VALUE)

target_sources(app PRIVATE
    src/main.c
    src/ble_transport.c
    src/flexit_bridge.c
    src/flexit_slave.c
    src/panel_mirror.c
    src/rs485_uart.c
    src/zigbee_ep.c
)
