# OrcaSlicer Config Codegen CMake Module
#
# Generates C++ source files from protobuf schema definitions.
# Generated files are checked into the repo so builds work without protoc.
#
# Targets:
#   codegen_config  - Custom target to regenerate C++ from .proto files
#   validate_config - Custom target to validate generated vs original
#
# Usage in parent CMakeLists.txt:
#   include(cmake/modules/ConfigCodegen.cmake)

find_program(PROTOC_EXECUTABLE protoc)
find_package(Python3 COMPONENTS Interpreter QUIET)

set(CONFIG_PROTO_DIR      "${CMAKE_SOURCE_DIR}/src/proto")
set(CONFIG_PROTO_GEN_DIR  "${CMAKE_SOURCE_DIR}/src/proto/generated")
set(CONFIG_CODEGEN_DIR    "${CMAKE_SOURCE_DIR}/codegen/generated")
set(CONFIG_DESC_FILE      "${CMAKE_BINARY_DIR}/config.desc")

set(CODEGEN_TOOL          "${CMAKE_SOURCE_DIR}/tools/config_codegen.py")
set(VALIDATE_TOOL         "${CMAKE_SOURCE_DIR}/tools/validate_codegen.py")
set(RUN_CODEGEN_TOOL      "${CMAKE_SOURCE_DIR}/tools/run_codegen.py")

# Generated output files
set(CONFIG_GENERATED_SOURCES
    "${CONFIG_CODEGEN_DIR}/PrintConfigDef_generated.cpp"
    "${CONFIG_CODEGEN_DIR}/Preset_options_generated.cpp"
    "${CONFIG_CODEGEN_DIR}/Invalidation_generated.cpp"
    "${CONFIG_CODEGEN_DIR}/OptionKeys_generated.cpp"
)

# Collect all .proto source files
file(GLOB CONFIG_PROTO_FILES
    "${CONFIG_PROTO_DIR}/config_metadata.proto"
    "${CONFIG_PROTO_GEN_DIR}/*.proto"
)

if(PROTOC_EXECUTABLE AND Python3_EXECUTABLE)
    # Step 1: Compile .proto files to descriptor set
    add_custom_command(
        OUTPUT  ${CONFIG_DESC_FILE}
        COMMAND ${PROTOC_EXECUTABLE}
                --proto_path=${CONFIG_PROTO_DIR}
                --proto_path=${CONFIG_PROTO_GEN_DIR}
                --descriptor_set_out=${CONFIG_DESC_FILE}
                --include_imports
                ${CONFIG_PROTO_FILES}
        DEPENDS ${CONFIG_PROTO_FILES}
        COMMENT "Compiling config .proto files to descriptor set"
        VERBATIM
    )

    # Step 2: Generate C++ from descriptor set
    add_custom_command(
        OUTPUT  ${CONFIG_GENERATED_SOURCES}
        COMMAND ${Python3_EXECUTABLE} ${CODEGEN_TOOL}
                ${CONFIG_DESC_FILE}
                ${CONFIG_CODEGEN_DIR}
        DEPENDS ${CONFIG_DESC_FILE} ${CODEGEN_TOOL}
        COMMENT "Generating C++ config code from proto descriptors"
        VERBATIM
    )

    # Named target for manual regeneration: cmake --build . --target codegen_config
    add_custom_target(codegen_config
        DEPENDS ${CONFIG_GENERATED_SOURCES}
        COMMENT "Config codegen complete"
    )

    # Validation target: cmake --build . --target validate_config
    add_custom_target(validate_config
        COMMAND ${Python3_EXECUTABLE} ${VALIDATE_TOOL}
        DEPENDS ${CONFIG_GENERATED_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Validating generated config code against PrintConfig.cpp"
        VERBATIM
    )

    message(STATUS "Config codegen: enabled (protoc=${PROTOC_EXECUTABLE})")
else()
    if(NOT PROTOC_EXECUTABLE)
        message(STATUS "Config codegen: disabled (protoc not found, using checked-in generated files)")
    endif()
    if(NOT Python3_EXECUTABLE)
        message(STATUS "Config codegen: disabled (Python3 not found, using checked-in generated files)")
    endif()
endif()
