add_executable(unit_tests
    test_main.c
    test_arena.c
    test_protocol.c
    test_transcode.c
    test_resample_adv.c
    test_mix.c
    test_vad_gru.c
)

target_link_libraries(unit_tests PRIVATE 
    unity 
    machaudio_core
    m
)

add_test(NAME unit_tests COMMAND unit_tests)
