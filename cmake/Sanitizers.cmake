# Asclepius sanitizers — ASan + UBSan in Debug builds for our own targets.

function(asclepius_target_sanitizers target)
    if(MSVC)
        return()
    endif()

    set(_san_flags "")

    if(ASCLEPIUS_ENABLE_ASAN AND CMAKE_BUILD_TYPE STREQUAL "Debug")
        list(APPEND _san_flags -fsanitize=address -fno-omit-frame-pointer)
    endif()
    if(ASCLEPIUS_ENABLE_UBSAN AND CMAKE_BUILD_TYPE STREQUAL "Debug")
        list(APPEND _san_flags -fsanitize=undefined)
    endif()

    if(_san_flags)
        target_compile_options(${target} PRIVATE ${_san_flags})
        target_link_options(${target}    PRIVATE ${_san_flags})
    endif()
endfunction()
