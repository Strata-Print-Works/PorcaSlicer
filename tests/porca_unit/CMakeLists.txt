# Reuse tests owned by the core/GUI suites, compiling only their pure production
# sources. Keep the full suites' copies for integration coverage. Do not link
# libslic3r or libslic3r_gui here: either would bring back the expensive rebuild.
add_executable(porca_unit_tests
    ../libslic3r/test_selection_alignment.cpp
    ../libslic3r/test_discovery_policy.cpp
    ../slic3rutils/test_printer_sessions.cpp
    ../slic3rutils/test_plugin_agent_owner.cpp
    ../slic3rutils/test_printer_connection_status.cpp
    ../../src/slic3r/Utils/PrinterConnectionStatus.cpp
    ../libslic3r/test_filament_usage.cpp
    ../libslic3r/test_plate_slice_batch.cpp
    ../libslic3r/test_production_ledger.cpp
    ../slic3rutils/test_preview_layout.cpp
    ../slic3rutils/test_preview_estimate.cpp
    ../../src/libslic3r/FilamentUsage.cpp
    ../../src/libslic3r/PlateSliceBatch.cpp
    ../../src/libslic3r/ProductionLedger.cpp
)
target_include_directories(porca_unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(porca_unit_tests SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/deps_src)
target_link_libraries(porca_unit_tests PRIVATE test_common cereal::cereal Catch2::Catch2WithMain)
set_property(TARGET porca_unit_tests PROPERTY FOLDER "tests")

# The same cases remain registered in their original suites. Give this faster
# executable its own CTest names so both versions can be selected reliably.
orcaslicer_discover_tests(porca_unit_tests TEST_PREFIX "porca_unit/")
