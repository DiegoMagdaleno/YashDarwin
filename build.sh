#! /bin/sh

Build() {
    export MACOSX_DEPLOYMENT_TARGET="10.4"

    export CC=$(xcrun --find --sdk "${SDK}" clang)
    export CXX=$(xcrun --find --sdk "${SDK}" clang++)
    export CPP=$(xcrun --find --sdk "${SDK}" cpp)
    export CFLAGS="${HOST_FLAGS} ${OPT_FLAGS}"
    export CXXFLAGS="${HOST_FLAGS} ${OPT_FLAGS}"
    export LDFLAGS="${HOST_FLAGS}"

    EXEC_PREFIX="${PLATFORMS}/${PLATFORM}"
    ./configure \
        --host="${CHOST}" \
        --prefix="${PREFIX}" \
        --exec-prefix="${EXEC_PREFIX}" \
        --enable-static \
        --disable-shared

    make clean
    mkdir -p "${PLATFORMS}" &> /dev/null
    make V=1 -j"${MAKE_JOBS}" --debug=j
    make install
}

ScriptDir="$( cd "$( dirname "$0" )" && pwd )"
cd - &> /dev/null
PREFIX="${ScriptDir}"/_build
PLATFORMS="${PREFIX}"/platforms
UNIVERSAL="${PREFIX}"/universal


# Compiler options
OPT_FLAGS="-O3 -g3 -fembed-bitcode"
MAKE_JOBS=8
MIN_IOS_VERSION=8.0

# Build for platforms
SDK="iphoneos"
PLATFORM="arm"
PLATFORM_ARM=${PLATFORM}
ARCH_FLAGS="-arch arm64 -arch arm64e"  # -arch armv7 -arch armv7s
HOST_FLAGS="${ARCH_FLAGS} -miphoneos-version-min=${MIN_IOS_VERSION} -isysroot $(xcrun --sdk ${SDK} --show-sdk-path)"
CHOST="arm-apple-darwin"
Build

SDK="iphonesimulator"
PLATFORM="x86_64-sim"
PLATFORM_ISIM=${PLATFORM}
ARCH_FLAGS="-arch x86_64"
HOST_FLAGS="${ARCH_FLAGS} -mios-simulator-version-min=${MIN_IOS_VERSION} -isysroot $(xcrun --sdk ${SDK} --show-sdk-path)"
CHOST="x86_64-apple-darwin"
Build

# Create universal binary
cd "${PLATFORMS}/${PLATFORM_ARM}/lib"
LIB_NAME=`find . -iname *.a`
cd -
mkdir -p "${UNIVERSAL}" &> /dev/null
lipo -create -output "${UNIVERSAL}/${LIB_NAME}" "${PLATFORMS}/${PLATFORM_ARM}/lib/${LIB_NAME}" "${PLATFORMS}/${PLATFORM_ISIM}/lib/${LIB_NAME}"