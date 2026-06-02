.PHONY: build clean run docker

build:
	cmake -S . -B build
	cmake --build build -j

run: build
	./build/cc-oai-gateway

clean:
	rm -rf build

docker:
	docker build -t cc-oai-gateway:latest .
