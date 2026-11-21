# Run at every build (CMakeLists.txt, reg2_commit): reg2_commit.h with the source tree's git commit, "-dirty" when it
# has changes; without git (a package built in a container), FALLBACK (the GTA2_COMMIT cache variable, from
# $GTA2_COMMIT at configure time) or $GTA2_COMMIT, as its first 10 characters; else empty. Written only when it
# changes, so only build_id.cpp builds again.
execute_process(COMMAND git -C ${SRC} rev-parse --short=10 HEAD OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE failed ERROR_QUIET)
if(failed)
    set(commit "${FALLBACK}")
    if(NOT commit)
        set(commit "$ENV{GTA2_COMMIT}")
    endif()
    string(SUBSTRING "${commit}" 0 10 commit)
    set(failed TRUE)
endif()
if(commit AND NOT failed)
    execute_process(COMMAND git -C ${SRC} status --porcelain --untracked-files=no --ignore-submodules=all
        OUTPUT_VARIABLE changes ERROR_QUIET)
    if(changes)
        string(APPEND commit "-dirty")
    endif()
endif()
file(CONFIGURE OUTPUT ${OUT} CONTENT "#define GTA2_COMMIT \"${commit}\"\n")
