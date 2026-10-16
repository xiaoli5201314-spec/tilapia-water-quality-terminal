#!/bin/sh
# 在已有 WSL Ubuntu 中执行主机逻辑测试，不连接任何实物或服务器。
set -eu
cd "$(dirname "$0")/.."
mkdir -p tests/build
cc -std=c99 -Wall -Wextra -Werror -pedantic -fsanitize=undefined,address \
  -DCONTROL_HOST_TEST -Itests/stubs -ICore/Inc -IApplication/Inc -IThirdParty/jsmn \
  tests/test_baseline.c Core/Src/gpio.c Application/Src/*.c \
  -lm -o tests/build/test_baseline
./tests/build/test_baseline
cc -std=c99 -Wall -Wextra -Werror -pedantic -fsanitize=undefined,address \
  -Itests/stubs -ICore/Inc -IApplication/Inc \
  tests/test_water_report.c Application/Src/water_report.c \
  -lm -o tests/build/test_water_report
./tests/build/test_water_report
cc -std=c99 -Wall -Wextra -Werror -pedantic -fsanitize=undefined,address \
  -DCONTROL_HOST_TEST -Itests/stubs -ICore/Inc -IApplication/Inc -IThirdParty/jsmn \
  tests/test_remote.c Core/Src/gpio.c Application/Src/*.c \
  -lm -o tests/build/test_remote
./tests/build/test_remote
cc -std=c99 -Wall -Wextra -Werror -pedantic -fsanitize=undefined,address \
  -DCONTROL_HOST_TEST -Itests/stubs -ICore/Inc -IApplication/Inc -IThirdParty/jsmn \
  tests/test_network_recovery.c Core/Src/gpio.c Application/Src/*.c \
  -lm -o tests/build/test_network_recovery
./tests/build/test_network_recovery
./tests/build/test_network_recovery recovery
./tests/build/test_network_recovery empty
./tests/build/test_network_recovery budgets
