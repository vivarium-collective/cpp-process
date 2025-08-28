cmake_minimum_required(VERSION 3.15)
project(vivarium_cpp_process LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# nlohmann/json is header-only; on Debian/Ubuntu use: apt-get install nlohmann-json3-dev
find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp)

if (NOT NLOHMANN_JSON_INCLUDE_DIR)
  message(FATAL_ERROR "nlohmann/json.hpp not found. Install nlohmann-json3-dev or add to include path.")
endif()

add_executable(vivarium_cpp_process src/main.cpp)
target_include_directories(vivarium_cpp_process PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
