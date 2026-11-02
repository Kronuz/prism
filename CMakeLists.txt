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
FetchContent_Declare(reactor GIT_REPOSITORY https://github.com/Kronuz/reactor.git GIT_TAG c6b2c3e5743d59a71bae4ad25010f8f66e8c093e)

# The HTTP/1.1 application framework on standalone Asio (C++20 coroutines). It
# transitively pulls reactor (the Asio server runtime), http-parser, radix-router,
# and compressors (the response-compression codecs), so the whole request path
# comes in with one dependency.
FetchContent_Declare(http_lib GIT_REPOSITORY https://github.com/Kronuz/http.git GIT_TAG d76c5efa4d4c4ae1a181780f593dc6b06786c9a5)
FetchContent_MakeAvailable(http_lib)

# The deferred, lock-free logger (leveled, colored, timestamped). It transitively
# brings term-color (the stacked-escape palette + the collapse/apply resolution
# the logger sink applies per --color/NO_COLOR/terminal) and scheduler, so prism's
# whole logging + color stack is the same one Xapiand runs on.
FetchContent_Declare(logger GIT_REPOSITORY https://github.com/Kronuz/logger.git GIT_TAG 765528e977b59e97b0ebf79738f9bf9041635c61)
FetchContent_MakeAvailable(logger)

# The request/response logging middleware (recovers Xapiand's dropped to_text
# logging): an http::HttpHandler decorator on http + logger + term-color.
FetchContent_Declare(http_log GIT_REPOSITORY https://github.com/Kronuz/http-log.git GIT_TAG 8a38bb0efdc5ab4e55951e222345e18efa68f924)
FetchContent_MakeAvailable(http_log)

# Formatted crash tracebacks (capture + demangle + macOS file:line), for the
# error path. Declared after http-log: traceback pins its own strings, and the
# first declaration wins, so http-log's newer strings and repr must come first.
FetchContent_Declare(traceback GIT_REPOSITORY https://github.com/Kronuz/traceback.git GIT_TAG b607242049aa996132a4544a018dafc3bbc71245)
FetchContent_MakeAvailable(traceback)

add_executable(prism src/main.cc)
target_link_libraries(prism PRIVATE http::http traceback::traceback logger::logger http_log::http_log Threads::Threads)
# HTTPS (--tls-cert/--tls-key) when OpenSSL 3 is available and reactor built its TLS layer.
if(TARGET reactor::tls)
  target_link_libraries(prism PRIVATE reactor::tls)
  target_compile_definitions(prism PRIVATE PRISM_TLS=1)
endif()
