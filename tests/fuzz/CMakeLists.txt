add_executable(fuzz_protocol fuzz_protocol.c)
target_link_libraries(fuzz_protocol machaudio_core ${OPUS_LIBRARIES} ${LIBUV_LIBRARIES} m)
target_link_options(fuzz_protocol PRIVATE -fsanitize=fuzzer)

add_executable(fuzz_vad fuzz_vad.c)
target_link_libraries(fuzz_vad machaudio_core ${OPUS_LIBRARIES} ${LIBUV_LIBRARIES} m)
target_link_options(fuzz_vad PRIVATE -fsanitize=fuzzer)
