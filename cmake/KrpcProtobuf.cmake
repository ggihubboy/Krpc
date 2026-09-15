# Resolve protoc and generate C++ sources from .proto files with the
# official CMake Protobuf helpers.
#
# Ubuntu 24.04's protobuf CONFIG package leaves Protobuf_PROTOC_EXECUTABLE
# empty unless protobuf_MODULE_COMPATIBLE is ON. An empty compiler path
# makes the generate step fail at build time with make exit code 2.

function(krpc_resolve_protoc)
    if(KRPC_PROTOC_EXECUTABLE AND EXISTS "${KRPC_PROTOC_EXECUTABLE}")
        set(Protobuf_PROTOC_EXECUTABLE "${KRPC_PROTOC_EXECUTABLE}" CACHE FILEPATH
            "protoc used to generate C++ sources" FORCE)
        set(PROTOBUF_PROTOC_EXECUTABLE "${KRPC_PROTOC_EXECUTABLE}" CACHE FILEPATH
            "protoc used to generate C++ sources" FORCE)
        return()
    endif()

    set(_protoc "")
    if(Protobuf_PROTOC_EXECUTABLE AND EXISTS "${Protobuf_PROTOC_EXECUTABLE}")
        set(_protoc "${Protobuf_PROTOC_EXECUTABLE}")
    elseif(PROTOBUF_PROTOC_EXECUTABLE AND EXISTS "${PROTOBUF_PROTOC_EXECUTABLE}")
        set(_protoc "${PROTOBUF_PROTOC_EXECUTABLE}")
    endif()

    if(NOT _protoc AND TARGET protobuf::protoc)
        foreach(_cfg
                IMPORTED_LOCATION
                IMPORTED_LOCATION_RELEASE
                IMPORTED_LOCATION_RELWITHDEBINFO
                IMPORTED_LOCATION_MINSIZEREL
                IMPORTED_LOCATION_DEBUG)
            get_target_property(_loc protobuf::protoc ${_cfg})
            if(_loc AND EXISTS "${_loc}")
                set(_protoc "${_loc}")
                break()
            endif()
        endforeach()
    endif()

    if(NOT _protoc)
        find_program(_krpc_protoc_path NAMES protoc)
        if(_krpc_protoc_path AND EXISTS "${_krpc_protoc_path}")
            set(_protoc "${_krpc_protoc_path}")
        endif()
    endif()

    if(NOT _protoc)
        message(FATAL_ERROR
            "protoc was not found. Install protobuf-compiler, or pass "
            "-DKRPC_PROTOC_EXECUTABLE=/path/to/protoc")
    endif()

    execute_process(
        COMMAND "${_protoc}" --version
        OUTPUT_VARIABLE _protoc_version
        ERROR_VARIABLE _protoc_version_err
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE)
    set(_version_text "${_protoc_version}")
    if(NOT _version_text)
        set(_version_text "${_protoc_version_err}")
    endif()

    set(KRPC_PROTOC_EXECUTABLE "${_protoc}" CACHE FILEPATH
        "protoc used to generate C++ sources" FORCE)
    set(Protobuf_PROTOC_EXECUTABLE "${_protoc}" CACHE FILEPATH
        "protoc used to generate C++ sources" FORCE)
    set(PROTOBUF_PROTOC_EXECUTABLE "${_protoc}" CACHE FILEPATH
        "protoc used to generate C++ sources" FORCE)
    message(STATUS "Using protoc: ${KRPC_PROTOC_EXECUTABLE} (${_version_text})")
endfunction()

function(krpc_protobuf_generate_fallback cc_var h_var proto_file)
    get_filename_component(_abs "${proto_file}" ABSOLUTE)
    get_filename_component(_dir "${_abs}" DIRECTORY)
    get_filename_component(_name "${_abs}" NAME_WE)
    set(_cc "${CMAKE_CURRENT_BINARY_DIR}/${_name}.pb.cc")
    set(_h "${CMAKE_CURRENT_BINARY_DIR}/${_name}.pb.h")

    if(TARGET protobuf::protoc)
        set(_cmd protobuf::protoc)
    else()
        set(_cmd "${KRPC_PROTOC_EXECUTABLE}")
    endif()

    add_custom_command(
        OUTPUT "${_cc}" "${_h}"
        COMMAND ${_cmd}
                "--cpp_out=${CMAKE_CURRENT_BINARY_DIR}"
                "-I" "${_dir}"
                "${_abs}"
        DEPENDS "${_abs}"
        COMMENT "Generating ${_name}.pb.cc from ${_name}.proto"
        VERBATIM)
    set(${cc_var} "${_cc}" PARENT_SCOPE)
    set(${h_var} "${_h}" PARENT_SCOPE)
endfunction()

# krpc_add_protobuf_library(<target> <proto_file>)
# Creates a STATIC library from CMake's official protobuf C++ generator.
function(krpc_add_protobuf_library target proto_file)
    if(NOT KRPC_PROTOC_EXECUTABLE AND NOT Protobuf_PROTOC_EXECUTABLE)
        message(FATAL_ERROR "krpc_resolve_protoc() must be called first")
    endif()

    get_filename_component(_abs "${proto_file}" ABSOLUTE)
    set(PROTOBUF_GENERATE_CPP_APPEND_PATH ON)

    if(COMMAND protobuf_generate_cpp)
        protobuf_generate_cpp(_srcs _hdrs "${_abs}")
    else()
        krpc_protobuf_generate_fallback(_srcs _hdrs "${_abs}")
    endif()

    add_library(${target} STATIC ${_srcs})
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set_source_files_properties(${_srcs} PROPERTIES COMPILE_OPTIONS "-w")
        target_compile_options(${target} PRIVATE -w)
    endif()
    target_include_directories(${target} SYSTEM PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(${target} PUBLIC protobuf::libprotobuf)
    set_target_properties(${target} PROPERTIES
        KRPC_PROTO_HEADER "${_hdrs}"
        KRPC_PROTO_SOURCE "${_srcs}")
endfunction()

# Make source files wait for a generated protobuf header before compiling.
function(krpc_sources_depend_on_proto proto_target)
    get_target_property(_h ${proto_target} KRPC_PROTO_HEADER)
    if(NOT _h)
        message(FATAL_ERROR
            "Target ${proto_target} has no KRPC_PROTO_HEADER property")
    endif()
    set_source_files_properties(${ARGN} PROPERTIES OBJECT_DEPENDS "${_h}")
endfunction()
