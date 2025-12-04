cmake_minimum_required(VERSION 3.14)
project(AiAgentExample LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Ищем SQLite3
find_library(SQLITE3_LIBRARY sqlite3)

# Ищем CURL
find_package(CURL REQUIRED)

# nlohmann/json
include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# OpenSSL для TLS
find_package(OpenSSL REQUIRED)

add_executable(ai_agent
    src/AiAgent.cpp
    src/main.cpp
)

target_include_directories(ai_agent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(ai_agent
    PRIVATE
      nlohmann_json::nlohmann_json
      OpenSSL::SSL
      OpenSSL::Crypto
      CURL::libcurl  # Добавляем libcurl
)

# Добавляем SQLite3 после объявления цели
if(SQLITE3_LIBRARY)
    message(STATUS "Found SQLite3: ${SQLITE3_LIBRARY}")
    target_link_libraries(ai_agent PRIVATE ${SQLITE3_LIBRARY})
else()
    message(WARNING "SQLite3 not found - context features will be disabled")
    target_compile_definitions(ai_agent PRIVATE NO_SQLITE)
endif()