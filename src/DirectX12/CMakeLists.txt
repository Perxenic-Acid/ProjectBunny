# ============================================================
# DirectX12 - d3d12.dll proxy and minimal DXGI/Present hook
# ============================================================
add_library(DirectX12 SHARED
    D3D12Wrapper.cpp
    DX12DeviceHooks.cpp
    DX12Input.cpp
    DX12Overlay.cpp
    DX12ShaderDump.cpp
    DX12State.cpp
    DXGIHooks.cpp
)

target_include_directories(DirectX12 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/3rdparty
)

target_link_libraries(DirectX12 PRIVATE
    Nektra::NktHookLib
    dxgi.lib
    User32.lib
    Gdi32.lib
    Shlwapi.lib
)

target_sources(DirectX12 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/d3d12Wrapper.def
)

set_target_properties(DirectX12 PROPERTIES
    OUTPUT_NAME "d3d12"
)

target_compile_definitions(DirectX12 PRIVATE
    UNICODE
    _UNICODE
    _USRDLL
    MIGOTO_DX=12
)

set_target_msvc_options(DirectX12)
set_target_common_definitions(DirectX12)
