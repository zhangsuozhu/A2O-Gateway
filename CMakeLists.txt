cmake_minimum_required(VERSION 3.16)
project(claude_code_openai_gateway C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter -O2")

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBEVENT REQUIRED libevent libevent_pthreads)
pkg_check_modules(LIBCURL REQUIRED libcurl)
pkg_check_modules(CJSON REQUIRED libcjson)

add_executable(cc-oai-gateway src/main.c)
target_include_directories(cc-oai-gateway PRIVATE ${LIBEVENT_INCLUDE_DIRS} ${LIBCURL_INCLUDE_DIRS} ${CJSON_INCLUDE_DIRS})
target_link_directories(cc-oai-gateway PRIVATE ${LIBEVENT_LIBRARY_DIRS} ${LIBCURL_LIBRARY_DIRS} ${CJSON_LIBRARY_DIRS})
target_link_libraries(cc-oai-gateway PRIVATE ${LIBEVENT_LIBRARIES} ${LIBCURL_LIBRARIES} ${CJSON_LIBRARIES} pthread)

install(TARGETS cc-oai-gateway DESTINATION bin)
install(DIRECTORY web config DESTINATION share/cc-oai-gateway)
