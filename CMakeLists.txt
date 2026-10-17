cmake_minimum_required(VERSION 3.14)
project(prism LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

include(FetchContent)

# Dependencies are pinned to commits. reactor is declared first (FetchContent's first
# declaration wins) for its TLS Server role without an authority, which --tls-cert uses.
FetchContent_Declare(reactor GIT_REPOSITORY https://github.com/Kronuz/reactor.git GIT_TAG 56783e5f9c3bbe36257e92e4bfd62cc3f53f0d48)

# The HTTP/1.1 application framework on standalone Asio (C++20 coroutines). It
# transitively pulls reactor (the Asio server runtime), http-parser, radix-router,
# and compressors (the response-compression codecs), so the whole request path
# comes in with one dependency.
FetchContent_Declare(http_lib GIT_REPOSITORY https://github.com/Kronuz/http.git GIT_TAG d3b3667339c011e8b5d3a84be3958f450be266e4)
FetchContent_MakeAvailable(http_lib)

# Formatted crash tracebacks (capture + demangle + macOS file:line), for the
# error path.
FetchContent_Declare(traceback GIT_REPOSITORY https://github.com/Kronuz/traceback.git GIT_TAG 689862139ab45bd0b9ff584f4084ed710b01784a)
FetchContent_MakeAvailable(traceback)

# The deferred, lock-free logger (leveled, colored, timestamped). It transitively
# brings term-color (the stacked-escape palette + the collapse/apply resolution
# the logger sink applies per --color/NO_COLOR/terminal) and scheduler, so prism's
# whole logging + color stack is the same one Xapiand runs on.
FetchContent_Declare(logger GIT_REPOSITORY https://github.com/Kronuz/logger.git GIT_TAG 8c5afcd89030586939dd5229ee2ae583069b83dd)
FetchContent_MakeAvailable(logger)

# The request/response logging middleware (recovers Xapiand's dropped to_text
# logging): an http::HttpHandler decorator on http + logger + term-color.
FetchContent_Declare(http_log GIT_REPOSITORY https://github.com/Kronuz/http-log.git GIT_TAG 090f3fbbebc067a3a9a62e64a2abf87289dbaf5d)
FetchContent_MakeAvailable(http_log)

add_executable(prism src/main.cc)
target_link_libraries(prism PRIVATE http::http traceback::traceback logger::logger http_log::http_log Threads::Threads)
# HTTPS (--tls-cert/--tls-key) when OpenSSL 3 is available and reactor built its TLS layer.
if(TARGET reactor::tls)
  target_link_libraries(prism PRIVATE reactor::tls)
  target_compile_definitions(prism PRIVATE PRISM_TLS=1)
endif()
