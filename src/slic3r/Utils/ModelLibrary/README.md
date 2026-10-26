# Native model library sources

This subsystem ports Strata Mesh Library behavior into Porca's C++ application.
The source reference is the local `strata-mesh-library` project: its catalog
migrations, hash/fingerprint conventions, tag rules, import protection, watcher,
source discovery, metadata and organization behavior inform this implementation.
`Schema.hpp` retains the six Strata migrations for on-disk compatibility. The
native UI is implemented in `GUI/ModelLibraryPanel.cpp` using Porca's widgets and
Plater integration. No Strata web UI, fonts, branding assets or Rust binaries are
bundled.

SQLite and BLAKE3 source provenance and licenses are retained in
`deps_src/library_storage`. Other libraries are inherited Porca dependencies.
See `docs/HLSD/model-library.md` for ownership, data safety and verification limits.
