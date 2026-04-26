add_executable(asclepius-cli main.cpp)
set_target_properties(asclepius-cli PROPERTIES OUTPUT_NAME asclepius)

target_link_libraries(asclepius-cli
    PRIVATE
        asclepius::asclepius
        CLI11::CLI11
        spdlog::spdlog
)

asclepius_target_warnings(asclepius-cli)
asclepius_target_sanitizers(asclepius-cli)

install(TARGETS asclepius-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
