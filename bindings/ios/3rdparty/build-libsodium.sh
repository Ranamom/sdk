#!/bin/sh

LIBSODIUM_VERSION="1.0.16"
SDKVERSION=`xcrun -sdk iphoneos --show-sdk-version`

##############################################
CURRENTPATH=`pwd`
ARCHS="x86_64 arm64 arm64-simulator"
DEVELOPER=`xcode-select -print-path`

CORES=$(sysctl -n hw.ncpu)

# Formating
green="\033[32m"
bold="\033[0m${green}\033[1m"
normal="\033[0m"

if [ ! -d "$DEVELOPER" ]; then
  echo "xcode path is not set correctly $DEVELOPER does not exist (most likely because of xcode > 4.3)"
  echo "run"
  echo "sudo xcode-select -switch <xcode path>"
  echo "for default installation:"
  echo "sudo xcode-select -switch /Applications/Xcode.app/Contents/Developer"
  exit 1
fi

case $DEVELOPER in
     *\ * )
           echo "Your Xcode path contains whitespaces, which is not supported."
           exit 1
          ;;
esac

case $CURRENTPATH in
     *\ * )
           echo "Your path contains whitespaces, which is not supported by 'make install'."
           exit 1
          ;;
esac

set -e

if [ ! -e "libsodium-${LIBSODIUM_VERSION}.tar.gz" ]
then
curl -LO "https://github.com/jedisct1/libsodium/releases/download/${LIBSODIUM_VERSION}/libsodium-${LIBSODIUM_VERSION}.tar.gz"
fi

for ARCH in ${ARCHS}
do
if [[ "${ARCH}" == "x86_64" || "${ARCH}" == "arm64-simulator" ]];
then
PLATFORM="iPhoneSimulator"
if [ "${ARCH}" == "arm64-simulator" ];
then
ARCH="arm64"
fi
else
PLATFORM="iPhoneOS"
fi

rm -rf libsodium-${LIBSODIUM_VERSION}
tar zxf libsodium-${LIBSODIUM_VERSION}.tar.gz
pushd "libsodium-${LIBSODIUM_VERSION}"


echo "${bold}Building sodium for $PLATFORM $ARCH ${normal}"

export BUILD_TOOLS="${DEVELOPER}"
export BUILD_DEVROOT="${DEVELOPER}/Platforms/${PLATFORM}.platform/Developer"
export BUILD_SDKROOT="${BUILD_DEVROOT}/SDKs/${PLATFORM}${SDKVERSION}.sdk"

RUNTARGET=""
if [[ "${ARCH}" == "arm64"  && "$PLATFORM" == "iPhoneSimulator" ]];
then
RUNTARGET="-target ${ARCH}-apple-ios13.0-simulator"
fi

export CC="${BUILD_TOOLS}/usr/bin/gcc -arch ${ARCH}"
mkdir -p "${CURRENTPATH}/bin/${PLATFORM}${SDKVERSION}-${ARCH}.sdk"

# Build
export LDFLAGS="-Os -arch ${ARCH} -Wl,-dead_strip -miphoneos-version-min=13.0 -L${BUILD_SDKROOT}/usr/lib"
export CFLAGS="-Os -arch ${ARCH} -pipe -no-cpp-precomp -isysroot ${BUILD_SDKROOT} -miphoneos-version-min=13.0 -DNDEBUG ${RUNTARGET}"
export CPPFLAGS="${CFLAGS} -I${BUILD_SDKROOT}/usr/include"
export CXXFLAGS="${CPPFLAGS}"

if [ "${ARCH}" == "arm64" ]; then
./configure --host=arm-apple-darwin --disable-shared --enable-minimal
else
./configure --host=${ARCH}-apple-darwin --disable-shared --enable-minimal
fi

make -j${CORES}

cp -f src/libsodium/.libs/libsodium.a ${CURRENTPATH}/bin/${PLATFORM}${SDKVERSION}-${ARCH}.sdk/

popd

done


mkdir lib || true

lipo -create ${CURRENTPATH}/bin/iPhoneSimulator${SDKVERSION}-x86_64.sdk/libsodium.a ${CURRENTPATH}/bin/iPhoneSimulator${SDKVERSION}-arm64.sdk/libsodium.a -output ${CURRENTPATH}/bin/libsodium.a

echo "${bold}Creating xcframework ${normal}"

xcodebuild -create-xcframework -library ${CURRENTPATH}/bin/libsodium.a -library ${CURRENTPATH}/bin/iPhoneOS${SDKVERSION}-arm64.sdk/libsodium.a -output ${CURRENTPATH}/xcframework/libsodium.xcframework

cp -fR libsodium-${LIBSODIUM_VERSION}/src/libsodium/include/sodium* include
 
echo "${bold}Cleaning up ${normal}"

rm -rf bin
rm -rf libsodium-${LIBSODIUM_VERSION}
rm -rf libsodium-${LIBSODIUM_VERSION}.tar.gz

echo "Done."

