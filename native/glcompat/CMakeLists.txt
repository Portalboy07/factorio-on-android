cmake_minimum_required(VERSION 3.22)
project(glcompat C)

# Host-side GL 3.3 desktop -> GLES 3.2 compatibility layer for the Factorio
# Android port.  Built with the NDK for arm64-v8a / API 26.  The exported
# entry point is fsgl_lookup(); SDL_GL_GetProcAddress calls it before falling
# back to eglGetProcAddress.  It never calls itself and never returns a
# pointer to one of its own wrapper lookups.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(SDL_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/SDL-release-3.4.14/include"
    CACHE PATH "SDL3 include directory")

# OFF: release layer (no per-call profiler/glGetError/call ring, bookkeeping-
# only entry points go straight to the driver).  ON: full instrumentation,
# glcompat-perf dump every 10 s and stall probes on glClientWaitSync,
# glFinish, glFlush, glWaitSync, glReadPixels and glBufferSubData.
option(GLCOMPAT_DIAG "Build GLCompat with profiling and GL error attribution" OFF)

add_library(GLCompat SHARED
    src/glcompat.c
    src/error.c
    src/profile.c
    src/translate.c
    src/etc2.c
    src/wrap_gl.c
    src/stream.c
    src/wrap_tex.c
    src/frametrace.c)

target_compile_definitions(GLCompat PRIVATE _GNU_SOURCE
    FSGL_DIAG=$<IF:$<BOOL:${GLCOMPAT_DIAG}>,1,0>)
target_include_directories(GLCompat PRIVATE "${SDL_INCLUDE_DIR}")
target_compile_options(GLCompat PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type
    -fvisibility=hidden -ffunction-sections -fdata-sections)
target_link_libraries(GLCompat PRIVATE log dl m)
target_link_options(GLCompat PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/glcompat.map"
    -Wl,--gc-sections)

# The wrappers call small state helpers across translation units
# (fsgl_buf_bound, fsgl_stream_*, ...); LTO lets them inline.
include(CheckIPOSupported)
check_ipo_supported(RESULT GLCOMPAT_IPO OUTPUT GLCOMPAT_IPO_MSG LANGUAGES C)
if(GLCOMPAT_IPO)
    set_property(TARGET GLCompat PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
else()
    message(STATUS "GLCompat: LTO unavailable: ${GLCOMPAT_IPO_MSG}")
endif()
set_target_properties(GLCompat PROPERTIES
    OUTPUT_NAME GLCompat
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    C_VISIBILITY_PRESET hidden)
