cmake_minimum_required(VERSION 3.24)

project(KeyHunt LANGUAGES CXX)

option(WITH_GPU "Enable CUDA GPU support" OFF)
option(ALLOW_UNSUPPORTED_MSVC_FOR_CUDA
  "Pass --allow-unsupported-compiler to nvcc (useful with newer MSVC toolsets than current CUDA support matrix)"
  OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(COMMON_SOURCES
    Base58.cpp
    Bech32.cpp
    Bloom.cpp
    Int.cpp
    IntGroup.cpp
    IntMod.cpp
    KeyHunt.cpp
    Main.cpp
    Point.cpp
    Random.cpp
    SECP256K1.cpp
    Timer.cpp
    GPU/GPUGenerate.cpp
    hash/ripemd160.cpp
    hash/sha256.cpp
    hash/sha512.cpp
    hash/ripemd160_sse.cpp
    hash/sha256_sse.cpp
)

add_executable(KeyHunt ${COMMON_SOURCES})

find_package(Threads REQUIRED)
target_link_libraries(KeyHunt PRIVATE Threads::Threads)
target_include_directories(KeyHunt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  target_compile_definitions(KeyHunt PRIVATE WIN64)
endif()

if(MSVC)
  target_compile_options(KeyHunt PRIVATE
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Release>>:/O2>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:RelWithDebInfo>>:/O2>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:MinSizeRel>>:/O1>
  )
else()
  target_compile_options(KeyHunt PRIVATE -mssse3 -Wno-write-strings -O3 -march=native)
endif()

if(WITH_GPU)
  if(WIN32 AND CMAKE_GENERATOR MATCHES "Visual Studio")
    message(WARNING
      "Visual Studio generators require CUDA VS toolset integration. "
      "If configure fails with 'No CUDA toolset found', use Ninja generator from a VS Developer shell "
      "(or install a CUDA version that integrates with your Visual Studio version).")
  endif()

  if(MSVC AND ALLOW_UNSUPPORTED_MSVC_FOR_CUDA)
    if(NOT CMAKE_CUDA_FLAGS MATCHES "(^| )--allow-unsupported-compiler($| )")
      set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} --allow-unsupported-compiler")
    endif()
    message(WARNING "ALLOW_UNSUPPORTED_MSVC_FOR_CUDA=ON: passing --allow-unsupported-compiler to nvcc.")
  endif()

  if(NOT CMAKE_CUDA_COMPILER)
    set(_KEYHUNT_CUDA_NVCC_HINTS "")
    if(DEFINED ENV{CUDA_PATH})
      list(APPEND _KEYHUNT_CUDA_NVCC_HINTS "$ENV{CUDA_PATH}/bin")
    endif()
    if(WIN32)
      file(GLOB _KEYHUNT_CUDA_NVCC_GLOB
        "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v*/bin/nvcc.exe")
      if(_KEYHUNT_CUDA_NVCC_GLOB)
        list(SORT _KEYHUNT_CUDA_NVCC_GLOB COMPARE NATURAL ORDER DESCENDING)
        foreach(_nvcc ${_KEYHUNT_CUDA_NVCC_GLOB})
          get_filename_component(_nvcc_dir "${_nvcc}" DIRECTORY)
          list(APPEND _KEYHUNT_CUDA_NVCC_HINTS "${_nvcc_dir}")
        endforeach()
      endif()
    endif()

    find_program(KEYHUNT_NVCC_EXE
      NAMES nvcc nvcc.exe
      HINTS ${_KEYHUNT_CUDA_NVCC_HINTS}
    )

    if(KEYHUNT_NVCC_EXE)
      set(CMAKE_CUDA_COMPILER "${KEYHUNT_NVCC_EXE}" CACHE FILEPATH "CUDA compiler" FORCE)
      message(STATUS "Using CUDA compiler: ${CMAKE_CUDA_COMPILER}")
    else()
      message(FATAL_ERROR "CUDA compiler not found. Install CUDA Toolkit 12+ or configure CMAKE_CUDA_COMPILER.")
    endif()
  endif()

  enable_language(CUDA)
  set(CMAKE_CUDA_STANDARD 17)
  set(CMAKE_CUDA_STANDARD_REQUIRED ON)

  find_package(CUDAToolkit 12.0 REQUIRED)

  if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    # Broad default support; sm_90a is appended via explicit gencode fallback below for compatibility.
    set(CMAKE_CUDA_ARCHITECTURES 60 70 75 80 86 89 90)
  endif()
  set_property(TARGET KeyHunt PROPERTY CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}")

  target_sources(KeyHunt PRIVATE GPU/GPUEngine.cu)
  target_compile_definitions(KeyHunt PRIVATE WITHGPU)
  target_link_libraries(KeyHunt PRIVATE CUDA::cudart)

  set_target_properties(KeyHunt PROPERTIES
    CUDA_SEPARABLE_COMPILATION OFF
    CUDA_RESOLVE_DEVICE_SYMBOLS ON
  )

  # IPO is toolchain-dependent; enable when supported.
  include(CheckIPOSupported)
  check_ipo_supported(RESULT KEYHUNT_IPO_SUPPORTED OUTPUT KEYHUNT_IPO_ERROR)
  if(KEYHUNT_IPO_SUPPORTED)
    set_property(TARGET KeyHunt PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  endif()

  set(KEYHUNT_CUDA_OPTS
      -gencode=arch=compute_90a,code=sm_90a
  )

  if(MSVC)
    # Host MSVC optimization flags are added per-config below to avoid /RTC1 (Debug) conflicts.
  else()
    list(APPEND KEYHUNT_CUDA_OPTS -Xcompiler=-fPIC,-O3,-march=native)
  endif()

  target_compile_options(KeyHunt PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:${KEYHUNT_CUDA_OPTS}>
    $<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:Release>>:-O3>
    $<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:Release>>:--use_fast_math>
    $<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:Release>>:--maxrregcount=128>
    $<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:Release>>:--ptxas-options=-v,-O3>
    $<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:RelWithDebInfo>>:-O3>
    $<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:RelWithDebInfo>>:--use_fast_math>
    $<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:RelWithDebInfo>>:--maxrregcount=128>
    $<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:RelWithDebInfo>>:--ptxas-options=-v,-O3>
    $<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:MinSizeRel>>:-O2>
    $<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:MinSizeRel>>:--use_fast_math>
    $<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:MinSizeRel>>:--maxrregcount=128>
    $<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:MinSizeRel>>:--ptxas-options=-v,-O2>
  )
endif()
