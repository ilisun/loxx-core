#!/bin/bash
set -e

# loxx-core iOS XCFramework Builder
# Собирает C++ ядро в XCFramework для iOS

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$SCRIPT_DIR/.."
BUILD_DIR="$PROJECT_ROOT/build-xcframework"
OUTPUT_DIR="$PROJECT_ROOT/releases"

echo "🔨 Building loxx-core XCFramework..."

# Очистка
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
mkdir -p "$OUTPUT_DIR"

# ============================================
# 1. Создать Xcode проект для C++ фреймворка
# ============================================
echo "📦 Creating Xcode project..."

cat > "$BUILD_DIR/CMakeLists.txt" << 'EOF'
cmake_minimum_required(VERSION 3.20)
project(LoxxRouterCore VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Framework настройки
set(CMAKE_FRAMEWORK TRUE)
set(CMAKE_MACOSX_RPATH TRUE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)

# ============================================
# FlatBuffers - генерация заголовков
# ============================================
find_program(FLATC_EXECUTABLE NAMES flatc)
if(NOT FLATC_EXECUTABLE)
    message(FATAL_ERROR "flatc not found! Install FlatBuffers: brew install flatbuffers")
endif()

set(GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(FBS_SCHEMA ${CMAKE_CURRENT_SOURCE_DIR}/../converter/src/land_tile.fbs)

file(MAKE_DIRECTORY ${GENERATED_DIR})

add_custom_command(
    OUTPUT ${GENERATED_DIR}/land_tile_generated.h
    COMMAND ${FLATC_EXECUTABLE} --cpp --scoped-enums -o ${GENERATED_DIR} ${FBS_SCHEMA}
    DEPENDS ${FBS_SCHEMA}
    COMMENT "Generating FlatBuffers headers for iOS XCFramework"
)

add_custom_target(generate_fbs ALL DEPENDS ${GENERATED_DIR}/land_tile_generated.h)

# ============================================
# loxx-core Framework
# ============================================
# Добавить исходники C++ ядра
file(GLOB_RECURSE SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/../core/src/*.cpp"
)

# Добавить Objective-C++ bridge
list(APPEND SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../bridge/LoxxRouterBridge.mm")

# Создать framework
add_library(LoxxRouterCore SHARED ${SOURCES})

# FlatBuffers include directories
# Попробуем найти через brew
execute_process(
    COMMAND brew --prefix flatbuffers
    OUTPUT_VARIABLE FLATBUFFERS_PREFIX
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

if(FLATBUFFERS_PREFIX)
    set(FLATBUFFERS_INCLUDE_DIR "${FLATBUFFERS_PREFIX}/include")
    message(STATUS "Found FlatBuffers via brew: ${FLATBUFFERS_INCLUDE_DIR}")
else()
    # Fallback: поиск в стандартных путях
    find_path(FLATBUFFERS_INCLUDE_DIR flatbuffers/flatbuffers.h
              PATHS /opt/homebrew/include /usr/local/include /usr/include)
    
    if(NOT FLATBUFFERS_INCLUDE_DIR)
        message(FATAL_ERROR "FlatBuffers headers not found! Install with: brew install flatbuffers")
    endif()
endif()

target_include_directories(LoxxRouterCore
    PUBLIC 
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../core/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../bridge/include>
        $<INSTALL_INTERFACE:Headers>
    PRIVATE
        ${GENERATED_DIR}
        ${FLATBUFFERS_INCLUDE_DIR}
)

# Зависимость от генерации FlatBuffers
add_dependencies(LoxxRouterCore generate_fbs)

# Собрать все публичные заголовки
file(GLOB ROUTING_CORE_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/../core/include/routing_core/*.h")

# Добавить сгенерированный FlatBuffers заголовок в корень Headers
set(GENERATED_HEADER "${GENERATED_DIR}/land_tile_generated.h")

# Скопировать сгенерированный заголовок в include для публичного доступа
add_custom_command(
    TARGET LoxxRouterCore POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${GENERATED_HEADER}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../core/include/land_tile_generated.h"
    DEPENDS "${GENERATED_HEADER}"
    COMMENT "Copying generated header to public includes"
)

# ТОЛЬКО bridge header публичный для Swift
# routing_core/*.h и land_tile_generated.h НЕ публичные - используются только при компиляции C++
set(PUBLIC_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/../bridge/include/LoxxRouterBridge.h")

# Отладка: показать все PUBLIC_HEADERS
message(STATUS "PUBLIC_HEADERS count: ${CMAKE_MATCH_COUNT}")
foreach(header ${PUBLIC_HEADERS})
    message(STATUS "  - ${header}")
endforeach()

# Framework свойства
set_target_properties(LoxxRouterCore PROPERTIES
    FRAMEWORK TRUE
    FRAMEWORK_VERSION A
    MACOSX_FRAMEWORK_IDENTIFIER com.loxx.LoxxRouterCore
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    PUBLIC_HEADER "${PUBLIC_HEADERS}"
    XCODE_ATTRIBUTE_INSTALL_PATH "@rpath"
)

# SQLite линковка
find_package(SQLite3 REQUIRED)
target_link_libraries(LoxxRouterCore PRIVATE SQLite::SQLite3)

# CoreLocation и Foundation для Objective-C++ bridge
target_link_libraries(LoxxRouterCore PUBLIC "-framework Foundation" "-framework CoreLocation")

install(TARGETS LoxxRouterCore
    FRAMEWORK DESTINATION .
)
EOF

# ============================================
# Helper Function: Build framework for platform
# ============================================
build_framework_for_platform() {
    local PLATFORM_NAME=$1       # e.g. "ios-arm64" or "ios-simulator"
    local DISPLAY_NAME=$2        # e.g. "iOS Device (arm64)"
    local ARCHITECTURES=$3       # e.g. "arm64" or "arm64;x86_64"
    local SDK_NAME=$4            # e.g. "iphoneos" or "iphonesimulator"
    local EMOJI=$5               # e.g. "📱" or "🖥️"
    
    local BUILD_PATH="$BUILD_DIR/$PLATFORM_NAME"
    local INSTALL_PATH="$BUILD_DIR/install/$PLATFORM_NAME"
    local FRAMEWORK_PATH="$INSTALL_PATH/LoxxRouterCore.framework"
    
    echo "$EMOJI Building for $DISPLAY_NAME..."
    
    # Configure with CMake
    cmake -S "$BUILD_DIR" -B "$BUILD_PATH" \
        -G Xcode \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_ARCHITECTURES="$ARCHITECTURES" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
        -DCMAKE_OSX_SYSROOT="$SDK_NAME" \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_PATH" \
        -DCMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH=NO
    
    # Build and install
    cmake --build "$BUILD_PATH" --config Release -- -j $(sysctl -n hw.ncpu)
    cmake --install "$BUILD_PATH" --config Release
    
    # Fix header structure
    echo "📂 Fixing header structure for $DISPLAY_NAME..."
    
    # Check if CMake installed headers
    echo "🔍 Debug: Headers installed by CMake:"
    if [ -d "$FRAMEWORK_PATH/Headers/" ]; then
        ls -la "$FRAMEWORK_PATH/Headers/"
    else
        echo "⚠️  Headers directory not created by CMake - will create manually"
        
        # Create Headers directory
        mkdir -p "$FRAMEWORK_PATH/Headers"
        
        # Copy headers manually
        echo "📋 Manually copying headers..."
        
        # Only copy LoxxRouterBridge.h (public header for Swift)
        # routing_core/*.h and land_tile_generated.h are NOT copied - C++ only
        if [ -f "$SCRIPT_DIR/../bridge/include/LoxxRouterBridge.h" ]; then
            cp "$SCRIPT_DIR/../bridge/include/LoxxRouterBridge.h" "$FRAMEWORK_PATH/Headers/"
            echo "  ✅ Copied LoxxRouterBridge.h"
        else
            echo "  ❌ LoxxRouterBridge.h not found!"
            exit 1
        fi
        
        echo "📋 Headers after manual copy:"
        ls -la "$FRAMEWORK_PATH/Headers/"
    fi
    
    # Verify critical headers
    if [ ! -f "$FRAMEWORK_PATH/Headers/LoxxRouterBridge.h" ]; then
        echo "❌ ERROR: LoxxRouterBridge.h not found after CMake install!"
        echo "   Available headers:"
        find "$FRAMEWORK_PATH/Headers" -name "*.h" -type f
        exit 1
    fi
    
    echo "✅ All required headers present"
    
    # Create Modules directory
    mkdir -p "$FRAMEWORK_PATH/Modules"
    
    # Log public headers
    echo "✅ Public headers ($DISPLAY_NAME):"
    echo "   - Headers/LoxxRouterBridge.h (Swift accessible)"
    echo "   - C++ headers (routing_core/*.h) not included - only needed during compilation"
    
    # Create module.modulemap
    cat > "$FRAMEWORK_PATH/Modules/module.modulemap" << 'MODULEMAP'
framework module LoxxRouterCore {
    umbrella header "loxx-core.h"
    export *
    module * { export * }
}
MODULEMAP
    
    # Create umbrella header
    cat > "$FRAMEWORK_PATH/Headers/loxx-core.h" << 'UMBRELLA'
// Umbrella header for loxx-core framework
#import <Foundation/Foundation.h>

// Objective-C++ Bridge (Swift-accessible)
#import "LoxxRouterBridge.h"

// Note: C++ headers (routing_core/*.h) and FlatBuffers headers (land_tile_generated.h)
// are available in Headers/ but not exposed to Swift. They are used only during compilation.
// The bridge is already compiled and does not require FlatBuffers at runtime.
UMBRELLA
    
    echo "✅ Framework built successfully: $FRAMEWORK_PATH"
    echo ""
}

# ============================================
# 2. Сборка для iOS Device (arm64)
# ============================================
build_framework_for_platform "ios-arm64" "iOS Device (arm64)" "arm64" "iphoneos" "📱"

# ============================================
# 3. Сборка для iOS Simulator (arm64 + x86_64)
# ============================================
build_framework_for_platform "ios-simulator" "iOS Simulator (arm64 + x86_64)" "arm64;x86_64" "iphonesimulator" "🖥️"

# ============================================
# 4. Создать XCFramework
# ============================================
echo "📦 Creating XCFramework..."

xcodebuild -create-xcframework \
    -framework "$BUILD_DIR/install/ios-arm64/LoxxRouterCore.framework" \
    -framework "$BUILD_DIR/install/ios-simulator/LoxxRouterCore.framework" \
    -output "$OUTPUT_DIR/LoxxRouterCore.xcframework"

echo ""
echo "🔍 Verifying XCFramework structure..."
echo ""
echo "📋 All headers in XCFramework:"
find "$OUTPUT_DIR/LoxxRouterCore.xcframework" -name "*.h" -type f | sort

echo ""
echo "✅ Critical files check:"
DEVICE_FRAMEWORK="$OUTPUT_DIR/LoxxRouterCore.xcframework/ios-arm64/LoxxRouterCore.framework"
if [ -f "$DEVICE_FRAMEWORK/Headers/LoxxRouterBridge.h" ]; then
    echo "   ✅ LoxxRouterBridge.h present"
else
    echo "   ❌ LoxxRouterBridge.h MISSING!"
    exit 1
fi

if [ -f "$DEVICE_FRAMEWORK/Headers/loxx-core.h" ]; then
    echo "   ✅ loxx-core.h (umbrella) present"
else
    echo "   ❌ loxx-core.h MISSING!"
    exit 1
fi

if [ -f "$DEVICE_FRAMEWORK/Modules/module.modulemap" ]; then
    echo "   ✅ module.modulemap present"
else
    echo "   ❌ module.modulemap MISSING!"
    exit 1
fi

echo ""

# ============================================
# 5. Упаковать в ZIP
# ============================================
echo "🗜️  Creating ZIP archive..."

cd "$OUTPUT_DIR"
zip -r LoxxRouterCore.xcframework.zip LoxxRouterCore.xcframework

# ============================================
# 6. Вычислить checksum
# ============================================
echo "🔐 Computing checksum..."

CHECKSUM=$(swift package compute-checksum LoxxRouterCore.xcframework.zip)

echo ""
echo "✅ Build complete!"
echo ""
echo "📦 Output:"
echo "   Framework: $OUTPUT_DIR/LoxxRouterCore.xcframework"
echo "   Archive:   $OUTPUT_DIR/LoxxRouterCore.xcframework.zip"
echo ""
echo "🔐 Checksum for Package.swift:"
echo "   $CHECKSUM"
echo ""
echo "📝 Next steps:"
echo "   1. Upload LoxxRouterCore.xcframework.zip to GitHub Release"
echo "   2. Use checksum in loxx-router-ios Package.swift"

