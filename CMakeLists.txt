cmake_minimum_required(VERSION 3.16)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" REMYDESK_VERSION)
string(STRIP "${REMYDESK_VERSION}" REMYDESK_VERSION)
project(Remydesk VERSION "${REMYDESK_VERSION}" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_executable(remydeskd
    src/core/main.cpp
    src/core/config.cpp
    src/core/download_manager.cpp
    src/core/file_service.cpp
    src/core/hardware_probe.cpp
    src/core/http_server.cpp
    src/core/network_manager.cpp
    src/core/process.cpp
    src/core/service_manager.cpp
)
target_include_directories(remydeskd PRIVATE src/core third_party)
target_compile_definitions(remydeskd PRIVATE REMYDESK_VERSION="${REMYDESK_VERSION}")
target_compile_options(remydeskd PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(remydeskd PRIVATE Threads::Threads)

install(TARGETS remydeskd RUNTIME DESTINATION bin)
install(DIRECTORY web/ DESTINATION share/remydesk/web)

include(CTest)
if(BUILD_TESTING)
    add_test(
        NAME remydesk_api_smoke
        COMMAND ${CMAKE_COMMAND} -E env
            REMYDESK_TEST_BINARY=$<TARGET_FILE:remydeskd>
            bash ${CMAKE_SOURCE_DIR}/tests/smoke_api.sh
    )
    set_tests_properties(remydesk_api_smoke PROPERTIES TIMEOUT 30)
endif()
