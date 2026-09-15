cmake_minimum_required(VERSION 3.28)
project(AFMF_Linux VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)

option(AFMF_WERROR "Treat warnings as errors" ON)
option(AFMF_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

# The layer only needs the headers: a Vulkan layer must never link the loader itself.
# The test host is an ordinary application and does link it.
find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)

set(AFMF_WARNINGS
    -Wall -Wextra
    -Wshadow -Wconversion -Wsign-conversion -Wdouble-promotion
    -Wformat=2 -Werror=format-security
    -Wcast-qual -Wcast-align -Wpointer-arith -Wwrite-strings
    -Wstrict-prototypes -Wold-style-definition -Wmissing-prototypes -Wmissing-declarations
    -Wvla -Walloca -Wstack-protector
    -Wnull-dereference -Wimplicit-fallthrough
    -Werror=implicit-function-declaration -Werror=incompatible-pointer-types -Werror=int-conversion
    $<$<BOOL:${AFMF_WERROR}>:-Werror>
)
set(AFMF_SANITIZE_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)

# ---- the layer -------------------------------------------------------------------------------

add_library(VkLayer_AFMF SHARED
    src/config.c
    src/layer.c
    src/log.c
    src/swapchain.c
)
target_include_directories(VkLayer_AFMF PRIVATE src)
target_link_libraries(VkLayer_AFMF PRIVATE Vulkan::Headers Threads::Threads)
target_compile_definitions(VkLayer_AFMF PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(VkLayer_AFMF PRIVATE ${AFMF_WARNINGS})
set_target_properties(VkLayer_AFMF PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/layer")

# The manifest points at the built .so by absolute path so the build tree is usable directly
# with VK_ADD_LAYER_PATH=<build>/layer; no install step is needed to test it.
set(AFMF_LAYER_LIBRARY_PATH "$<TARGET_FILE:VkLayer_AFMF>")
configure_file(layer/VkLayer_AFMF.json.in "${CMAKE_BINARY_DIR}/layer/VkLayer_AFMF.json.genex" @ONLY)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/layer/VkLayer_AFMF.json"
     INPUT "${CMAKE_BINARY_DIR}/layer/VkLayer_AFMF.json.genex")

# ---- tests -----------------------------------------------------------------------------------

enable_testing()

add_executable(afmf_headless tests/headless.c)
target_link_libraries(afmf_headless PRIVATE Vulkan::Vulkan)
target_compile_options(afmf_headless PRIVATE ${AFMF_WARNINGS})

add_test(NAME headless COMMAND afmf_headless)
set_tests_properties(headless PROPERTIES
    ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_BINARY_DIR}/layer;AFMF_LOG=3"
    SKIP_RETURN_CODE 77
)

if(AFMF_SANITIZE)
    foreach(target VkLayer_AFMF afmf_headless)
        target_compile_options(${target} PRIVATE ${AFMF_SANITIZE_FLAGS})
        target_link_options(${target} PRIVATE ${AFMF_SANITIZE_FLAGS})
    endforeach()
endif()
