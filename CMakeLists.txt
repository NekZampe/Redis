cmake_minimum_required(VERSION 3.2)
project(Redis)

set(CMAKE_CXX_STANDARD 20)

add_executable(clientDriver
        client/client.cpp)

add_executable(serverDriver
        server/server.cpp
        server/hashtable.cpp
        server/hashtable.h
        server/avl.cpp
        server/test_avl.cpp
        server/avl.h
        server/zset.cpp
        server/zset.h
        server/common.h
)



