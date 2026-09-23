# --- Phase 1: gRPC 뼈대 -----------------------------------------------
# proto/sysmonitor.proto로부터 *.pb.cc/*.grpc.pb.cc를 빌드 시점에 생성한다.

set(PROTO_DIR "${CMAKE_SOURCE_DIR}/proto")
set(PROTO_FILE "${PROTO_DIR}/sysmonitor.proto")
set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

set(PROTO_SRCS "${PROTO_GEN_DIR}/sysmonitor.pb.cc")
set(PROTO_HDRS "${PROTO_GEN_DIR}/sysmonitor.pb.h")
set(GRPC_SRCS "${PROTO_GEN_DIR}/sysmonitor.grpc.pb.cc")
set(GRPC_HDRS "${PROTO_GEN_DIR}/sysmonitor.grpc.pb.h")

add_custom_command(
	OUTPUT ${PROTO_SRCS} ${PROTO_HDRS} ${GRPC_SRCS} ${GRPC_HDRS}
	COMMAND $<TARGET_FILE:protoc>
	ARGS --grpc_out=${PROTO_GEN_DIR}
	     --cpp_out=${PROTO_GEN_DIR}
	     --plugin=protoc-gen-grpc=$<TARGET_FILE:grpc_cpp_plugin>
	     -I ${PROTO_DIR}
	     ${PROTO_FILE}
	DEPENDS ${PROTO_FILE} protoc grpc_cpp_plugin
)

add_library(SysMonitorServerProto STATIC ${PROTO_SRCS} ${GRPC_SRCS})
target_include_directories(SysMonitorServerProto PUBLIC ${PROTO_GEN_DIR})
target_link_libraries(SysMonitorServerProto PUBLIC grpc++ libprotobuf)

# --- Phase 2: agents.ini 파싱 (inih) ------------------------------------
add_library(inih STATIC "${inih_SOURCE_DIR}/ini.c" "${inih_SOURCE_DIR}/cpp/INIReader.cpp")
target_include_directories(inih PUBLIC "${inih_SOURCE_DIR}" "${inih_SOURCE_DIR}/cpp")

add_executable(SysMonitorServer
	"main.cpp"
	"grpc/GrpcServer.cpp"
	"AgentAuth.cpp"
)
target_include_directories(SysMonitorServer PRIVATE "../include")
target_link_libraries(SysMonitorServer PRIVATE SysMonitorServerProto spdlog::spdlog inih)

if(MSVC)
    target_compile_options(SysMonitorServerProto PRIVATE /utf-8)
    target_compile_options(SysMonitorServer PRIVATE /utf-8)
    target_compile_options(inih PRIVATE /utf-8)
endif()
