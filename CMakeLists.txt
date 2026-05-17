cmake_minimum_required(VERSION 3.16)
project(WebAgent VERSION 2.0.0 LANGUAGES CXX)

# C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options
option(BUILD_TESTS "Build unit tests" ON)
option(BUILD_DOCS "Build documentation" OFF)
option(ENABLE_COVERAGE "Enable code coverage" OFF)

# Find dependencies
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_package(nlohmann_json 3.11.0 REQUIRED)  # Proper JSON library
find_package(spdlog 1.11.0 REQUIRED)        # Structured logging
find_package(fmt 9.1.0 REQUIRED)            # String formatting

if(BUILD_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()
endif()

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)

# Source files
set(SOURCES
    src/main.cpp
    src/Config.cpp
    src/Logger.cpp
    src/HttpClient.cpp
    src/TaskHandler.cpp
    src/TaskDispatcher.cpp
    src/ResultSender.cpp
    src/StateMachine.cpp
    src/models/Task.cpp
    src/models/Result.cpp
    src/models/AgentState.cpp
)

# Create library
add_library(webagent_core STATIC ${SOURCES})
target_link_libraries(webagent_core
    PRIVATE
    CURL::libcurl
    Threads::Threads
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    fmt::fmt
)

# Create executable
add_executable(${PROJECT_NAME} src/main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE webagent_core)

# Install targets
install(TARGETS ${PROJECT_NAME} webagent_core
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib/static
)

install(DIRECTORY config/ DESTINATION etc/webagent)
install(DIRECTORY include/ DESTINATION include/webagent)

# Tests
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# CPack for packaging
include(CPack)
set(CPACK_GENERATOR "TGZ;DEB")
set(CPACK_PACKAGE_NAME ${PROJECT_NAME})
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
