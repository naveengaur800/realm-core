set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

set(CMAKE_CXX_FLAGS_INIT "-Wno-psabi")

set(CMAKE_FIND_ROOT_PATH "/usr/${CMAKE_LIBRARY_ARCHITECTURE}")

set(ENV{PKG_CONFIG_SYSROOT_DIR} "/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}")

set(THREADS_PTHREAD_ARG -pthread)

set(REALM_USE_SYSTEM_OPENSSL ON)
