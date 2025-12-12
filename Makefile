all:
	if [ ! -d build ]; then cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -Wno-dev; fi
	cmake --build build -- -j $(nproc) fast unit_tests

run:
	./build/fast plans.json