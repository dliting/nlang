# tinyxml2 10.1.0 (vendored, zlib license — see LICENSE.txt).
# Used by ncc for .nproj project files (Phase 10 Step 0). Kept as a static
# library so tools and tests share one build of it.
add_library(tinyxml2 STATIC
    tinyxml2.cpp
)

target_include_directories(tinyxml2
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)
