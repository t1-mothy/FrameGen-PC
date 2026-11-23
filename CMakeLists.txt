cmake_minimum_required(VERSION 3.21)
project(FrameGen VERSION 1.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # static CRT: no VC++ redist needed

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(FG_SOURCES
  src/Core.cpp
  src/Prefs.cpp
  src/Gpu.cpp
  src/Capture.cpp
  src/Synthetic.cpp
  src/Pipeline.cpp
  src/Interpolators.cpp
  src/FlowEngine.cpp
  src/VfgEngine.cpp
  src/Hud.cpp
  src/Renderer.cpp
  src/Audio.cpp
  src/Dashboard.cpp
  src/Settings.cpp
  src/App.cpp
  src/Tools.cpp
  src/DlssG.cpp
  src/DlssPresenter.cpp
  src/Install.cpp
  src/main.cpp
)

set(FG_LIBS d3d11 dxgi d3dcompiler d2d1 dwrite windowscodecs
            mfplat mfreadwrite mf mfuuid ole32 oleaut32 uuid
            d3d12 wintrust crypt32 dbghelp avrt winmm comctl32 shlwapi dwmapi setupapi cfgmgr32 propsys shell32 user32 gdi32)

set(FG_DEFS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
            FG_VERSION="${PROJECT_VERSION}" FG_VER_MAJOR=${PROJECT_VERSION_MAJOR} FG_VER_MINOR=${PROJECT_VERSION_MINOR} FG_VER_PATCH=${PROJECT_VERSION_PATCH})

# FrameGen.exe — the app (Windows subsystem: no console window on double-click).
add_executable(FrameGen WIN32 ${FG_SOURCES} src/app.rc src/app.manifest)
target_compile_definitions(FrameGen PRIVATE ${FG_DEFS})
target_link_libraries(FrameGen PRIVATE ${FG_LIBS})
target_compile_options(FrameGen PRIVATE /W3 /utf-8 /MP /permissive-)

# framegen-cli.exe — same program as a console app, for the test tools and --log
# (output goes straight to the terminal and the terminal waits for it to finish).
add_executable(framegen-cli ${FG_SOURCES} src/app.rc src/app.manifest)
target_compile_definitions(framegen-cli PRIVATE ${FG_DEFS} FG_CONSOLE=1)
target_link_libraries(framegen-cli PRIVATE ${FG_LIBS})
target_compile_options(framegen-cli PRIVATE /W3 /utf-8 /MP /permissive-)

# Optional: NVIDIA Streamline (DLSS Frame Generation). Get it with tools/get_streamline.ps1.
set(SL_DIR ${CMAKE_SOURCE_DIR}/third_party/streamline)
if(EXISTS ${SL_DIR}/include/sl.h)
  message(STATUS "Streamline found: DLSS Frame Generation enabled")
  foreach(t FrameGen framegen-cli)
    target_include_directories(${t} PRIVATE ${SL_DIR}/include)
    target_compile_definitions(${t} PRIVATE FG_HAVE_STREAMLINE=1)
  endforeach()
  set(SL_DLLS sl.interposer.dll sl.common.dll sl.dlss_g.dll sl.reflex.dll sl.pcl.dll nvngx_dlssg.dll)
  foreach(d ${SL_DLLS})
    add_custom_command(TARGET FrameGen POST_BUILD COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:FrameGen>/streamline
                       COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SL_DIR}/bin/x64/${d} $<TARGET_FILE_DIR:FrameGen>/streamline/${d})
  endforeach()
endif()

# Optional: NVIDIA VFG runtime for local testing (NVIDIA's DLLs, not committed — see docs/VFG.md).
# Anything in third_party/nvvfx/runtime is copied next to the app as nvidia-vfx\.
set(VFX_RT ${CMAKE_SOURCE_DIR}/third_party/nvvfx/runtime)
if(EXISTS ${VFX_RT}/NVVideoEffects.dll)
  message(STATUS "NVIDIA VFG runtime found: copied to nvidia-vfx")
  foreach(t FrameGen framegen-cli)
    add_custom_command(TARGET ${t} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${VFX_RT} $<TARGET_FILE_DIR:${t}>/nvidia-vfx)
  endforeach()
endif()

