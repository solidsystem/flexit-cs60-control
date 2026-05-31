cmake_minimum_required(VERSION 3.20.0)

# Zigbee was removed from the main NCS in v3.0 and now ships as the out-of-tree
# ncs-zigbee add-on. Register it as an extra Zephyr module so the app image can
# pull in the ZBOSS stack. Override on the command line with
# -DZEPHYR_EXTRA_MODULES=<path> if the add-on lives elsewhere.
if(NOT DEFINED ZEPHYR_EXTRA_MODULES)
    set(ZEPHYR_EXTRA_MODULES "$ENV{HOME}/ncs/v3.3.0/ncs-zigbee")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(flexit-cs60-control)

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
