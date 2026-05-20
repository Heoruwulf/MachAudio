add_executable(fuzz_protocol fuzz_protocol.c)
target_link_libraries(fuzz_protocol machaudio_core ${OPUS_LIBRARIES} ${LIBUV_LIBRARIES} m)
target_link_options(fuzz_protocol PRIVATE -fsanitize=fuzzer)
