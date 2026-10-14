# Porca design-system assets

The user supplied `tokens.json`, `tokens.resolved.json`, `BRAND.md`, variable WOFF2 fonts and the icon paths in the Porca design-system handoff. These are reference data; repository behavior and compatibility constraints continue to apply.

`scripts/generate_design_tokens.py` generates `GUI/DesignTokens.hpp` from the source tokens. Native typography uses static TrueType instances of the supplied variable fonts, generated offline with `scripts/convert_design_fonts.py` (fontTools and a local Node Brotli decoder). The input font files and SIL OFL notices are retained. These are the supplied Latin subsets; existing CJK font choices and system fallback remain available.

`lucide-icons.json` retains the supplied paths from Lucide 0.460.0. The SVG files use its 24-unit grid and the handoff's 1.75-unit stroke. Copyright and permission notices are in `Lucide-LICENSE.txt`, sourced from https://raw.githubusercontent.com/lucide-icons/lucide/0.460.0/LICENSE. Native toolbar atlas generation adds padding, theme colors and interaction backgrounds without changing icon identity or tool actions. Specialized slicer glyphs retain their inherited sources and provenance.

The existing vector pig and wordmark remain the brand assets. The handoff's raster logo crops are not substituted for those vectors.
