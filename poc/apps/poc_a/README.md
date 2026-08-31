# PoC-A ESP-IDF Test Application

This directory is a standard ESP-IDF project. Open `poc/poc-a.code-workspace`, select the `v6.0.2` setup and `esp32s3` target for the `poc-a` folder, then use the extension Build, Flash, Monitor, and Size commands.

The application consumes `frame_poc_runtime` through `EXTRA_COMPONENT_DIRS`; shared PoC runtime code must use `idf_component_register` and must not be added as an ordinary CMake target.

The separate `poc/host` project is only for ABI and pure state-model tests that do not produce ESP32 firmware.
