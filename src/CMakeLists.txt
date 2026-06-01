file(GLOB_RECURSE CORE_SOURCES 
    "core/*.c"
    "protocol/*.c"
    "processing/*.c"
    "util/*.c"
)

add_library(machaudio_core STATIC ${CORE_SOURCES})

target_include_directories(machaudio_core PUBLIC 
    ${CMAKE_SOURCE_DIR}/include
    ${LIBURING_INCLUDE_DIRS}
)

target_link_libraries(machaudio_core PRIVATE 
    ${LIBURING_LIBRARIES}
    ${OPUS_LIBRARIES}
)

# Main Executable
add_executable(machaudio main.c)
target_link_libraries(machaudio PRIVATE machaudio_core m)

# Test Client
add_executable(machaudio-client client/main.c)
target_link_libraries(machaudio-client PRIVATE machaudio_core ${LIBUV_LIBRARIES} ${OPUS_LIBRARIES} m)
