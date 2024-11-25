#!/bin/bash

# CUDA-Q Build Script with Incremental Builds and Memory Optimizations
# Usage:
#   bash scripts/build_cudaq_incremental.sh [options]
# Options:
#   -c <Debug|Release>  Set build configuration (default: Release)
#   -t <toolchain>      Specify toolchain
#   -v                  Enable verbose output

CUDAQ_INSTALL_PREFIX=${CUDAQ_INSTALL_PREFIX:-"$HOME/.cudaq"}
build_configuration=${CMAKE_BUILD_TYPE:-Release}
verbose=false
install_toolchain=""
ninja_jobs=1  # Limit to 1 job to minimize memory usage

# Parse command-line arguments
__optind__=$OPTIND
OPTIND=1
while getopts ":c:t:v" opt; do
  case $opt in
    c) build_configuration="$OPTARG";;
    t) install_toolchain="$OPTARG";;
    v) verbose=true;;
    \?) echo "Invalid option -$OPTARG" >&2; exit 1;;
  esac
done
OPTIND=$__optind__

# Set up the working directory
working_dir=$(pwd)
this_file_dir=$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")
repo_root=$(cd "$this_file_dir" && git rev-parse --show-toplevel)
build_dir="$working_dir/build"

# Prepare build directories
mkdir -p "$CUDAQ_INSTALL_PREFIX/bin"
mkdir -p "$build_dir" && cd "$build_dir" && rm -rf *
mkdir -p logs && rm -rf logs/*

# Install prerequisites if requested
if [ -n "$install_toolchain" ]; then
  echo "Installing prerequisites..."
  if $verbose; then
    source "$this_file_dir/install_prerequisites.sh" -t "$install_toolchain"
  else
    source "$this_file_dir/install_prerequisites.sh" -t "$install_toolchain" \
      2> logs/prereqs_error.txt 1> logs/prereqs_output.txt
  fi
fi

# Detect CUDA installation
cuda_driver=${CUDACXX:-${CUDA_HOME:-/usr/local/cuda}/bin/nvcc}
cuda_version=$("$cuda_driver" --version 2>/dev/null | grep -o 'release [0-9]*\.[0-9]*' | cut -d ' ' -f 2)
cuda_major=$(echo $cuda_version | cut -d '.' -f 1)
cuda_minor=$(echo $cuda_version | cut -d '.' -f 2)

if [ "$cuda_version" = "" ] || [ "$cuda_major" -lt "11" ] || ([ "$cuda_minor" -lt "8" ] && [ "$cuda_major" -eq "11" ]); then
  echo "CUDA version requirement not satisfied (required: >= 11.8, got: $cuda_version)."
  unset cuda_driver
fi

# Configure build with memory optimizations
echo "Configuring CUDA-Q build..."
cmake_args="-G Ninja \
  -DCMAKE_INSTALL_PREFIX=$CUDAQ_INSTALL_PREFIX \
  -DCMAKE_BUILD_TYPE=$build_configuration \
  -DCMAKE_CUDA_COMPILER=$cuda_driver \
  -DCMAKE_CUDA_FLAGS='-O1 -g0' \
  -DCMAKE_CXX_FLAGS='-O1 -g0' \
  -DCMAKE_COMPILE_WARNING_AS_ERROR=OFF \
  -DCMAKE_JOB_POOLS:STRING=compile=$ninja_jobs \
  -DCMAKE_JOB_POOL_COMPILE:STRING=compile"

if $verbose; then
  echo $cmake_args | xargs cmake "$repo_root"
else
  echo $cmake_args | xargs cmake "$repo_root" \
    2> logs/cmake_error.txt 1> logs/cmake_output.txt
fi

# List of targets to build incrementally
targets=(
  cudaq-mlir-runtime
  rest-remote-platform-server
  rest-remote-platform-client
  cudaq-rest-qpu
  obj.cudaq-mlirgen
  # Add more targets as needed
)

# Build each target incrementally
echo "Building CUDA-Q incrementally..."
logs_dir="$build_dir/logs"

for target in "${targets[@]}"; do
  echo "Building target: $target..."
  if $verbose; then
    ninja "$target"
  else
    ninja "$target" -j$ninja_jobs \
      2> "$logs_dir/ninja_error_$target.txt" \
      1> "$logs_dir/ninja_output_$target.txt"
  fi

  if [ $? -ne 0 ]; then
    echo -e "\e[01;31mError: Failed to build $target. Check logs in $logs_dir.\e[0m" >&2
    exit 1
  fi
done

# Install the build
echo "Installing CUDA-Q..."
if $verbose; then
  ninja install
else
  ninja install -j$ninja_jobs \
    2> "$logs_dir/ninja_error_install.txt" \
    1> "$logs_dir/ninja_output_install.txt"
fi

if [ $? -ne 0 ]; then
  echo -e "\e[01;31mError: Installation failed. Check logs in $logs_dir.\e[0m" >&2
  exit 1
fi

# Copy additional files to install directory
cp "$repo_root/LICENSE" "$CUDAQ_INSTALL_PREFIX/LICENSE"
cp "$repo_root/NOTICE" "$CUDAQ_INSTALL_PREFIX/NOTICE"
cp "$repo_root/scripts/cudaq_set_env.sh" "$CUDAQ_INSTALL_PREFIX/set_env.sh"

# Save build configuration
cat <<EOF > "$CUDAQ_INSTALL_PREFIX/build_config.xml"
<build_config>
  <LLVM_INSTALL_PREFIX>$LLVM_INSTALL_PREFIX</LLVM_INSTALL_PREFIX>
  <CUQUANTUM_INSTALL_PREFIX>$CUQUANTUM_INSTALL_PREFIX</CUQUANTUM_INSTALL_PREFIX>
  <CUTENSOR_INSTALL_PREFIX>$CUTENSOR_INSTALL_PREFIX</CUTENSOR_INSTALL_PREFIX>
</build_config>
EOF

echo "CUDA-Q installed successfully in: $CUDAQ_INSTALL_PREFIX"
