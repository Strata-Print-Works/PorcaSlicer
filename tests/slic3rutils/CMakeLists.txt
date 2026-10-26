# Directory-only target paths in worker copy commands must not create a reverse
# dependency on the executable that consumes the copy.
if (POLICY CMP0112)
    cmake_policy(SET CMP0112 NEW)
endif ()

get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests
    test_model_library.cpp
    ${_TEST_NAME}_tests_main.cpp
    test_bambu_filament_ids.cpp
    test_bambu_device_sessions.cpp
    test_bambu_process_sessions.cpp
    test_plugin_agent_owner.cpp
    test_printer_sessions.cpp
    test_network_device_sessions.cpp
    test_printer_connection_status.cpp
    test_config_value_formatter.cpp
    test_creality_cfs_match.cpp
    test_dev_mapping.cpp
    test_filament_bitmap_utils.cpp
    test_device_progress.cpp
    test_job_progress.cpp
    test_device_manager_integration.cpp
    test_web_media_controller.cpp
    test_scene_raycaster.cpp
    test_workspace_scene.cpp
    test_imgui_input.cpp
    test_camera.cpp
    test_undo_redo.cpp
    test_preview_layout.cpp
    test_preview_estimate.cpp
    test_scene_benchmark.cpp
    test_viewport_lighting.cpp
    test_workspace_style.cpp
    test_lazy.cpp
    test_prebuild_queue.cpp
    test_staged_build.cpp
    test_network_versions.cpp
    test_orca_cloud_agent.cpp
    test_action_source.cpp
    test_plugin_workspace.cpp
    test_plugin_host_api.cpp
    # Exercise seam enums and predicates through the embedded Python host API.
    test_precise_seam_plugin.cpp
    test_plugin_capability_config.cpp
    test_plugin_config.cpp
    test_plugin_capabilities_in_use.cpp
    test_plugin_status.cpp
    test_printer_agent.cpp
    test_moonraker_printer_agent.cpp
    test_plugin_install.cpp
    test_plugin_lifecycle.cpp
    test_python_shutdown.cpp
    test_branding.cpp
    test_live_toolbar_style.cpp
    test_plugin_printer_agent.cpp
    test_printhost.cpp
    test_elegoo_link.cpp
    test_slicing_pipeline_bindings.cpp
    test_slicing_pipeline_config.cpp
    test_plugin_sort.cpp
    test_plugin_cloud_metadata.cpp
    test_plugin_audit.cpp
    test_plugin_json_depth.cpp
    test_shortcuts.cpp
    test_settings_scope.cpp
    test_texture_color_palette.cpp
    test_texture_island_view.cpp
    test_file_url.cpp
    test_user_manager.cpp
    ../fff_print/test_helpers.cpp
    )

if (SLIC3R_CAD)
    target_sources(${_TEST_NAME}_tests PRIVATE test_design_sketch_tool.cpp)
endif ()

if (MSVC)
    target_link_libraries(${_TEST_NAME}_tests Setupapi.lib)
endif ()

target_link_libraries(${_TEST_NAME}_tests porca_model_library test_common libslic3r_gui libslic3r pybind11::embed Catch2::Catch2WithMain)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

add_library(porca_fake_bbl_plugin SHARED fake_bbl_plugin.cpp)
target_include_directories(porca_fake_bbl_plugin PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/deps_src)
target_link_libraries(porca_fake_bbl_plugin PRIVATE boost_headeronly libslic3r)
add_library(porca_fake_bbl_missing SHARED fake_bbl_missing.cpp)
add_library(porca_fake_bbl_badabi SHARED fake_bbl_missing.cpp)
target_compile_definitions(porca_fake_bbl_badabi PRIVATE PORCA_FAKE_VERSION="02.08.01.00")
add_dependencies(${_TEST_NAME}_tests porca_fake_bbl_plugin porca_fake_bbl_missing porca_fake_bbl_badabi porca_bambu_worker)
# CMP0112 is set NEW at the top of this file when available.
if (POLICY CMP0112)
    add_custom_target(porca_bambu_worker_test_copy
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:porca_bambu_worker>" "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/$<TARGET_FILE_NAME:porca_bambu_worker>"
        DEPENDS porca_bambu_worker VERBATIM)
    add_dependencies(${_TEST_NAME}_tests porca_bambu_worker_test_copy)
else ()
    set_property(TARGET ${_TEST_NAME}_tests APPEND PROPERTY LINK_DEPENDS "$<TARGET_FILE:porca_bambu_worker>")
    add_custom_command(TARGET ${_TEST_NAME}_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:porca_bambu_worker>" "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/$<TARGET_FILE_NAME:porca_bambu_worker>"
        VERBATIM)
endif ()
target_compile_definitions(${_TEST_NAME}_tests PRIVATE PORCA_BAMBU_WORKER="$<TARGET_FILE_NAME:porca_bambu_worker>")
target_compile_definitions(${_TEST_NAME}_tests PRIVATE PORCA_FAKE_BBL_PLUGIN="$<TARGET_FILE_NAME:porca_fake_bbl_plugin>")

orcaslicer_copy_test_dlls(${_TEST_NAME}_tests)

if (WIN32)
    add_custom_command(TARGET ${_TEST_NAME}_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_PREFIX_PATH}/libpython" "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_PREFIX_PATH}/libpython/python${_bundled_python_abi}.dll"
            "${CMAKE_PREFIX_PATH}/libpython/vcruntime140.dll"
            "${CMAKE_PREFIX_PATH}/libpython/vcruntime140_1.dll"
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>"
        COMMENT "Copying Python runtime for slic3rutils plugin host API tests"
        VERBATIM
    )
elseif (NOT FLATPAK)
    # The CI unit-test runner only receives the build/tests tree, so both the
    # interpreter and the libpython the test binary links have to travel next to
    # the executable; find_bundled_python_home() picks the copy up from there.
    if (APPLE)
        target_link_options(${_TEST_NAME}_tests PRIVATE
            "LINKER:-rpath,@executable_path/python/lib")
    else ()
        set_property(TARGET ${_TEST_NAME}_tests APPEND PROPERTY BUILD_RPATH "$ORIGIN/python/lib")
    endif ()

    add_custom_command(TARGET ${_TEST_NAME}_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_PREFIX_PATH}/libpython"
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMENT "Copying Python runtime for the plugin host API tests"
        VERBATIM
    )
elseif (FLATPAK)
    # Same <testdir>/python home as WIN32/APPLE; symlink since /app/libpython
    # already ships in the flatpak (the test exe links libpython3.12.so from it).
    add_custom_command(TARGET ${_TEST_NAME}_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            "${CMAKE_PREFIX_PATH}/libpython"
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMENT "Linking Python runtime for flatpak plugin host API tests"
        VERBATIM
    )
endif()

# scripts/run_unit_tests.sh installs the tests' numpy into that runtime with this uv,
# staged where the app build tree keeps it (<exe dir>/tools/uv, see src/CMakeLists.txt).
if (ORCA_BUNDLED_UV_EXECUTABLE)
    add_custom_command(TARGET ${_TEST_NAME}_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/tools/uv"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ORCA_BUNDLED_UV_EXECUTABLE}"
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/tools/uv/${ORCA_BUNDLED_UV_FILENAME}"
        COMMENT "Copying uv for the plugin host API tests"
        VERBATIM
    )
endif()

orcaslicer_discover_tests(${_TEST_NAME}_tests)

target_compile_definitions(${_TEST_NAME}_tests PRIVATE PORCA_LIBRARY_FORMAT_TESTS)
