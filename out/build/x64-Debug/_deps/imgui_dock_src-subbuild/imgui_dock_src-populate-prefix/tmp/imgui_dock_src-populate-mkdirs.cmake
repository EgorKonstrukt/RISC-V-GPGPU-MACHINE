# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "A:/DEV/RISC-V-GPGPU-MACHINE/out/build/x64-Debug/_deps/imgui_dock_src-src")
  file(MAKE_DIRECTORY "A:/DEV/RISC-V-GPGPU-MACHINE/out/build/x64-Debug/_deps/imgui_dock_src-src")
endif()
file(MAKE_DIRECTORY
  "A:/DEV/RISC-V-GPGPU-MACHINE/out/build/x64-Debug/_deps/imgui_dock_src-build"
  "A:/DEV/RISC-V-GPGPU-MACHINE/out/build/x64-Debug/_deps/imgui_dock_src-subbuild/imgui_dock_src-populate-prefix"
  "A:/DEV/RISC-V-GPGPU-MACHINE/out/build/x64-Debug/_deps/imgui_dock_src-subbuild/imgui_dock_src-populate-prefix/tmp"
  "A:/DEV/RISC-V-GPGPU-MACHINE/out/build/x64-Debug/_deps/imgui_dock_src-subbuild/imgui_dock_src-populate-prefix/src/imgui_dock_src-populate-stamp"
  "A:/DEV/RISC-V-GPGPU-MACHINE/out/build/x64-Debug/_deps/imgui_dock_src-subbuild/imgui_dock_src-populate-prefix/src"
  "A:/DEV/RISC-V-GPGPU-MACHINE/out/build/x64-Debug/_deps/imgui_dock_src-subbuild/imgui_dock_src-populate-prefix/src/imgui_dock_src-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "A:/DEV/RISC-V-GPGPU-MACHINE/out/build/x64-Debug/_deps/imgui_dock_src-subbuild/imgui_dock_src-populate-prefix/src/imgui_dock_src-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "A:/DEV/RISC-V-GPGPU-MACHINE/out/build/x64-Debug/_deps/imgui_dock_src-subbuild/imgui_dock_src-populate-prefix/src/imgui_dock_src-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
