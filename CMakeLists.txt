cmake_minimum_required(VERSION 3.14)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project("SysMonitor-Server" LANGUAGES CXX C)

include(FetchContent)

FetchContent_Declare(
	spdlog
	GIT_REPOSITORY https://github.com/gabime/spdlog
	GIT_TAG v1.x
)

FetchContent_Declare(
	grpc
	GIT_REPOSITORY https://github.com/grpc/grpc
	GIT_TAG v1.66.0
)

# inih (agents.ini 파싱용) - CMakeLists가 없는 저장소라 소스만 받아 직접 컴파일한다 (src/CMakeLists.txt 참고).
FetchContent_Declare(
	inih
	GIT_REPOSITORY https://github.com/benhoyt/inih
	GIT_TAG r58
)

set(FETCHCONTENT_QUIET OFF)
set(gRPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_CSHARP_EXT OFF CACHE BOOL "" FORCE)
set(gRPC_SSL_PROVIDER "module" CACHE STRING "" FORCE)
set(OPENSSL_NO_ASM ON CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(spdlog grpc)

FetchContent_GetProperties(inih)
if(NOT inih_POPULATED)
	FetchContent_Populate(inih)
endif()

add_subdirectory("src")
