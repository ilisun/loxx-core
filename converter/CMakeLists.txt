cmake_minimum_required(VERSION 3.20)

project(converter LANGUAGES CXX)

find_package(SQLite3 REQUIRED)

# libosmium/protozero headers (Homebrew installs headers only)
find_path(OSMIUM_INCLUDE_DIR osmium/io/any_input.hpp
  PATHS /opt/homebrew/include /usr/local/include)
find_path(PROTOZERO_INCLUDE_DIR protozero/byteswap.hpp
  PATHS /opt/homebrew/include /usr/local/include)

# FlatBuffers codegen (requires flatc available at configure/build time)
find_program(FLATC_EXECUTABLE NAMES flatc)
if(NOT FLATC_EXECUTABLE)
  message(WARNING "flatc not found. FlatBuffers code will not be generated now.")
endif()

set(FBS_SCHEMA ${CMAKE_CURRENT_SOURCE_DIR}/src/land_tile.fbs)
set(GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GENERATED_DIR})

if(FLATC_EXECUTABLE)
  add_custom_command(
    OUTPUT ${GENERATED_DIR}/land_tile_generated.h
    COMMAND ${FLATC_EXECUTABLE} --cpp --scoped-enums -o ${GENERATED_DIR} ${FBS_SCHEMA}
    DEPENDS ${FBS_SCHEMA}
    COMMENT "Generating FlatBuffers C++ code from land_tile.fbs"
  )
  add_custom_target(generate_flatbuffers ALL DEPENDS ${GENERATED_DIR}/land_tile_generated.h)
endif()

add_executable(converter
  src/main.cpp
  src/sqlite_writer.cpp
  src/pbf_reader.cpp
  src/serializer.cpp
)

target_include_directories(converter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${GENERATED_DIR})
if(FLATC_EXECUTABLE)
  # flatbuffers headers are needed; assume system-installed
  find_path(FLATBUFFERS_INCLUDE_DIR flatbuffers/flatbuffers.h)
  if(FLATBUFFERS_INCLUDE_DIR)
    target_include_directories(converter PRIVATE ${FLATBUFFERS_INCLUDE_DIR})
  endif()
endif()
if(OSMIUM_INCLUDE_DIR)
  target_include_directories(converter PRIVATE ${OSMIUM_INCLUDE_DIR})
  target_compile_definitions(converter PRIVATE HAVE_LIBOSMIUM=1)
endif()
if(PROTOZERO_INCLUDE_DIR)
  target_include_directories(converter PRIVATE ${PROTOZERO_INCLUDE_DIR})
endif()

# Link SQLite with compatibility for environments where imported target is missing
if(TARGET SQLite::SQLite3)
  target_link_libraries(converter PRIVATE SQLite::SQLite3)
else()
  target_include_directories(converter PRIVATE ${SQLite3_INCLUDE_DIRS})
  target_link_libraries(converter PRIVATE ${SQLite3_LIBRARIES})
endif()

# Compression libs commonly used by libosmium for PBF
find_library(ZLIB_LIB z)
if(ZLIB_LIB)
  target_link_libraries(converter PRIVATE ${ZLIB_LIB})
endif()
find_library(BZ2_LIB bz2)
if(BZ2_LIB)
  target_link_libraries(converter PRIVATE ${BZ2_LIB})
endif()
find_library(LZ4_LIB lz4)
if(LZ4_LIB)
  target_link_libraries(converter PRIVATE ${LZ4_LIB})
endif()
find_library(ZSTD_LIB zstd)
if(ZSTD_LIB)
  target_link_libraries(converter PRIVATE ${ZSTD_LIB})
endif()

# Expat (XML) is pulled by libosmium headers; needed at link time even for PBF builds
find_library(EXPAT_LIB expat)
if(EXPAT_LIB)
  target_link_libraries(converter PRIVATE ${EXPAT_LIB})
endif()

if(TARGET generate_flatbuffers)
  add_dependencies(converter generate_flatbuffers)
endif()

if(APPLE)
  # Nothing special; SQLite is provided by macOS SDK
endif()


