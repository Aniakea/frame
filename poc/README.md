# Architecture Proofs of Concept

This tree is isolated from the product firmware. It may implement only the minimum code needed to prove or disprove the gates in `_doc/project.md`.

Every board PoC under `apps/` is a standard ESP-IDF project, and every shared firmware component
under `common/components/` uses `idf_component_register`. The checked-in CMake presets, VS Code
ESP-IDF extension, and CI all use the same generated sdkconfig location. Board evidence may be
collected with the extension or the documented `idf.py` commands; ordinary host CMake must not link
ESP-IDF firmware targets.

## Gate Order

1. PoC-A: toolchain, dynamic ELF, scheduler, cancellation, and memory behavior
2. PoC-B: display latency and SPI bandwidth
3. PoC-C: lifecycle, update rollback, watchdog, and fault handling
4. PoC-D: system/SD storage isolation and recovery
5. PoC-E: power-loss durability, package fuzzing, and production security

A failed gate stops later gates and returns the architecture to review. PoC code must not be linked into the product components before all gates pass and the architecture is signed again.

## Host-Only Contract Tests

`host/` is the only ordinary CMake project. It produces test executables for ABI headers and pure state models; it does not produce ESP32 firmware or compile ESP-IDF components.

```sh
cmake -S poc/host -B .build/poc-host -DENABLE_SANITIZERS=ON
cmake --build .build/poc-host
ctest --test-dir .build/poc-host --output-on-failure
```

PoC-A provides the first ESP-IDF test application. Later board applications and pytest-embedded drivers are added one gate at a time after their required hardware and loader decisions are accepted.
