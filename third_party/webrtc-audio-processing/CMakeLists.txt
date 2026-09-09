cmake_minimum_required(VERSION 3.14)
project(webrtc_apm LANGUAGES C CXX)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 依赖 Abseil (由父项目 add_subdirectory 提供，无需 find_package)
# find_package(absl REQUIRED)

# 定义宏
add_definitions(
    -DWEBRTC_WIN 
    -D_WIN32 
    -DWEBRTC_APM_DEBUG_DUMP=0 
    -DWEBRTC_POSIX=0
    -DNOMINMAX
    -D__STDC_FORMAT_MACROS
    -D__USE_MINGW_ANSI_STDIO=1
)

# 包含路径
include_directories(
    .
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/webrtc
)

# 收集源文件
file(GLOB_RECURSE WEBRTC_SOURCES 
    "webrtc/api/*.cc" "webrtc/api/*.c"
    "webrtc/audio/*.cc" "webrtc/audio/*.c"
    "webrtc/common_audio/*.cc" "webrtc/common_audio/*.c"
    "webrtc/modules/audio_processing/*.cc" "webrtc/modules/audio_processing/*.c"
    "webrtc/rtc_base/*.cc" "webrtc/rtc_base/*.c"
    "webrtc/system_wrappers/*.cc" "webrtc/system_wrappers/*.c"
    "webrtc/third_party/rnnoise/src/*.cc" "webrtc/third_party/rnnoise/src/*.c"
    "webrtc/third_party/pffft/src/*.cc" "webrtc/third_party/pffft/src/*.c"
    "webrtc/modules/audio_coding/codecs/isac/main/source/*.cc" "webrtc/modules/audio_coding/codecs/isac/main/source/*.c"
)

# 排除测试和 Mock 文件
list(FILTER WEBRTC_SOURCES EXCLUDE REGEX ".*_unittest\\.cc$")
list(FILTER WEBRTC_SOURCES EXCLUDE REGEX ".*_test\\.cc$")
list(FILTER WEBRTC_SOURCES EXCLUDE REGEX ".*test_utils\\.cc$")
list(FILTER WEBRTC_SOURCES EXCLUDE REGEX ".*mock.*\\.cc$")
list(FILTER WEBRTC_SOURCES EXCLUDE REGEX ".*_fuzzer\\.cc$")
list(FILTER WEBRTC_SOURCES EXCLUDE REGEX ".*_integration\\.cc$")
list(FILTER WEBRTC_SOURCES EXCLUDE REGEX ".*tools/.*")
list(FILTER WEBRTC_SOURCES EXCLUDE REGEX ".*android.*")
list(FILTER WEBRTC_SOURCES EXCLUDE REGEX ".*ios.*")
list(FILTER WEBRTC_SOURCES EXCLUDE REGEX ".*mac.*")

# 排除 NEON 文件 (除非在 ARM 上)
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|aarch64)")
    list(FILTER WEBRTC_SOURCES EXCLUDE REGEX ".*_neon\\.cc$")
    list(FILTER WEBRTC_SOURCES EXCLUDE REGEX ".*_neon\\.c$")
endif()

# 排除 MIPS 文件 (除非在 MIPS 上)
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(mips)")
    list(FILTER WEBRTC_SOURCES EXCLUDE REGEX ".*_mips\\.cc$")
    list(FILTER WEBRTC_SOURCES EXCLUDE REGEX ".*_mips\\.c$")
endif()

# 为所有 AVX2 文件设置编译标志
file(GLOB_RECURSE WEBRTC_AVX2_FILES 
    "${CMAKE_CURRENT_SOURCE_DIR}/webrtc/*_avx2.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/webrtc/*_avx2.c"
)
foreach(FILE ${WEBRTC_AVX2_FILES})
    set_source_files_properties("${FILE}" PROPERTIES COMPILE_FLAGS "-mavx2 -mfma")
endforeach()

# 创建库
add_library(webrtc_apm STATIC ${WEBRTC_SOURCES})

# 链接 Abseil (链接所有常用组件以防万一)
target_link_libraries(webrtc_apm PUBLIC 
    absl::base
    absl::strings
    absl::optional
    absl::synchronization
    absl::flat_hash_map
    absl::flags
)

# 暴露包含路径
target_include_directories(webrtc_apm PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/webrtc
)
