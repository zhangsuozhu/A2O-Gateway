cmake_minimum_required(VERSION 3.16)
project(claude_code_openai_gateway C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter -O2")

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBEVENT REQUIRED libevent libevent_pthreads)
pkg_check_modules(LIBCURL REQUIRED libcurl)
pkg_check_modules(CJSON REQUIRED libcjson)

set(GATEWAY_SOURCES
    src/main.c
    src/log.c
    src/config.c
    src/convert.c
    src/stream.c
    src/worker.c
    src/handlers.c
    src/stats.c
)

add_executable(cc-oai-gateway ${GATEWAY_SOURCES})
target_include_directories(cc-oai-gateway PRIVATE src ${LIBEVENT_INCLUDE_DIRS} ${LIBCURL_INCLUDE_DIRS} ${CJSON_INCLUDE_DIRS})
target_link_directories(cc-oai-gateway PRIVATE ${LIBEVENT_LIBRARY_DIRS} ${LIBCURL_LIBRARY_DIRS} ${CJSON_LIBRARY_DIRS})
target_link_libraries(cc-oai-gateway PRIVATE ${LIBEVENT_LIBRARIES} ${LIBCURL_LIBRARIES} ${CJSON_LIBRARIES} pthread)

enable_testing()
add_executable(test_convert tests/test_convert.c src/convert.c)
target_include_directories(test_convert PRIVATE src ${CJSON_INCLUDE_DIRS})
target_link_directories(test_convert PRIVATE ${CJSON_LIBRARY_DIRS})
target_link_libraries(test_convert PRIVATE ${CJSON_LIBRARIES})
add_test(NAME convert_tests COMMAND test_convert)

add_executable(test_stream tests/test_stream.c src/stream.c src/convert.c src/log.c)
target_include_directories(test_stream PRIVATE src ${LIBEVENT_INCLUDE_DIRS} ${LIBCURL_INCLUDE_DIRS} ${CJSON_INCLUDE_DIRS})
target_link_directories(test_stream PRIVATE ${LIBEVENT_LIBRARY_DIRS} ${LIBCURL_LIBRARY_DIRS} ${CJSON_LIBRARY_DIRS})
target_link_libraries(test_stream PRIVATE ${LIBEVENT_LIBRARIES} ${LIBCURL_LIBRARIES} ${CJSON_LIBRARIES} pthread)
add_test(NAME stream_tests COMMAND test_stream)

install(TARGETS cc-oai-gateway DESTINATION bin)
install(DIRECTORY web config DESTINATION share/cc-oai-gateway)
