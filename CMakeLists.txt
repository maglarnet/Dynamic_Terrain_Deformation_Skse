cmake_minimum_required(VERSION 3.21)

project(NMN_DeformableTerrain VERSION 0.1.0 LANGUAGES CXX)

set(ENABLE_SKYRIM_SE ON  CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON  CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS      OFF CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_XBYAK ON CACHE BOOL "" FORCE)

add_subdirectory(extern/CommonLibSSE-NG CommonLibSSE EXCLUDE_FROM_ALL)

add_library(${PROJECT_NAME} SHARED)

target_sources(${PROJECT_NAME} PRIVATE
    src/plugin.cpp
    src/ActorPaint.cpp
    src/BloodDecals.cpp
    src/ActorShapes.cpp
    src/Clipmap.cpp
    src/Globals.cpp
    src/HeatSources.cpp
    src/MagicImpacts.cpp
    src/Hooks.cpp
    src/MeshGeometry.cpp
    src/ObjectStamps.cpp
    src/Profiler.cpp
    src/ShaderReflection.cpp
    src/ShaderRegistry.cpp
    src/Shelter.cpp
    src/SnowCoverage.cpp
    src/SnowSparkle.cpp
    src/SnowSurface.cpp
    src/SurfaceProfiles.cpp
    src/StampShapes.cpp
    src/Settings.cpp
    src/SurfaceTypes.cpp
    src/Tessellation.cpp
    src/Weather.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE include/PCH.h)

find_package(directxtk CONFIG REQUIRED)

target_link_libraries(${PROJECT_NAME} PRIVATE
    CommonLibSSE
    Microsoft::DirectXTK
    d3d11.lib
    bcrypt.lib
    d3dcompiler.lib
    dxgi.lib
    dxguid.lib
)

option(BUILD_OFFLINE_TOOLS "Build the offline shader generator" ON)

if(BUILD_OFFLINE_TOOLS)
    # `/utf-8` on both offline tools, matching what the plugin target already
    # gets.
    #
    # The plugin is built through the vcpkg toolchain because it links vcpkg
    # libraries, and that toolchain passes `/utf-8`; these two link only system
    # libraries (`d3d11`, `d3dcompiler`, `bcrypt`), never touch vcpkg, and so
    # never got the flag.  The room that difference leaves is invisible until a
    # source file - or a header one of these includes - carries a character
    # outside code page 936, and then the same comment is silent in the plugin
    # and a C4819 warning here.  That is what happened: the two builds disagreed
    # about one file, which reads like a bad character rather than a missing
    # flag.
    #
    # Written unconditionally rather than behind a compiler-id generator,
    # because the whole project is MSVC on Windows (the link lines above are
    # bare `.lib` names).
    add_executable(ShaderGen tools/offline/ShaderGen.cpp)
    target_compile_options(ShaderGen PRIVATE /utf-8)

    target_include_directories(ShaderGen PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/offline
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_compile_features(ShaderGen PRIVATE cxx_std_23)
    target_link_libraries(ShaderGen PRIVATE d3dcompiler.lib dxguid.lib bcrypt.lib)

    add_executable(StampSurfaceTest tools/offline/StampSurfaceTest.cpp)
    target_compile_options(StampSurfaceTest PRIVATE /utf-8)
    target_include_directories(StampSurfaceTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(StampSurfaceTest PRIVATE cxx_std_23)
    target_link_libraries(StampSurfaceTest PRIVATE d3d11.lib d3dcompiler.lib bcrypt.lib)
endif()

if(DEFINED ENV{SKYRIM_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_FOLDER}/Data")
    set(OUTPUT_FOLDER "$ENV{SKYRIM_FOLDER}/Data")
endif()

if(DEFINED OUTPUT_FOLDER)
    set(DLL_FOLDER "${OUTPUT_FOLDER}/SKSE/Plugins")
    message(STATUS "SKSE plugin output folder: ${DLL_FOLDER}")

    add_custom_command(
        TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DLL_FOLDER}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:${PROJECT_NAME}>"
                "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        VERBATIM
    )

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_custom_command(
            TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_PDB_FILE:${PROJECT_NAME}>"
                    "${DLL_FOLDER}/$<TARGET_PDB_FILE_NAME:${PROJECT_NAME}>"
            VERBATIM
        )
    endif()
endif()
