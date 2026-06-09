cmake -S . -B build -DBUILD_DEPS=ON
#allows paralellism in build
cmake --build build -j2
./build/main