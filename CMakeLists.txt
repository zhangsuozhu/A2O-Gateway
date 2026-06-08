cmake_minimum_required(VERSION 3.16)
project(claude_code_openai_gateway C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter -O2")
set_source_files_properties(src/sqlite3/sqlite3.c PROPERTIES COMPILE_FLAGS "-O0 -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION")

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBEVENT REQUIRED libevent libevent_pthreads)
pkg_check_modules(LIBCURL REQUIRED libcurl)
pkg_check_modules(CJSON REQUIRED libcjson)
# cjson pkg-config 在某些 cmake 版本下 include 路径解析不正确，手动兜底
if(NOT EXISTS "${CJSON_INCLUDE_DIRS}/cJSON.h")
    find_path(CJSON_INCLUDE_DIRS cJSON.h /usr/include/cjson /usr/local/include/cjson)
endif()

# Optional: OpenSSL + libevent_openssl for HTTPS listening
pkg_check_modules(LIBEVENT_SSL libevent_openssl)
pkg_check_modules(OPENSSL openssl)


# Generate embedded CA header if missing (same CA across all builds)
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/src/generated/ca_embedded.h)
    execute_process(COMMAND ${CMAKE_SOURCE_DIR}/scripts/gen-ca.sh
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Generating embedded CA certificate")
endif()
# Generate embedded admin.html C file when the source HTML changes
add_custom_command(
    OUTPUT ${CMAKE_SOURCE_DIR}/src/generated/admin_html.c
    COMMAND xxd -i admin.html |
            sed 's/unsigned char admin_html\\[\\]/unsigned char EMBEDDED_ADMIN_HTML[]/' |
            sed 's/unsigned int admin_html_len/unsigned int EMBEDDED_ADMIN_HTML_LEN/' >
            ${CMAKE_SOURCE_DIR}/src/generated/admin_html.c
    DEPENDS ${CMAKE_SOURCE_DIR}/web/admin.html
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/web
    COMMENT "Embedding web/admin.html into C source"
)

set(GATEWAY_SOURCES
    src/main.c
    src/log.c
    src/config.c
    src/convert.c
    src/stream.c
    src/worker.c
    src/handlers.c
    src/stats.c
    src/db.c
    src/sqlite3/sqlite3.c
    src/generated/admin_html.c
    src/generated/favicon.c
)

add_executable(cc-oai-gateway ${GATEWAY_SOURCES})
target_include_directories(cc-oai-gateway PRIVATE src src/sqlite3 ${LIBEVENT_INCLUDE_DIRS} ${LIBCURL_INCLUDE_DIRS} ${CJSON_INCLUDE_DIRS})
target_link_directories(cc-oai-gateway PRIVATE ${LIBEVENT_LIBRARY_DIRS} ${LIBCURL_LIBRARY_DIRS} ${CJSON_LIBRARY_DIRS})
target_link_libraries(cc-oai-gateway PRIVATE ${LIBEVENT_LIBRARIES} ${LIBCURL_LIBRARIES} ${CJSON_LIBRARIES} pthread m dl)
if(LIBEVENT_SSL_FOUND AND OPENSSL_FOUND)
    target_compile_definitions(cc-oai-gateway PRIVATE HAVE_SSL)
    target_include_directories(cc-oai-gateway PRIVATE ${LIBEVENT_SSL_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIRS})
    target_link_directories(cc-oai-gateway PRIVATE ${LIBEVENT_SSL_LIBRARY_DIRS} ${OPENSSL_LIBRARY_DIRS})
    target_link_libraries(cc-oai-gateway PRIVATE ${LIBEVENT_SSL_LIBRARIES} ${OPENSSL_LIBRARIES})
endif()

enable_testing()
add_executable(test_convert tests/test_convert.c tests/test_stubs.c src/convert.c src/config.c src/log.c)
target_include_directories(test_convert PRIVATE src ${CJSON_INCLUDE_DIRS})
target_link_directories(test_convert PRIVATE ${CJSON_LIBRARY_DIRS})
target_link_libraries(test_convert PRIVATE ${CJSON_LIBRARIES} pthread)
add_test(NAME convert_tests COMMAND test_convert)

add_executable(test_stream tests/test_stream.c tests/test_stubs.c src/stream.c src/convert.c src/config.c src/log.c src/stats.c src/db.c src/sqlite3/sqlite3.c)
target_include_directories(test_stream PRIVATE src src/sqlite3 ${LIBEVENT_INCLUDE_DIRS} ${LIBCURL_INCLUDE_DIRS} ${CJSON_INCLUDE_DIRS})
target_link_directories(test_stream PRIVATE ${LIBEVENT_LIBRARY_DIRS} ${LIBCURL_LIBRARY_DIRS} ${CJSON_LIBRARY_DIRS})
target_link_libraries(test_stream PRIVATE ${LIBEVENT_LIBRARIES} ${LIBCURL_LIBRARIES} ${CJSON_LIBRARIES} pthread dl)
add_test(NAME stream_tests COMMAND test_stream)

add_executable(test_log tests/test_log.c src/log.c)
target_include_directories(test_log PRIVATE src ${CJSON_INCLUDE_DIRS})
target_link_directories(test_log PRIVATE ${CJSON_LIBRARY_DIRS})
target_link_libraries(test_log PRIVATE ${CJSON_LIBRARIES} pthread)
add_test(NAME log_tests COMMAND test_log)

add_executable(test_cache_control tests/test_cache_control.c tests/test_stubs.c src/convert.c src/config.c src/stats.c src/log.c src/db.c src/sqlite3/sqlite3.c)
target_include_directories(test_cache_control PRIVATE src src/sqlite3 ${CJSON_INCLUDE_DIRS})
target_link_directories(test_cache_control PRIVATE ${CJSON_LIBRARY_DIRS})
target_link_libraries(test_cache_control PRIVATE ${CJSON_LIBRARIES} pthread dl)
add_test(NAME cache_control_tests COMMAND test_cache_control)

add_executable(mock_server tests/mock_server.c)
target_include_directories(mock_server PRIVATE ${LIBEVENT_INCLUDE_DIRS})
target_link_directories(mock_server PRIVATE ${LIBEVENT_LIBRARY_DIRS})
target_link_libraries(mock_server PRIVATE ${LIBEVENT_LIBRARIES})

install(TARGETS cc-oai-gateway mock_server DESTINATION bin)
install(DIRECTORY config DESTINATION share/cc-oai-gateway)