# ---- Installer (per-user, no admin rights): .\package.ps1, or cmake --build build --target FrameGenSetup
# Embeds FrameGen.exe, the licences, NVIDIA DLSS Frame Generation (Streamline) when the SDK is in
# third_party/streamline, and NVIDIA VFG when its runtime is in third_party/nvvfx/runtime
# (plus the Visual C++ runtime DLLs NVIDIA's DLLs need, app-local in nvidia-vfx\).
set(payload_rel FrameGen.exe LICENSE.txt THIRD_PARTY_NOTICES.txt README.txt)
set(payload_src "${CMAKE_BINARY_DIR}/FrameGen.exe" "${CMAKE_SOURCE_DIR}/LICENSE" "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.txt" "${CMAKE_SOURCE_DIR}/installer/README.txt")
if(EXISTS ${VFX_RT}/NVVideoEffects.dll)
  file(GLOB_RECURSE rt RELATIVE ${VFX_RT} ${VFX_RT}/*)
  foreach(f ${rt})
    list(APPEND payload_rel "nvidia-vfx/${f}")
    list(APPEND payload_src "${VFX_RT}/${f}")
  endforeach()
  if(DEFINED ENV{VCToolsRedistDir})
    file(TO_CMAKE_PATH "$ENV{VCToolsRedistDir}" redist)
    file(GLOB crt "${redist}/x64/Microsoft.VC*.CRT/msvcp140.dll" "${redist}/x64/Microsoft.VC*.CRT/msvcp140_1.dll"
                  "${redist}/x64/Microsoft.VC*.CRT/vcruntime140.dll" "${redist}/x64/Microsoft.VC*.CRT/vcruntime140_1.dll")
    foreach(f ${crt})
      get_filename_component(n ${f} NAME)
      list(APPEND payload_rel "nvidia-vfx/${n}")
      list(APPEND payload_src "${f}")
    endforeach()
  endif()
endif()
if(EXISTS ${SL_DIR}/include/sl.h)
  foreach(d ${SL_DLLS})
    list(APPEND payload_rel "streamline/${d}")
    list(APPEND payload_src "${SL_DIR}/bin/x64/${d}")
  endforeach()
  list(APPEND payload_rel streamline/licenses/Streamline-license.txt streamline/licenses/nvngx_dlss.license.txt
                          streamline/licenses/reflex.license.txt streamline/licenses/3rd-party-licenses.md)
  list(APPEND payload_src "${SL_DIR}/license.txt" "${SL_DIR}/bin/x64/nvngx_dlss.license.txt"
                          "${SL_DIR}/bin/x64/reflex.license.txt" "${SL_DIR}/3rd-party-licenses.md")
endif()
set(list_text "")
set(rc_text "999 RCDATA \"${CMAKE_BINARY_DIR}/setup_files.txt\"
")
set(id 1000)
foreach(rel src IN ZIP_LISTS payload_rel payload_src)
  string(APPEND list_text "${rel}
")
  string(APPEND rc_text "${id} RCDATA \"${src}\"
")
  math(EXPR id "${id} + 1")
endforeach()
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/setup_files.txt CONTENT "${list_text}")
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/setup_payload.rc CONTENT "${rc_text}")
set_source_files_properties(${CMAKE_BINARY_DIR}/setup_payload.rc PROPERTIES GENERATED TRUE OBJECT_DEPENDS "${payload_src};${CMAKE_BINARY_DIR}/setup_files.txt")
add_executable(FrameGenSetup WIN32 EXCLUDE_FROM_ALL installer/Setup.cpp src/Install.cpp installer/setup.rc
               ${CMAKE_BINARY_DIR}/setup_payload.rc installer/setup.manifest)
target_compile_definitions(FrameGenSetup PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
                           FG_VERSION="${PROJECT_VERSION}" FG_VER_MAJOR=${PROJECT_VERSION_MAJOR} FG_VER_MINOR=${PROJECT_VERSION_MINOR} FG_VER_PATCH=${PROJECT_VERSION_PATCH})
target_link_libraries(FrameGenSetup PRIVATE comctl32 shell32 ole32 user32 gdi32 advapi32 uuid)
target_compile_options(FrameGenSetup PRIVATE /W3 /utf-8 /permissive-)
set_target_properties(FrameGenSetup PROPERTIES OUTPUT_NAME "FrameGen-Setup-${PROJECT_VERSION}")
add_dependencies(FrameGenSetup FrameGen)
