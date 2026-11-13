cmake_minimum_required(VERSION 3.16)
project(coarena_client VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)

add_executable(coarena
    src/main.c
    src/core.c
    src/keys.c
    src/attachments.c
    src/api.c
    src/files.c
    src/render.c
    src/sessions.c
)

target_include_directories(coarena PRIVATE include)
target_link_libraries(coarena PRIVATE PkgConfig::LIBCURL PkgConfig::JSONC)
target_compile_options(coarena PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS coarena RUNTIME DESTINATION bin)
