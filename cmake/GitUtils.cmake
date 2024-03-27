find_package(Git QUIET)

function(git_refresh_submodules)
    if (GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
        option(GIT_SUBMODULE "Check submodules during build" ON)
        if (GIT_SUBMODULE)
            message(STATUS "Submodule update")
            execute_process(COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                    RESULT_VARIABLE GIT_SUBMOD_RESULT)
            if (NOT GIT_SUBMOD_RESULT EQUAL "0")
                message(FATAL_ERROR "git submodule update --init --recursive failed with ${GIT_SUBMOD_RESULT}, please checkout submodules")
            endif()
        endif()
    endif()
endfunction()

function(git_extract_version target_var)
    if (GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
        execute_process(
                COMMAND ${GIT_EXECUTABLE} rev-list HEAD --count
                OUTPUT_VARIABLE GIT_REVISION
                OUTPUT_STRIP_TRAILING_WHITESPACE
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        )

        execute_process(
                COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
                OUTPUT_VARIABLE GIT_COMMIT_HASH
                OUTPUT_STRIP_TRAILING_WHITESPACE
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        )

        execute_process(
                COMMAND ${GIT_EXECUTABLE} describe --tags
                OUTPUT_VARIABLE GIT_DESCRIBE
                RESULT_VARIABLE GIT_DESCRIBE_RESULT
                OUTPUT_STRIP_TRAILING_WHITESPACE
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        )

        add_library(${target_var} INTERFACE)
        target_compile_definitions(${target_var} INTERFACE
                REVISION="${GIT_REVISION}"
                VERSION="${GIT_REVISION}~${GIT_COMMIT_HASH}"
        )

        # TODO: remove me
        message("Git describe: ${GIT_DESCRIBE} - ${GIT_DESCRIBE_RESULT}")

        if(NOT (GIT_DESC_RESULT EQUAL "0"))
            set(GIT_DESCRIBE "0.0.0-0-g00000000")
        endif()

        string(REGEX REPLACE "^([0-9]+)\\.([0-9]+)\\.([0-9]+)-([0-9]+)-g[0-9a-fA-F]+$" "\\1;\\2;\\3;\\4" PARTS "${GIT_DESCRIBE}")
        list(GET PARTS 0 VERSION_MAJOR)
        list(GET PARTS 1 VERSION_MINOR)
        list(GET PARTS 2 VERSION_PATCH)
        list(GET PARTS 3 VERSION_BUILD)

        set_target_properties(${target_var} PROPERTIES
                REVISION      "${GIT_REVISION}"
                VERSION       "${REVISION}~${COMMIT_HASH}"
                GIT_DESCRIBE  "${GIT_DESCRIBE}"
                VERSION_MAJOR "${VERSION_MAJOR}"
                VERSION_MINOR "${VERSION_MINOR}"
                VERSION_PATCH "${VERSION_PATCH}"
                VERSION_BUILD "${VERSION_BUILD}"
        )
    endif()
endfunction()

#[[
if (GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
    option(GIT_SUBMODULE "Check submodules during build" ON)
    if (GIT_SUBMODULE)
        message(STATUS "Submodule update")
        execute_process(COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                RESULT_VARIABLE GIT_SUBMOD_RESULT)
        if (NOT GIT_SUBMOD_RESULT EQUAL "0")
            message(FATAL_ERROR "git submodule update --init --recursive failed with ${GIT_SUBMOD_RESULT}, please checkout submodules")
        endif ()
    endif ()

    execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-list HEAD --count
            OUTPUT_VARIABLE REVISION
            OUTPUT_STRIP_TRAILING_WHITESPACE
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )

    execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
            OUTPUT_VARIABLE COMMIT_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )

    execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags
            OUTPUT_VARIABLE GIT_DESC
            RESULT_VARIABLE GIT_DESC_RESULT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    if(GIT_DESC_RESULT EQUAL "0")
        set(COMMIT_DESC "${GIT_DESC}")
    endif()

    set(VERSION "${REVISION}~${COMMIT_HASH}")
endif ()
]]
