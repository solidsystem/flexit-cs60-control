#!/bin/bash

set -euo pipefail

export SDK_VERSION="v3.4.0"

source ~/ncs/${SDK_VERSION}/zephyr/zephyr-env.sh

nrfutil sdk-manager toolchain launch --ncs-version "${SDK_VERSION}" --shell



