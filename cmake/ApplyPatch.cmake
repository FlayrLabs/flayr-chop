# Applies each of PATCHES ("|"-separated) to the current directory, a JUCE source tree,
# exactly once. With CHECK_ONLY, only verifies that every patch is in.
#
# Whether a patch is in is decided by content, never by git's exit code: each patch's
# preamble names one or more "Flayr-Patch-Marker: <file> <text>" lines, and the patch
# counts as applied only if every marker text is in its file. (git apply --check
# --reverse "succeeds" on a tree with no .git that sits inside another work tree: it
# skips every file and exits 0.)
if(NOT PATCHES)
    message(FATAL_ERROR "ApplyPatch.cmake: PATCHES not set")
endif()
string(REPLACE "|" ";" PATCHES "${PATCHES}")
set(tree "${CMAKE_CURRENT_BINARY_DIR}") # cmake -P runs in the working directory
get_filename_component(tree "${tree}" ABSOLUTE)

# Sets <out> to ON if every marker of <patch> is present, OFF if none is; anything in
# between (a half-applied or edited tree) is an error.
function(flayr_patch_state patch out)
    file(STRINGS "${patch}" markers REGEX "^Flayr-Patch-Marker: ")
    if(NOT markers)
        message(FATAL_ERROR "${patch} has no Flayr-Patch-Marker lines")
    endif()
    set(found 0)
    set(total 0)
    foreach(line IN LISTS markers)
        string(REGEX REPLACE "^Flayr-Patch-Marker: ([^ ]+) (.+)$" "\\1" file "${line}")
        string(REGEX REPLACE "^Flayr-Patch-Marker: ([^ ]+) (.+)$" "\\2" text "${line}")
        math(EXPR total "${total} + 1")
        if(EXISTS "${tree}/${file}")
            file(READ "${tree}/${file}" content)
            string(FIND "${content}" "${text}" at)
            if(NOT at EQUAL -1)
                math(EXPR found "${found} + 1")
            endif()
        endif() # a missing file (one the patch adds) is a missing marker
    endforeach()
    if(found EQUAL total)
        set(${out} ON PARENT_SCOPE)
    elseif(found EQUAL 0)
        set(${out} OFF PARENT_SCOPE)
    else()
        message(FATAL_ERROR "${patch} is only partly applied in ${tree} (an older version of the "
                            "patch? Delete the build directory's _deps/juce-* to fetch JUCE afresh)")
    endif()
endfunction()

if(NOT EXISTS "${tree}/modules/juce_core/juce_core.h")
    message(FATAL_ERROR "${tree} is not a JUCE source tree")
endif()
find_package(Git REQUIRED)
get_filename_component(parent "${tree}" DIRECTORY)
foreach(patch IN LISTS PATCHES)
    flayr_patch_state("${patch}" applied)
    if(applied)
        message(STATUS "JUCE patch present: ${patch}")
    elseif(CHECK_ONLY)
        message(FATAL_ERROR "JUCE patch not applied: ${patch}")
    else()
        # The ceiling stops git from treating an enclosing work tree as the repository,
        # in which case it would skip every file and report success.
        execute_process(COMMAND ${CMAKE_COMMAND} -E env "GIT_CEILING_DIRECTORIES=${parent}"
                                "${GIT_EXECUTABLE}" apply -v --whitespace=nowarn "${patch}"
                        WORKING_DIRECTORY "${tree}"
                        RESULT_VARIABLE failed OUTPUT_VARIABLE out ERROR_VARIABLE out)
        if(failed OR out MATCHES "Skipped patch")
            message(FATAL_ERROR "Could not apply ${patch}:\n${out}")
        endif()
        flayr_patch_state("${patch}" applied)
        if(NOT applied)
            message(FATAL_ERROR "git apply reported success but ${patch} is not in ${tree}:\n${out}")
        endif()
        message(STATUS "Applied ${patch}")
    endif()
endforeach()
