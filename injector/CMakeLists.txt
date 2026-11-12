cmake_minimum_required(VERSION 3.20)
project(slinky-inject LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(slinky-inject
    main.cpp
    tui.cpp
)

# The injector ships as Slinky.exe. The target name is left alone so the build graph,
# the copy step below and existing build directories keep working unchanged.
set_target_properties(slinky-inject PROPERTIES OUTPUT_NAME "Slinky")

if(WIN32)
    target_sources(slinky-inject PRIVATE inject_windows.cpp)
    target_compile_definitions(slinky-inject PRIVATE _WIN32_WINNT=0x0601)
    # psapi: EnumProcessModulesEx / GetModuleFileNameExW, used to check whether a
    # candidate process has LWJGL loaded and whether the client is already mapped.
    target_link_libraries(slinky-inject PRIVATE psapi)
    # A standalone .exe: no MinGW runtime DLLs to copy beside it.
    target_link_options(slinky-inject PRIVATE -static -static-libgcc -static-libstdc++)

    # ---- embed the client library ----
    # The injector ships as one file, so the DLL travels inside it as RCDATA 200 and is
    # written out at run time, the same arrangement the reference launcher uses.
    # The DLL is looked for in the sibling client build tree first, then in dist, so
    # this works whether or not the two were built together.
    set(SLINKY_PAYLOAD "" CACHE FILEPATH "Client DLL to embed in Slinky.exe")
    if(NOT SLINKY_PAYLOAD)
        foreach(candidate
                "${CMAKE_CURRENT_SOURCE_DIR}/../build/cmake/SlinkyNative.dll"
                "${CMAKE_CURRENT_SOURCE_DIR}/../dist/SlinkyNative.dll")
            if(EXISTS "${candidate}")
                set(SLINKY_PAYLOAD "${candidate}")
                break()
            endif()
        endforeach()
    endif()
    if(SLINKY_PAYLOAD AND EXISTS "${SLINKY_PAYLOAD}")
        get_filename_component(SLINKY_PAYLOAD "${SLINKY_PAYLOAD}" ABSOLUTE)
        # A .rc path in an #include takes backslashes as escapes; forward slashes work
        # for windres either way.
        file(TO_CMAKE_PATH "${SLINKY_PAYLOAD}" _payload_rc)
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/injector.rc.in"
                       "${CMAKE_CURRENT_BINARY_DIR}/injector.rc" @ONLY)
        set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/injector.rc"
                                    PROPERTIES LANGUAGE RC
                                               OBJECT_DEPENDS "${SLINKY_PAYLOAD}")
        target_sources(slinky-inject PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/injector.rc")
        # OBJECT_DEPENDS is what actually re-embeds on a client rebuild. Relying on
        # CMAKE_CONFIGURE_DEPENDS alone was not enough: it only re-runs configure, and
        # configure_file leaves the .rc untouched when its content is unchanged, so
        # ninja found nothing to rebuild and a fresh Slinky.exe kept the OLD payload.
        # windres reads the DLL directly, so the .rc now depends on it by name.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SLINKY_PAYLOAD}")
        message(STATUS "Embedding client: ${SLINKY_PAYLOAD}")
    else()
        # Not fatal: a development build without the client still produces an injector
        # that can be pointed at a DLL with --lib.
        message(WARNING "No client DLL found to embed; Slinky.exe will need --lib <path>")
    endif()
else()
    target_sources(slinky-inject PRIVATE inject_linux.cpp)
    target_link_libraries(slinky-inject PRIVATE ${CMAKE_DL_LIBS} pthread)
endif()

# Copy the client library next to the injector when it is built in the same
# tree (the top-level CMakeLists.txt does this via add_subdirectory).
# A separate always-run target: a POST_BUILD step on slinky-inject only ran when
# the injector itself was relinked, so the sibling copy went stale whenever only
# the client changed.
if(TARGET slinky)
    add_custom_target(slinky-copy-beside-injector ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:slinky>
                $<TARGET_FILE_DIR:slinky-inject>/$<TARGET_FILE_NAME:slinky>
        DEPENDS slinky slinky-inject
        VERBATIM)
endif()
