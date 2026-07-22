# cws_example_app

Reference consumer of the `cws` HTTP library via a git submodule.

## Layout

```
.
├── CMakeLists.txt
├── main.c
└── third_party/cws/      # git submodule (this repo's companion)
```

## Build & run

```bash
# 1. get sources + submodules
git clone --recurse-submodules <this-repo-url> cws_example_app
cd cws_example_app

# 2. configure + build
cmake -S . -B build
cmake --build build -j

# 3. run
./build/cws_example_app
```

The server listens on `:8181`. Try:

```bash
curl http://127.0.0.1:8181/
curl http://127.0.0.1:8181/healthz
curl http://127.0.0.1:8181/users/42
curl http://127.0.0.1:8181/metrics
```

## Updating the library

```bash
git submodule update --remote third_party/cws
git add third_party/cws
git commit -m "bump cws"
```

## How it works

`CMakeLists.txt` calls `add_subdirectory(third_party/cws)`, which registers a
`cws` static library target that exports include paths and compile flags
(`-march=x86-64-v3`, `-mtune=native`). The host target just links it:

```cmake
add_subdirectory(third_party/cws)
target_link_libraries(cws_example_app PRIVATE cws)
```

For an installed `cws` (instead of a submodule), swap the above lines for:

```cmake
find_package(cws REQUIRED CONFIG)
target_link_libraries(cws_example_app PRIVATE cws::cws)
```
