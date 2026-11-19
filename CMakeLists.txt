cmake_minimum_required(VERSION 3.14)
project(prism LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

include(FetchContent)

# Dependencies are pinned to commits; FetchContent's first declaration wins, so these pins hold
# for every library that fetches the same dependency.

# task: coroutines, the lifecycle (components, stop sources, stop_signals) and the process step
# (run_daemon, ProcessLock). Declared before http so reactor and http build against this pin.
FetchContent_Declare(task GIT_REPOSITORY https://github.com/Kronuz/task.git GIT_TAG 0316befffa225849f04a8064ea6c46a334e52b94)

# RapidJSON reads prism.json. Headers only: its own build stays off.
FetchContent_Declare(rapidjson GIT_REPOSITORY https://github.com/Tencent/rapidjson.git GIT_TAG 24b5e7a8b27f42fa16b96fc70aade9106cf7102f
  SOURCE_SUBDIR do-not-configure)
FetchContent_MakeAvailable(rapidjson)

# The HTTP/1.1 application framework on standalone Asio (C++20 coroutines). It
# transitively pulls reactor (the Asio server runtime, with its TLS Server role),
# http-parser, radix-router, and compressors (the response-compression codecs), so
# the whole request path comes in with one dependency.
FetchContent_Declare(http_lib GIT_REPOSITORY https://github.com/Kronuz/http.git GIT_TAG 58d4188f78b769286005fc744485a54a22a4e243)
FetchContent_MakeAvailable(http_lib)

# The deferred, lock-free logger (leveled, colored, timestamped). It transitively
# brings term-color (the stacked-escape palette + the collapse/apply resolution
# the logger sink applies per --color/NO_COLOR/terminal) and scheduler, so prism's
# whole logging + color stack is the same one Xapiand runs on.
FetchContent_Declare(logger GIT_REPOSITORY https://github.com/Kronuz/logger.git GIT_TAG 529ae7694a33a183725b9f453cf63539d9690890)
FetchContent_MakeAvailable(logger)

# The request/response logging middleware (recovers Xapiand's dropped to_text
# logging): an http::HttpHandler decorator on http + logger + term-color.
FetchContent_Declare(http_log GIT_REPOSITORY https://github.com/Kronuz/http-log.git GIT_TAG 5e3df31e359336cd36b1949e2895c0951b356b31)
FetchContent_MakeAvailable(http_log)

# Formatted crash tracebacks (capture + demangle + macOS file:line), for the
# error path. Declared after http-log: traceback pins its own strings, and the
# first declaration wins, so http-log's newer strings and repr must come first.
FetchContent_Declare(traceback GIT_REPOSITORY https://github.com/Kronuz/traceback.git GIT_TAG b607242049aa996132a4544a018dafc3bbc71245)
FetchContent_MakeAvailable(traceback)

add_executable(prism src/main.cc)
target_include_directories(prism PRIVATE "${rapidjson_SOURCE_DIR}/include")
target_link_libraries(prism PRIVATE http::http task::asio traceback::traceback logger::logger http_log::http_log Threads::Threads)
# HTTPS (--tls-cert/--tls-key) when OpenSSL 3 is available and reactor built its TLS layer.
if(TARGET reactor::tls)
  target_link_libraries(prism PRIVATE reactor::tls)
  target_compile_definitions(prism PRIVATE PRISM_TLS=1)
endif()

# The lifecycle test runs the built prism: readiness, the pid lock, and a SIGTERM under load.
enable_testing()
add_executable(prism_lifecycle_test src/test/lifecycle_test.cc)
target_link_libraries(prism_lifecycle_test PRIVATE task::task Threads::Threads)
add_test(NAME prism_lifecycle COMMAND prism_lifecycle_test $<TARGET_FILE:prism>)
