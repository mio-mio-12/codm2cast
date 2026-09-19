# Third-party dependencies

All runtime dependencies used by the build are vendored. License texts are in `licenses/`; original notices also remain in the source files.

| Dependency | Version / revision | License | Purpose |
|---|---|---|---|
| Dear ImGui | 1.92.5 | MIT | Native three-column interface |
| GLFW | 3.4.0 | zlib/libpng | Windows and OpenGL context |
| GLM | 1.0.1 | MIT / Happy Bunny | Geometry and transform math |
| nlohmann JSON | 3.12.0 | MIT | Data and reports |
| LZ4 | 1.10.0 | BSD 2-Clause | Bundle decompression |
| SQLite | 3.50.4 | Public domain | Persistent library and material index |
| texture2ddecoder | `3ebc3b758bd6b1a3108b50148f7998ee34f058c6` | MIT; retained upstream file notices | Native texture decoding |
| stb | `2c980bb59875b0d32144a71867fbdebb2f77cd20` | MIT / public domain | PNG and image utilities |
| UnityPy | 1.25.3, development reference only | MIT | Schema/string-table reference and independent comparisons |

Upstream acquisition information is retained in data/dependencies.json and data/texture-dependencies.json. Original license notices remain in vendor/ and licenses/.