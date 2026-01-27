cmake_minimum_required(VERSION 3.13)

if (NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if (EXISTS $ENV{DEVKITPRO}/cmake/Switch.cmake)
        set(CMAKE_TOOLCHAIN_FILE $ENV{DEVKITPRO}/cmake/Switch.cmake)
    else()
        message(FATAL_ERROR "please run 'sudo pacman -S switch-cmake`")
    endif()
endif()

project(libhaze LANGUAGES C CXX)

add_library(libhaze
    ${libhaze_SOURCE_DIR}/source/async_usb_server.cpp
    ${libhaze_SOURCE_DIR}/source/device_properties.cpp
    ${libhaze_SOURCE_DIR}/source/event_reactor.cpp
    ${libhaze_SOURCE_DIR}/source/haze.cpp
    ${libhaze_SOURCE_DIR}/source/ptp_object_database.cpp
    ${libhaze_SOURCE_DIR}/source/ptp_object_heap.cpp
    ${libhaze_SOURCE_DIR}/source/ptp_responder_mtp_operations.cpp
    ${libhaze_SOURCE_DIR}/source/ptp_responder_ptp_operations.cpp
    ${libhaze_SOURCE_DIR}/source/ptp_responder.cpp
    ${libhaze_SOURCE_DIR}/source/usb_session.cpp
    ${libhaze_SOURCE_DIR}/source/threaded_file_transfer.cpp
)

target_include_directories(libhaze PUBLIC ${libhaze_SOURCE_DIR}/include)

set_target_properties(libhaze PROPERTIES
    C_STANDARD 11
    C_EXTENSIONS ON
    CXX_STANDARD 20
    CXX_EXTENSIONS ON
    # force optimisations in debug mode as otherwise vapor errors
    # due to force_inline attribute failing...
    COMPILE_OPTIONS "$<$<CONFIG:Debug>:-Os>"
)

target_compile_options(libhaze PRIVATE
    -Wall
    -Wno-tautological-compare
)

if (BUILD_EXAMPLES)
    add_executable(libhaze-example example/source/main.cpp)

    target_link_libraries(libhaze-example PRIVATE libhaze)

    set_target_properties(libhaze-example PROPERTIES
        C_STANDARD 11
        C_EXTENSIONS ON
        CXX_STANDARD 20
        CXX_EXTENSIONS ON
    )

    target_compile_options(libhaze-example PRIVATE
        -Wall
    )

    nx_generate_nacp(libhaze-example.nacp
        NAME "libhaze-example"
        AUTHOR "TotalJustice"
        VERSION 2.0.0
    )

    nx_create_nro(libhaze-example
        NACP libhaze-example.nacp
    )
endif()
