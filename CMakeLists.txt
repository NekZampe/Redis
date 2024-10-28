cmake_minimum_required(VERSION 3.30)
project(Redis)

set(CMAKE_CXX_STANDARD 20)

add_executable(clientDriver
        client/clientDriver.cpp
        client/client.cpp)

add_executable(serverDriver
        server/server.cpp
        server/serverDriver.cpp)



