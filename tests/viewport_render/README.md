# Isolated viewport graphics fixture

This opt-in executable uses a private wx/OpenGL context and production shader resources. It links core math, GLAD and wx libraries, never GUI_App or libslic3r_gui. It does not initialize devices, discovery, plugins, telemetry or project loading, and is not registered in CTest.

Build with the existing configured dependencies:

```bash
cmake -S . -B build/prototype -DPORCA_BUILD_VIEWPORT_RENDER_TESTS=ON
cmake --build build/prototype --config Release --target porca_viewport_render_tests -j6
```

On a machine permitted to run graphics validation:

```bash
build/prototype/tests/viewport_render/porca_viewport_render_tests /tmp/porca-viewport-fixture
```

The output contains Classic and Satin PNGs for a smooth sphere and sharp cube above a receiver, plus a GPU/viewport line. The executable compiles the physical-bed receiver too. It uses the production workspace shadow-fit math and offline-generated environment. It is a shader/FBO fixture, not proof of full native selection, mixed-bed placement, painting or project fidelity. Inspect those interactions separately on a permitted machine. The current local Mac restriction does not permit executing this graphics fixture.
