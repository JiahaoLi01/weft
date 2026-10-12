# Target helpers shared by src/, tests/ and probes/.
#
# Every src/<module> directory builds one static library weft_<module>, used through its alias weft::<module>.
# Module headers are included as "<module>/<file>.hpp" from the src/ root; the versioned C ABI lives in include/weft/.

function(weft_project_options target)
    target_include_directories(${target} PUBLIC "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src")
    target_compile_features(${target} PUBLIC cxx_std_20)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        # Aggregate initializers leave members to their default member initializers on purpose.
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers
            $<$<CXX_COMPILER_ID:Clang>:-Wno-missing-designated-field-initializers>)
    endif()
endfunction()

# weft_add_library(<module> SOURCES <file>... [PUBLIC <dependency>...] [PRIVATE <dependency>...])
function(weft_add_library module)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;PUBLIC;PRIVATE")
    set(target weft_${module})
    add_library(${target} STATIC ${arg_SOURCES})
    add_library(weft::${module} ALIAS ${target})
    weft_project_options(${target})
    if(arg_PUBLIC)
        target_link_libraries(${target} PUBLIC ${arg_PUBLIC})
    endif()
    if(arg_PRIVATE)
        target_link_libraries(${target} PRIVATE ${arg_PRIVATE})
    endif()
endfunction()

# weft_add_executable(<name> SOURCES <file>... [LINK <dependency>...] [SOURCE_DIR] [LARGE_STACK] [GC_STRESS])
#   SOURCE_DIR   defines WEFT_SOURCE_DIR so the program can read fixtures, fonts and original bundles.
#   LARGE_STACK  links an 8 MiB main-thread stack on MSVC for QuickJS recursion in the React/D3 bundles.
#   GC_STRESS    defines WEFT_REACT_GC_STRESS=1 when QuickJS is built with QJS_ENABLE_GC_STRESS.
function(weft_add_executable name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "SOURCE_DIR;LARGE_STACK;GC_STRESS" "" "SOURCES;LINK")
    add_executable(${name} ${arg_SOURCES})
    weft_project_options(${name})
    if(arg_LINK)
        target_link_libraries(${name} PRIVATE ${arg_LINK})
    endif()
    if(arg_SOURCE_DIR)
        target_compile_definitions(${name} PRIVATE WEFT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    endif()
    if(arg_LARGE_STACK AND MSVC)
        target_link_options(${name} PRIVATE /STACK:8388608)
    endif()
    if(arg_GC_STRESS AND QJS_ENABLE_GC_STRESS)
        target_compile_definitions(${name} PRIVATE WEFT_REACT_GC_STRESS=1)
    endif()
endfunction()

# weft_add_test(NAME <name> COMMAND <command>... [TIMEOUT <seconds>] [GC_STRESS_TIMEOUT <seconds>]
#               [WORKING_DIRECTORY <dir>] [FIXTURES_SETUP <fixture>] [FIXTURES_REQUIRED <fixture>] [DEPENDS <test>])
# Tests run from the build root unless WORKING_DIRECTORY is given. GC_STRESS_TIMEOUT replaces TIMEOUT when QuickJS
# is built with QJS_ENABLE_GC_STRESS.
function(weft_add_test)
    cmake_parse_arguments(PARSE_ARGV 0 arg ""
        "NAME;TIMEOUT;GC_STRESS_TIMEOUT;WORKING_DIRECTORY;FIXTURES_SETUP;FIXTURES_REQUIRED;DEPENDS" "COMMAND")
    if(NOT arg_WORKING_DIRECTORY)
        set(arg_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
    endif()
    add_test(NAME ${arg_NAME} COMMAND ${arg_COMMAND} WORKING_DIRECTORY "${arg_WORKING_DIRECTORY}")
    if(QJS_ENABLE_GC_STRESS AND DEFINED arg_GC_STRESS_TIMEOUT)
        set(arg_TIMEOUT ${arg_GC_STRESS_TIMEOUT})
    endif()
    foreach(property TIMEOUT FIXTURES_SETUP FIXTURES_REQUIRED DEPENDS)
        if(DEFINED arg_${property})
            set_tests_properties(${arg_NAME} PROPERTIES ${property} "${arg_${property}}")
        endif()
    endforeach()
endfunction()

# weft_add_python_test(NAME <name> SCRIPT <tools/script.py> [ARGS <argument>...] [FIXTURES_REQUIRED <fixture>]
#                      [DEPENDS <test>])
# Registers nothing when no Python 3 interpreter was found.
function(weft_add_python_test)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "NAME;SCRIPT;FIXTURES_REQUIRED;DEPENDS" "ARGS")
    if(NOT Python3_Interpreter_FOUND)
        return()
    endif()
    set(properties)
    foreach(property FIXTURES_REQUIRED DEPENDS)
        if(DEFINED arg_${property})
            list(APPEND properties ${property} "${arg_${property}}")
        endif()
    endforeach()
    weft_add_test(NAME ${arg_NAME} COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/${arg_SCRIPT}" ${arg_ARGS}
        ${properties})
endfunction()
