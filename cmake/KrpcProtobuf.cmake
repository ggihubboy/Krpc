# Resolve protoc and generate C++ sources from .proto files.
#
# Ubuntu 24.04's protobuf CMake CONFIG package may not set
# Protobuf_PROTOC_EXECUTABLE. CI previously failed at build time when the
# custom command ran an empty compiler path, or compiled .cc files before
# the generated headers existed. This helper:
#   1. Finds protoc from CMake variables, protobuf::protoc, or PATH
#   2. Generates sources at configure time so the first compile already has headers
#   3. Keeps an add_custom_command so editing a .proto still rebuilds

function(krpc_resolve_protoc)
    if(KRPC_PROTOC_EXECUTABLE AND EXISTS "${KRPC_PROTOC_EXECUTABLE}")
        return()
    endif()

    set(_protoc "")
    if(Protobuf_PROTOC_EXECUTABLE)
        set(_protoc "${Protobuf_PROTOC_EXECUTABLE}")
    elseif(PROTOBUF_PROTOC_EXECUTABLE)
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
            if(_loc)
                set(_protoc "${_loc}")
                break()
            endif()
        endforeach()
    endif()

    if(NOT _protoc)
        find_program(_krpc_protoc_path NAMES protoc)
        set(_protoc "${_krpc_protoc_path}")
    endif()

    if(NOT _protoc)
        message(FATAL_ERROR
            "protoc was not found. Install protobuf-compiler, or pass "
            "-DKRPC_PROTOC_EXECUTABLE=/path/to/protoc")
    endif()

    if(NOT EXISTS "${_protoc}")
        message(FATAL_ERROR "protoc path does not exist: ${_protoc}")
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
    message(STATUS "Using protoc: ${KRPC_PROTOC_EXECUTABLE} (${_version_text})")
endfunction()

# krpc_add_protobuf_library(<target> <proto_file>)
# Creates a STATIC library from the generated .pb.cc / .pb.h.
function(krpc_add_protobuf_library target proto_file)
    if(NOT KRPC_PROTOC_EXECUTABLE)
        message(FATAL_ERROR "krpc_resolve_protoc() must be called first")
    endif()

    get_filename_component(_abs "${proto_file}" ABSOLUTE)
    get_filename_component(_dir "${_abs}" DIRECTORY)
    get_filename_component(_name "${_abs}" NAME_WE)
    set(_gen "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${_gen}")
    set(_cc "${_gen}/${_name}.pb.cc")
    set(_h "${_gen}/${_name}.pb.h")

    execute_process(
        COMMAND "${KRPC_PROTOC_EXECUTABLE}"
                "--cpp_out=${_gen}"
                "-I${_dir}"
                "${_abs}"
        RESULT_VARIABLE _krpc_protoc_rc
        ERROR_VARIABLE _krpc_protoc_err
        OUTPUT_VARIABLE _krpc_protoc_out)
    if(NOT _krpc_protoc_rc EQUAL 0)
        message(FATAL_ERROR
            "protoc failed to generate ${_name}.pb.cc "
            "(exit ${_krpc_protoc_rc}): ${_krpc_protoc_err}${_krpc_protoc_out}")
    endif()
    if(NOT EXISTS "${_cc}" OR NOT EXISTS "${_h}")
        message(FATAL_ERROR
            "protoc ran but did not write ${_cc} and ${_h}")
    endif()

    add_custom_command(
        OUTPUT "${_cc}" "${_h}"
        COMMAND "${KRPC_PROTOC_EXECUTABLE}"
                "--cpp_out=${_gen}"
                "-I${_dir}"
                "${_abs}"
        DEPENDS "${_abs}"
        COMMENT "Generating ${_name}.pb.cc from ${_name}.proto"
        VERBATIM)

    add_library(${target} STATIC "${_cc}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set_source_files_properties("${_cc}" PROPERTIES COMPILE_OPTIONS "-w")
    endif()
    target_include_directories(${target} SYSTEM PUBLIC "${_gen}")
    target_link_libraries(${target} PUBLIC
        krpc_project_options
        protobuf::libprotobuf)
    set_target_properties(${target} PROPERTIES
        KRPC_PROTO_HEADER "${_h}"
        KRPC_PROTO_SOURCE "${_cc}")
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
