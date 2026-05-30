cmake_minimum_required(VERSION 3.20.0)

# Zigbee was removed from the main NCS in v3.0 and now ships as the out-of-tree
# ncs-zigbee add-on. Register it as an extra Zephyr module so the app image can
# pull in the ZBOSS stack. Override on the command line with
# -DZEPHYR_EXTRA_MODULES=<path> if the add-on lives elsewhere.
if(NOT DEFINED ZEPHYR_EXTRA_MODULES)
    set(ZEPHYR_EXTRA_MODULES "$ENV{HOME}/ncs/v3.3.0/ncs-zigbee")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(flexitMC)

target_sources(app PRIVATE
    src/main.c
    src/ble_transport.c
    src/flexit_bridge.c
    src/flexit_slave.c
    src/panel_mirror.c
    src/rs485_uart.c
    src/zigbee_ep.c
)
