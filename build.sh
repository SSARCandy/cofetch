#!/bin/bash

PROGNAME=${0##*/}

CURDIR="$( cd "$(dirname "$0")" ; pwd -P )"
BUILD_TYPE="Release"
BUILD_DIR="build"
TEST="False"

if which nproc; then
    # Linux
    CORES="$(nproc --all)"
elif which sysctl; then
    # MacOS
    CORES="$(sysctl -n hw.logicalcpu)"
else
    CORES=2
fi
JOBS=$(( (CORES + 1) / 2 ))
usage() {

cat <<EOF

  Usage: $PROGNAME [options]

  Options:

    -h, --help        Display this help and exit
    -c, --clean       Clean build
    -d, --debug       Build with debug mode
    -j, --jobs        Use N cores to build
    -t, --tetst       Build Debug and run tests

EOF
}

clean() {
    rm -rf "$BUILD_DIR"
}

test() {
    ECHO_PORT=18089
    python3 "${CURDIR}/test/echo_server.py" --port "${ECHO_PORT}" &
    ECHO_PID=$!
    trap 'kill ${ECHO_PID} 2>/dev/null' EXIT
    for _ in $(seq 1 50); do
        curl -s -o /dev/null "http://127.0.0.1:${ECHO_PORT}/get" && break
        sleep 0.1
    done

    cd ./build/test;
    if ! COFETCH_ECHO="http://127.0.0.1:${ECHO_PORT}" ./all_test; then
        cd $CURDIR
        exit 1
    fi
    cd $CURDIR;

    # lcov 2.x hard-errors on line inconsistencies from gcc-14 coroutine code
    LCOV_OPTS="--ignore-errors inconsistent,mismatch"

    lcov \
        --capture ${LCOV_OPTS} \
        --directory build/test/ \
        --output-file coverage.info \
        --test-name coverageHtml > /dev/null

    lcov ${LCOV_OPTS} -o coverage.info --extract coverage.info "${CURDIR}/http/*" > /dev/null
    genhtml --ignore-errors inconsistent -o .coverage coverage.info
}


while (( "$#" )); do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        -j|--jobs)
            JOBS="$2"
            shift 2
            ;;
        -d|--debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        -t|--test)
            BUILD_TYPE="Debug"
            TEST="True"
            shift
            ;;
        -c|--clean)
            clean
            shift
            ;;
        -*|--*=)
            echo "Invalid arguments"
            exit 1
            ;;
        *)
            break
            ;;
    esac
done

BUILD_OPTIONS="-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
if [ "${BUILD_TYPE}" == "Debug" ]; then
    BUILD_OPTIONS="${BUILD_OPTIONS} -DCOFETCH_BUILD_TESTS=ON"
fi
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"
if cmake .. ${BUILD_OPTIONS}; then
     if ! make -j "${JOBS}"; then
         exit 1
     fi
     cd "$CURDIR"
    
    if [ "${TEST}" == "True" ]; then
        test
        cd "$CURDIR"
    fi
else
    cd "$CURDIR"
    exit 1
fi
