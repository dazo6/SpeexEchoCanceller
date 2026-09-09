cmake_minimum_required(VERSION 3.20)
project(SpeexEchoCanceller VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AEC_BUILD_BENCHMARK "Build the offline per-frame benchmark utility" OFF)
option(AEC_ENABLE_LOCAL_DEPLOY "Copy a successful local build to AEC_DEPLOY_DIR" ON)
set(AEC_DEPLOY_DIR "D:/实时回声消除" CACHE PATH "Local deployment directory")
set(REAL_AEC_CONFIG "Debug" CACHE STRING "Bundled RealAEC binary variant (Debug or Release)")
set_property(CACHE REAL_AEC_CONFIG PROPERTY STRINGS Debug Release)

# Prefer the project-local Qt installed by aqtinstall, but accept a regular
# CMAKE_PREFIX_PATH as used by CI and other developer machines.
set(AEC_LOCAL_QT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/qt6/6.8.3/mingw_64")
if(EXISTS "${AEC_LOCAL_QT}/lib/cmake/Qt6")
    list(PREPEND CMAKE_PREFIX_PATH "${AEC_LOCAL_QT}")
endif()
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets)

add_compile_definitions(_WIN32_WINNT=0x0601 WINVER=0x0601 FLOATING_POINT USE_SMALLFT)
if(MINGW)
    add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)
    add_compile_options(-mstackrealign -mavx)
endif()

include_directories(BEFORE third_party/compat third_party/mingw-std-threads)
include_directories(include libspeex libspeexdsp third_party/real-aec)

set(ABSL_BUILD_TESTING OFF CACHE BOOL "Disable Abseil testing" FORCE)
set(ABSL_USE_GOOGLETEST_HEAD OFF CACHE BOOL "Disable GoogleTest" FORCE)
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "Propagate C++ standard" FORCE)
add_subdirectory(third_party/abseil-cpp)
add_subdirectory(third_party/webrtc-audio-processing)

file(GLOB SPEEX_SOURCES CONFIGURE_DEPENDS "libspeex/*.c")
file(GLOB SPEEXDSP_SOURCES CONFIGURE_DEPENDS "libspeexdsp/*.c")
list(REMOVE_ITEM SPEEX_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/libspeex/kiss_fft.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/libspeex/kiss_fftr.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/libspeex/smallft.c"
)

set(REAL_AEC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/real-aec/MD-libs/x64/${REAL_AEC_CONFIG}")
set(REAL_AEC_LIBRARY "${REAL_AEC_DIR}/RealAEC.lib")
set(REAL_AEC_RUNTIME "${REAL_AEC_DIR}/RealAEC.dll")
if(NOT EXISTS "${REAL_AEC_LIBRARY}" OR NOT EXISTS "${REAL_AEC_RUNTIME}")
    message(FATAL_ERROR "RealAEC ${REAL_AEC_CONFIG} binaries were not found in ${REAL_AEC_DIR}")
endif()
message(STATUS "Using bundled RealAEC ${REAL_AEC_CONFIG}")

set(AEC_SYSTEM_LIBS ole32 user32 uuid shell32 comctl32 advapi32)

function(aec_configure_target target)
    target_link_libraries(${target} PRIVATE webrtc_apm "${REAL_AEC_LIBRARY}" ${AEC_SYSTEM_LIBS})
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${REAL_AEC_RUNTIME}" "$<TARGET_FILE_DIR:${target}>/RealAEC.dll"
        VERBATIM
    )
endfunction()

add_executable(SpeexEchoCanceller WIN32
    qt_gui_main.cpp AudioUtils.h ${SPEEX_SOURCES} ${SPEEXDSP_SOURCES}
)
target_link_libraries(SpeexEchoCanceller PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
aec_configure_target(SpeexEchoCanceller)

add_custom_command(TARGET SpeexEchoCanceller POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SpeexEchoCanceller>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/tray_icon_states.png"
            "$<TARGET_FILE_DIR:SpeexEchoCanceller>/assets/tray_icon_states.png"
    VERBATIM
)

if(AEC_ENABLE_LOCAL_DEPLOY AND WIN32)
    add_custom_command(TARGET SpeexEchoCanceller POST_BUILD
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/deploy_after_build.ps1"
                -SourceDir "$<TARGET_FILE_DIR:SpeexEchoCanceller>"
                -TargetDir "${AEC_DEPLOY_DIR}"
                -QtBinDir "$<TARGET_FILE_DIR:Qt6::Core>"
        VERBATIM
    )
endif()

if(AEC_BUILD_BENCHMARK)
    add_executable(aec_benchmark
        benchmark_main.cpp AudioUtils.h ${SPEEX_SOURCES} ${SPEEXDSP_SOURCES}
    )
    aec_configure_target(aec_benchmark)
endif()
