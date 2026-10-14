# No GUI_App or libslic3r_gui: this executable cannot start printer services.
add_executable(porca_viewport_render_tests viewport_render.cpp
    ${CMAKE_SOURCE_DIR}/src/slic3r/GUI/ViewportLighting.cpp)
target_include_directories(porca_viewport_render_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${wxWidgets_INCLUDE_DIRS})
target_link_libraries(porca_viewport_render_tests PRIVATE libslic3r glad ${wxWidgets_LIBRARIES} ${_opengl_link_lib})
target_compile_definitions(porca_viewport_render_tests PRIVATE wxDEBUG_LEVEL=0
    PORCA_RENDER_RESOURCES="${CMAKE_SOURCE_DIR}/resources")
set_target_properties(porca_viewport_render_tests PROPERTIES MACOSX_BUNDLE FALSE)

if (WIN32)
    set_target_properties(porca_viewport_render_tests PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

if (TARGET OpenGL::EGL)
    target_link_libraries(porca_viewport_render_tests PRIVATE OpenGL::EGL)
endif()
