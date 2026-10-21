add_library(silent_interfaces INTERFACE)

target_include_directories(silent_interfaces
    INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
