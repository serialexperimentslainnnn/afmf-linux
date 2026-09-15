cmake_minimum_required(VERSION 3.28)
project(afmf-linux VERSION 0.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)

option(AFMF_WERROR "Treat warnings as errors" ON)
option(AFMF_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

include(GNUInstallDirs)

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

# ---- the layer: libafmf-linux.so + afmf-linux.json ----------------------------------------------

add_library(afmf-linux SHARED
    src/config.c
    src/layer.c
    src/log.c
    src/swapchain.c
)
target_include_directories(afmf-linux PRIVATE src)
target_link_libraries(afmf-linux PRIVATE Vulkan::Headers Threads::Threads)
target_compile_definitions(afmf-linux PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(afmf-linux PRIVATE ${AFMF_WARNINGS})
set_target_properties(afmf-linux PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/layer")

# Two manifests from one template: the build tree's points at the built .so by absolute path (usable
# straight away with VK_ADD_IMPLICIT_LAYER_PATH=<build>/layer), the installed one at the installed .so.
set(AFMF_LAYER_LIBRARY_PATH "$<TARGET_FILE:afmf-linux>")
configure_file(layer/afmf-linux.json.in "${CMAKE_BINARY_DIR}/layer/afmf-linux.json.genex" @ONLY)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/layer/afmf-linux.json"
     INPUT "${CMAKE_BINARY_DIR}/layer/afmf-linux.json.genex")

set(AFMF_LAYER_LIBRARY_PATH "${CMAKE_INSTALL_FULL_LIBDIR}/libafmf-linux.so")
configure_file(layer/afmf-linux.json.in "${CMAKE_BINARY_DIR}/install/afmf-linux.json" @ONLY)

install(TARGETS afmf-linux LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES "${CMAKE_BINARY_DIR}/install/afmf-linux.json"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/vulkan/implicit_layer.d")

# ---- tests -----------------------------------------------------------------------------------

enable_testing()

add_executable(afmf_headless tests/headless.c)
target_link_libraries(afmf_headless PRIVATE Vulkan::Vulkan)
target_compile_options(afmf_headless PRIVATE ${AFMF_WARNINGS})

# 120 presents: the first has no history, every other one must have got a generated companion.
add_test(NAME headless COMMAND afmf_headless)
set_tests_properties(headless PROPERTIES
    ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_BINARY_DIR}/layer;AFMF_LOG=3"
    PASS_REGULAR_EXPRESSION "120 presents: 119 generated"
    SKIP_RETURN_CODE 77
)

if(AFMF_SANITIZE)
    foreach(target afmf-linux afmf_headless)
        target_compile_options(${target} PRIVATE ${AFMF_SANITIZE_FLAGS})
        target_link_options(${target} PRIVATE ${AFMF_SANITIZE_FLAGS})
    endforeach()
endif()
