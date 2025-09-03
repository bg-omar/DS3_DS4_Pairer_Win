cmake_minimum_required(VERSION 3.18)

project(sixaxispairer VERSION 1.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(SOURCES
        pair_sixaxis_win.c      # your Windows-native HID code from the previous message
)

add_executable(${PROJECT_NAME} ${SOURCES})
add_executable(sixaxispairer_gui WIN32 gui_sixaxispairer.c sixaxispairer_gui.rc)

target_compile_definitions(sixaxispairer_gui PRIVATE WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_link_libraries(sixaxispairer_gui PRIVATE hid setupapi comctl32)


# Windows-only: link to native HID + SetupAPI (no hidapi).
if (WIN32)
    # Helpful warnings + UTF-8 on MSVC
    if (MSVC)
        target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    target_compile_definitions(${PROJECT_NAME} PRIVATE WIN32_LEAN_AND_MEAN)

    # These libraries are provided by the Windows SDK/MinGW toolchains
    target_link_libraries(${PROJECT_NAME} PRIVATE hid setupapi)
endif()

# (Optional) Keep macOS bits around but disabled by default since you said "Windows only".
# if(APPLE)
#   find_library(IOKIT NAMES IOKit)
#   if (IOKIT)
#     target_link_libraries(${PROJECT_NAME} PRIVATE ${IOKIT})
#   endif()
# endif()

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
