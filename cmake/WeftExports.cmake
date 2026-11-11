# The C functions the public headers declare, the exports of the shared library. Taken from the headers so that the
# release and dev builds export the same list whatever either compiled.
#
# weft_collect_exports(<out_var> <header>...)
function(weft_collect_exports out)
    set(names)
    foreach(header ${ARGN})
        # A declaration at the start of a line: `weft_result weft_view_load(`. Function pointer typedefs, struct members
        # and comments do not begin that way.
        file(STRINGS "${PROJECT_SOURCE_DIR}/include/weft/${header}" lines
            REGEX "^[A-Za-z_][A-Za-z0-9_ *]*[ *]weft_[a-z0-9_]+\\(")
        foreach(line IN LISTS lines)
            string(REGEX MATCH "weft_[a-z0-9_]+\\(" name "${line}")
            string(REPLACE "(" "" name "${name}")
            list(APPEND names "${name}")
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES names)
    list(SORT names)
    set(${out} "${names}" PARENT_SCOPE)
endfunction()
