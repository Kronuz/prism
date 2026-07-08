cmake_minimum_required(VERSION 3.14)
project(prism LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

include(FetchContent)

# The HTTP/1.1 application framework on standalone Asio (C++20 coroutines). It
# transitively pulls reactor (the Asio server runtime), http-parser, radix-router,
# and compressors (the response-compression codecs), so the whole request path
# comes in with one dependency.
FetchContent_Declare(http_lib GIT_REPOSITORY https://github.com/Kronuz/http.git GIT_TAG main)
FetchContent_MakeAvailable(http_lib)

# Formatted crash tracebacks (capture + demangle + macOS file:line), for the
# error path.
FetchContent_Declare(traceback GIT_REPOSITORY https://github.com/Kronuz/traceback.git GIT_TAG main)
FetchContent_MakeAvailable(traceback)

add_executable(prism src/main.cc)
target_link_libraries(prism PRIVATE http::http traceback::traceback Threads::Threads)
