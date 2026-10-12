# Refuses an effect source that declares resources (cmake/WeftEffects.cmake): an effect reads its EffectInput and the
# textures and sampler the shared shader declares (effectTexture0-3, effectRepeat), nothing else. Comments may name
# anything. Its globals are static: HLSL makes a global without static a uniform value of a constant buffer of its
# own. An effect is one file of top-level functions, types and static globals: no includes, no namespaces. A guard
# against mistakes, not against a source that hides declarations in macros. Writes STAMP when the source passes.
#
# cmake -DSOURCE=<effect.hlsl> -DSTAMP=<file> -P check_effect.cmake
if(NOT SOURCE OR NOT STAMP)
    message(FATAL_ERROR "check_effect.cmake requires -DSOURCE=<file> and -DSTAMP=<file>")
endif()
file(READ "${SOURCE}" text)
string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" " " text "${text}")
string(REGEX REPLACE "//[^\n]*" " " text "${text}")
set(refusal "${SOURCE}: an effect declares no resources")
set(resource_types "cbuffer|tbuffer|ConstantBuffer|RWTexture[A-Za-z0-9]*|Texture[A-Za-z0-9]*|RWBuffer|Buffer")
string(APPEND resource_types "|RWByteAddressBuffer|ByteAddressBuffer|RWStructuredBuffer|StructuredBuffer")
string(APPEND resource_types "|AppendStructuredBuffer|ConsumeStructuredBuffer|SamplerComparisonState|SamplerState")
string(APPEND resource_types "|sampler[A-Za-z0-9]*|groupshared")
if(text MATCHES "register[ \t\r\n]*\\(" OR text MATCHES "(^|[^A-Za-z0-9_])(${resource_types})([^A-Za-z0-9_]|$)")
    message(FATAL_ERROR "${refusal}; it samples effectTexture0-3 with effectRepeat")
endif()
if(text MATCHES "#[ \t]*include" OR text MATCHES "(^|[^A-Za-z0-9_])namespace([^A-Za-z0-9_]|$)")
    message(FATAL_ERROR "${refusal}: it is one file at the top level, without includes or namespaces")
endif()

# The top level: preprocessor lines out with their continuations, then the bodies of functions and types, innermost
# first, each left as the end of a statement. A statement there is a function, whose first parenthesis comes before any
# =, a type, or a static global.
string(REGEX REPLACE "\\\\\r?\n" " " top "${text}")
string(REGEX REPLACE "#[^\n]*" " " top "${top}")
while(top MATCHES "{")
    string(REGEX REPLACE "{[^{}]*}" ";" inner "${top}")
    if(inner STREQUAL top)
        message(FATAL_ERROR "${SOURCE}: unbalanced braces")
    endif()
    set(top "${inner}")
endwhile()
string(REGEX REPLACE "[][]" " " top "${top}")
foreach(statement IN LISTS top)
    string(STRIP "${statement}" statement)
    if(statement STREQUAL "" OR statement MATCHES "^(struct|class|interface|typedef)([^A-Za-z0-9_]|$)" OR
       statement MATCHES "(^|[^A-Za-z0-9_])static([^A-Za-z0-9_]|$)")
        continue()
    endif()
    string(FIND "${statement}" "(" parenthesis)
    string(FIND "${statement}" "=" assignment)
    if(parenthesis GREATER -1 AND (assignment EQUAL -1 OR parenthesis LESS assignment))
        continue()
    endif()
    message(FATAL_ERROR "${refusal}: '${statement}' is a global without static, a uniform value")
endforeach()
file(WRITE "${STAMP}" "")
