#!/bin/bash

# Machnet build script for development
# This script builds both release and debug versions of the project

set -e

echo "Building Machnet..."

# Update submodules
echo "Updating submodules..."
git submodule update --init --recursive

# Refresh library paths (use sudo since we're not root)
echo "Refreshing library paths..."
sudo ldconfig

# Build Release version
echo "Building Release version..."
mkdir -p release_build
cd release_build
cmake -DCMAKE_BUILD_TYPE=Release -GNinja ../
ninja
cd ..

# Build Debug version
echo "Building Debug version..."
mkdir -p debug_build
cd debug_build
cmake -DCMAKE_BUILD_TYPE=Debug -GNinja ../
ninja
cd ..

echo "Build completed successfully!"
echo "Release build: ./release_build/"
echo "Debug build: ./debug_build/" 