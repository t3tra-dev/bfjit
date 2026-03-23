cmake_minimum_required(VERSION 3.16)

project(bfjit LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_program(LLVM_CONFIG_EXECUTABLE NAMES llvm-config-22 llvm-config REQUIRED)

function(bfjit_query_llvm_config output_variable option)
    execute_process(
        COMMAND "${LLVM_CONFIG_EXECUTABLE}" "${option}"
        OUTPUT_VARIABLE query_output
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE query_status
    )

    if(NOT query_status EQUAL 0)
        message(FATAL_ERROR "Failed to query LLVM using ${LLVM_CONFIG_EXECUTABLE} ${option}")
    endif()

    set(${output_variable} "${query_output}" PARENT_SCOPE)
endfunction()

bfjit_query_llvm_config(LLVM_VERSION --version)

if(NOT LLVM_VERSION MATCHES "^22\\.")
    message(FATAL_ERROR "LLVM 22 is required, but ${LLVM_VERSION} was found")
endif()

bfjit_query_llvm_config(LLVM_INCLUDE_DIR --includedir)
bfjit_query_llvm_config(LLVM_SHARED_MODE --shared-mode)
bfjit_query_llvm_config(LLVM_LIBFILES_RAW --libfiles)

if(NOT LLVM_SHARED_MODE STREQUAL "shared")
    message(FATAL_ERROR "LLVM must provide shared libraries, but llvm-config --shared-mode returned ${LLVM_SHARED_MODE}")
endif()

string(REPLACE "\n" ";" LLVM_SHARED_LIBRARIES "${LLVM_LIBFILES_RAW}")
list(FILTER LLVM_SHARED_LIBRARIES EXCLUDE REGEX "^$")

if(NOT LLVM_SHARED_LIBRARIES)
    message(FATAL_ERROR "No LLVM shared libraries were reported by llvm-config --libfiles")
endif()

add_library(llvm-shared INTERFACE)
target_include_directories(llvm-shared INTERFACE "${LLVM_INCLUDE_DIR}")
target_link_libraries(llvm-shared INTERFACE ${LLVM_SHARED_LIBRARIES})

add_library(libbfjit STATIC
	include/bfjit.h
    include/bflexer.h
    include/bfopt.h
    include/bfparser.h
	src/bfjit.c
    src/bflexer.c
    src/bfopt.c
    src/bfparser.c
)

target_include_directories(libbfjit PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(libbfjit PUBLIC llvm-shared)
set_target_properties(libbfjit PROPERTIES OUTPUT_NAME bfjit)

add_executable(bfjit src/main.c)
target_link_libraries(bfjit PRIVATE libbfjit)
