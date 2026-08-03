cmake_minimum_required(VERSION 3.20)
project(WoWTranslate VERSION 0.1.0)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set output directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Create the WoWTranslate DLL
add_library(WoWTranslate SHARED
    src/dllmain.cpp
    src/lua_interface.cpp
    src/translator_core.cpp
    src/logging.cpp
    src/utils.cpp
    src/WoWTranslate.def
)

# Include directories
target_include_directories(WoWTranslate PRIVATE
    include
    third_party
)

# MinHook library setup - Force 32-bit for WoW compatibility
set(MINHOOK_LIB "${CMAKE_CURRENT_SOURCE_DIR}/third_party/MinHook.x86.lib")

# Link libraries - adding winhttp for translation functionality, advapi32 for MD5
target_link_libraries(WoWTranslate PRIVATE
    kernel32
    user32
    shell32
    winhttp
    advapi32
)

# Add MinHook if available
if(EXISTS ${MINHOOK_LIB})
    target_link_libraries(WoWTranslate PRIVATE ${MINHOOK_LIB})
    message(STATUS "Using MinHook library: ${MINHOOK_LIB}")
    add_compile_definitions(MINHOOK_AVAILABLE)
else()
    message(WARNING "MinHook library not found: ${MINHOOK_LIB}")
    message(WARNING "Download from: https://github.com/TsudaKageyu/minhook/releases")
endif()

# Compiler-specific settings
if(MSVC)
    # Set static runtime library for release builds
    set_property(TARGET WoWTranslate PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    # Additional MSVC settings
    target_compile_options(WoWTranslate PRIVATE
        /W4
        /permissive-
    )

    # Enable larger object files for complex translation logic
    target_compile_options(WoWTranslate PRIVATE /bigobj)
endif()

# Debug configurations
set_target_properties(WoWTranslate PROPERTIES
    DEBUG_POSTFIX "_d"
)

# Standalone injector. The 3.3.5 client has no dlls.txt loader like the patched
# 1.12 clients do, so the DLL has to be pushed in from outside.
add_executable(WoWTranslateInjector src/injector.cpp)

target_link_libraries(WoWTranslateInjector PRIVATE
    kernel32
    user32
)

if(MSVC)
    set_property(TARGET WoWTranslateInjector PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    # /utf-8: the injector prints Chinese diagnostics
    target_compile_options(WoWTranslateInjector PRIVATE
        /W4
        /permissive-
        /utf-8
    )
endif()

# Install rules
install(TARGETS WoWTranslate WoWTranslateInjector
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
)
