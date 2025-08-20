cmake_minimum_required(VERSION 3.15)
project(BitTrickle LANGUAGES CXX)

# Use C++23
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Output directory for executables
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/build)

# Add executables
add_executable(Server src/C++/Server.cpp)
add_executable(Client src/C++/Client.cpp)
