/* Interface/method tables for the D3D11 + DXGI COM skeleton.
 *
 * Method order and arity are the documented Direct3D 11 / DXGI interface
 * declarations (d3d11.h, dxgi.h). The ID3D11DeviceContext layout was
 * additionally cross-checked against a real implementation: the reference
 * DXVK d3d11.dll in the reference DLLs of a Wine/Proton prefix exposes the vtable of
 * dxvk::D3D11ImmediateContext (152 slots = 115 base + ID3D11DeviceContext1..4)
 * and every slot's stdcall argument byte count can be read from the trailing
 * `ret imm16` of its target function (tool:
 * port/build/agents/a8-vtable-extract.py). The extracted sequence matches the
 * table below slot for slot, which fixes the base interface at 115 slots and
 * confirms the arity of each entry.
 *
 * Six base-context slots could not be identified offline (their arity is
 * known, their name is not): they are emitted as `unknown_<slot>` and produce
 * the same named stop as any other unimplemented method. They are listed in
 * port/build/agents/a8-report.md together with the evidence.
 *
 * `arg_bytes` is the stdcall argument size including `this` (4 + 4*arguments),
 * i.e. exactly what the callee pops. It is diagnostic information for the
 * trap resolver; no method other than IUnknown is executed. */
#include "guest_com.h"

#define M(name, bytes) { name, bytes }

/* --- IUnknown / DXGI ---------------------------------------------------- */
#define IUNKNOWN_METHODS \
    M("QueryInterface", 12), M("AddRef", 4), M("Release", 4)

#define DXGI_OBJECT_METHODS \
    IUNKNOWN_METHODS, \
    M("SetPrivateData", 16), M("SetPrivateDataInterface", 12), \
    M("GetPrivateData", 16), M("GetParent", 12)

#define DXGI_DEVICE_SUB_OBJECT_METHODS \
    DXGI_OBJECT_METHODS, M("GetDevice", 12)

#define DXGI_FACTORY_METHODS \
    DXGI_OBJECT_METHODS, \
    M("EnumAdapters", 12), M("MakeWindowAssociation", 12), \
    M("GetWindowAssociation", 8), M("CreateSwapChain", 16), \
    M("CreateSoftwareAdapter", 12)

#define DXGI_FACTORY1_METHODS \
    DXGI_FACTORY_METHODS, M("EnumAdapters1", 12), M("IsCurrent", 4)

#define DXGI_ADAPTER_METHODS \
    DXGI_OBJECT_METHODS, \
    M("EnumOutputs", 12), M("GetDesc", 8), M("CheckInterfaceSupport", 12)

#define DXGI_ADAPTER1_METHODS \
    DXGI_ADAPTER_METHODS, M("GetDesc1", 8)

#define DXGI_OUTPUT_METHODS \
    DXGI_OBJECT_METHODS, \
    M("GetDesc", 8), M("GetDisplayModeList", 20), \
    M("FindClosestMatchingMode", 16), M("WaitForVBlank", 4), \
    M("TakeOwnership", 12), M("ReleaseOwnership", 4), \
    M("GetGammaControlCapabilities", 8), M("SetGammaControl", 8), \
    M("GetGammaControl", 8), M("SetDisplaySurface", 8), \
    M("GetDisplaySurfaceData", 8), M("GetFrameStatistics", 8)

#define DXGI_SWAP_CHAIN_METHODS \
    DXGI_DEVICE_SUB_OBJECT_METHODS, \
    M("Present", 12), M("GetBuffer", 16), M("SetFullscreenState", 12), \
    M("GetFullscreenState", 12), M("GetDesc", 8), M("ResizeBuffers", 24), \
    M("ResizeTarget", 8), M("GetContainingOutput", 8), \
    M("GetFrameStatistics", 8), M("GetLastPresentCount", 8)

/* --- D3D11 device child / device --------------------------------------- */
#define DEVICE_CHILD_METHODS \
    IUNKNOWN_METHODS, \
    M("GetDevice", 8), M("GetPrivateData", 16), M("SetPrivateData", 16), \
    M("SetPrivateDataInterface", 12)

#define DEVICE_METHODS \
    IUNKNOWN_METHODS, \
    M("CreateBuffer", 16), M("CreateTexture1D", 16), M("CreateTexture2D", 16), \
    M("CreateTexture3D", 16), M("CreateShaderResourceView", 16), \
    M("CreateUnorderedAccessView", 16), M("CreateRenderTargetView", 16), \
    M("CreateDepthStencilView", 16), M("CreateInputLayout", 24), \
    M("CreateVertexShader", 20), M("CreateGeometryShader", 20), \
    M("CreateGeometryShaderWithStreamOutput", 36), \
    M("CreatePixelShader", 20), M("CreateHullShader", 20), \
    M("CreateDomainShader", 20), M("CreateComputeShader", 20), \
    M("CreateClassLinkage", 8), M("CreateBlendState", 12), \
    M("CreateDepthStencilState", 12), M("CreateRasterizerState", 12), \
    M("CreateSamplerState", 12), M("CreateQuery", 12), \
    M("CreatePredicate", 12), M("CreateCounter", 12), \
    M("CreateDeferredContext", 12), M("OpenSharedResource", 16), \
    M("CheckFormatSupport", 12), M("CheckMultisampleQualityLevels", 16), \
    M("CheckCounterInfo", 8), M("CheckCounter", 40), \
    M("CheckFeatureSupport", 16), M("GetPrivateData", 16), \
    M("SetPrivateData", 16), M("SetPrivateDataInterface", 12), \
    M("GetFeatureLevel", 4), M("GetCreationFlags", 4), \
    M("GetDeviceRemovedReason", 4), M("GetImmediateContext", 8), \
    M("SetExceptionMode", 8), M("GetExceptionMode", 4)

/* --- D3D11 device context: 115 slots = 7 + 108 methods ----------------- */
#define DEVICE_CONTEXT_METHODS \
    DEVICE_CHILD_METHODS, \
    M("VSSetConstantBuffers", 16), M("PSSetShaderResources", 16), \
    M("PSSetShader", 16), M("PSSetSamplers", 16), M("VSSetShader", 16), \
    M("Draw", 12), M("DrawIndexed", 16), M("Map", 24), M("Unmap", 12), \
    M("PSSetConstantBuffers", 16), M("IASetInputLayout", 8), \
    M("IASetVertexBuffers", 24), M("IASetIndexBuffer", 16), \
    M("DrawIndexedInstanced", 24), M("DrawInstanced", 20), \
    M("GSSetConstantBuffers", 16), M("GSSetShader", 16), \
    M("IASetPrimitiveTopology", 8), M("VSSetShaderResources", 16), \
    M("VSSetSamplers", 16), M("Begin", 8), M("End", 8), M("GetData", 20), \
    M("SetPredication", 12), M("GSSetShaderResources", 16), \
    M("GSSetSamplers", 16), M("OMSetRenderTargets", 16), \
    M("OMSetRenderTargetsAndUnorderedAccessViews", 32), \
    M("OMSetBlendState", 16), M("OMSetDepthStencilState", 12), \
    M("SOSetTargets", 16), M("DrawAuto", 4), \
    M("DrawIndexedInstancedIndirect", 12), M("DrawInstancedIndirect", 12), \
    M("Dispatch", 16), M("DispatchIndirect", 12), M("RSSetState", 8), \
    M("RSSetViewports", 12), M("RSSetScissorRects", 12), \
    M("CopySubresourceRegion", 36), M("CopyResource", 12), \
    M("UpdateSubresource", 28), M("CopyStructureCount", 16), \
    M("ClearRenderTargetView", 12), M("ClearUnorderedAccessViewUint", 12), \
    M("ClearUnorderedAccessViewFloat", 12), M("ClearDepthStencilView", 20), \
    M("GenerateMips", 8), M("SetResourceMinLOD", 12), \
    M("GetResourceMinLOD", 8), M("ResolveSubresource", 24), \
    M("unknown_58", 12), \
    M("HSSetShaderResources", 16), M("HSSetShader", 16), \
    M("HSSetSamplers", 16), M("HSSetConstantBuffers", 16), \
    M("DSSetShaderResources", 16), M("DSSetShader", 16), \
    M("DSSetSamplers", 16), M("DSSetConstantBuffers", 16), \
    M("CSSetShaderResources", 16), M("CSSetUnorderedAccessViews", 20), \
    M("CSSetShader", 16), M("CSSetSamplers", 16), \
    M("CSSetConstantBuffers", 16), \
    M("VSGetConstantBuffers", 16), M("PSGetShaderResources", 16), \
    M("PSGetShader", 16), M("PSGetSamplers", 16), M("VSGetShader", 16), \
    M("PSGetConstantBuffers", 16), M("IAGetInputLayout", 8), \
    M("IAGetVertexBuffers", 24), M("IAGetIndexBuffer", 16), \
    M("GSGetConstantBuffers", 16), M("GSGetShader", 16), \
    M("IAGetPrimitiveTopology", 8), M("VSGetShaderResources", 16), \
    M("VSGetSamplers", 16), M("GetPredication", 12), \
    M("GSGetShaderResources", 16), M("GSGetSamplers", 16), \
    M("OMGetRenderTargets", 16), \
    M("OMGetRenderTargetsAndUnorderedAccessViews", 28), \
    M("OMGetBlendState", 16), M("OMGetDepthStencilState", 12), \
    M("SOGetTargets", 12), M("RSGetState", 8), M("RSGetViewports", 12), \
    M("RSGetScissorRects", 12), \
    M("HSGetShaderResources", 16), M("HSGetShader", 16), \
    M("HSGetSamplers", 16), M("HSGetConstantBuffers", 16), \
    M("DSGetShaderResources", 16), M("DSGetShader", 16), \
    M("DSGetSamplers", 16), M("DSGetConstantBuffers", 16), \
    M("CSGetShaderResources", 16), M("CSGetUnorderedAccessViews", 16), \
    M("CSGetShader", 16), M("CSGetSamplers", 16), \
    M("CSGetConstantBuffers", 16), \
    M("unknown_110", 16), M("unknown_111", 4), M("unknown_112", 4), \
    M("unknown_113", 4), M("unknown_114", 12)

/* --- resources, views, shaders, states --------------------------------- */
#define RESOURCE_METHODS \
    DEVICE_CHILD_METHODS, \
    M("GetType", 8), M("SetEvictionPriority", 8), M("GetEvictionPriority", 8)

#define BUFFER_METHODS RESOURCE_METHODS, M("GetDesc", 8)
#define TEXTURE1D_METHODS RESOURCE_METHODS, M("GetDesc", 8)
#define TEXTURE2D_METHODS RESOURCE_METHODS, M("GetDesc", 8)
#define TEXTURE3D_METHODS RESOURCE_METHODS, M("GetDesc", 8)
#define VIEW_METHODS DEVICE_CHILD_METHODS, M("GetResource", 8)
#define SRV_METHODS VIEW_METHODS, M("GetDesc", 8)
#define RTV_METHODS VIEW_METHODS, M("GetDesc", 8)
#define DSV_METHODS VIEW_METHODS, M("GetDesc", 8)
#define UAV_METHODS VIEW_METHODS, M("GetDesc", 8)
#define SHADER_METHODS DEVICE_CHILD_METHODS
#define INPUT_LAYOUT_METHODS DEVICE_CHILD_METHODS
#define BLEND_STATE_METHODS DEVICE_CHILD_METHODS, M("GetDesc", 8)
#define DEPTH_STENCIL_STATE_METHODS DEVICE_CHILD_METHODS, M("GetDesc", 8)
#define RASTERIZER_STATE_METHODS DEVICE_CHILD_METHODS, M("GetDesc", 8)
#define SAMPLER_STATE_METHODS DEVICE_CHILD_METHODS, M("GetDesc", 8)
#define QUERY_METHODS DEVICE_CHILD_METHODS, M("GetDesc", 8)
#define COUNTER_METHODS DEVICE_CHILD_METHODS, M("GetDesc", 8)
#define PREDICATE_METHODS DEVICE_CHILD_METHODS
#define CLASS_LINKAGE_METHODS \
    DEVICE_CHILD_METHODS, M("CreateClassInstance", 24), \
    M("GetClassInstance", 16)
#define COMMAND_LIST_METHODS DEVICE_CHILD_METHODS, M("GetContextFlags", 8)

/* --- tables ------------------------------------------------------------- */
static const GuComMethod m_iunknown[] = { IUNKNOWN_METHODS };
static const GuComMethod m_dxgi_object[] = { DXGI_OBJECT_METHODS };
static const GuComMethod m_dxgi_device_sub_object[] = { DXGI_DEVICE_SUB_OBJECT_METHODS };
static const GuComMethod m_dxgi_factory[] = { DXGI_FACTORY_METHODS };
static const GuComMethod m_dxgi_factory1[] = { DXGI_FACTORY1_METHODS };
static const GuComMethod m_dxgi_adapter[] = { DXGI_ADAPTER_METHODS };
static const GuComMethod m_dxgi_adapter1[] = { DXGI_ADAPTER1_METHODS };
static const GuComMethod m_dxgi_output[] = { DXGI_OUTPUT_METHODS };
static const GuComMethod m_dxgi_swap_chain[] = { DXGI_SWAP_CHAIN_METHODS };
static const GuComMethod m_device_child[] = { DEVICE_CHILD_METHODS };
static const GuComMethod m_device[] = { DEVICE_METHODS };
static const GuComMethod m_device_context[] = { DEVICE_CONTEXT_METHODS };
static const GuComMethod m_resource[] = { RESOURCE_METHODS };
static const GuComMethod m_buffer[] = { BUFFER_METHODS };
static const GuComMethod m_texture1d[] = { TEXTURE1D_METHODS };
static const GuComMethod m_texture2d[] = { TEXTURE2D_METHODS };
static const GuComMethod m_texture3d[] = { TEXTURE3D_METHODS };
static const GuComMethod m_view[] = { VIEW_METHODS };
static const GuComMethod m_srv[] = { SRV_METHODS };
static const GuComMethod m_rtv[] = { RTV_METHODS };
static const GuComMethod m_dsv[] = { DSV_METHODS };
static const GuComMethod m_uav[] = { UAV_METHODS };
static const GuComMethod m_shader[] = { SHADER_METHODS };
static const GuComMethod m_input_layout[] = { INPUT_LAYOUT_METHODS };
static const GuComMethod m_blend_state[] = { BLEND_STATE_METHODS };
static const GuComMethod m_depth_stencil_state[] = { DEPTH_STENCIL_STATE_METHODS };
static const GuComMethod m_rasterizer_state[] = { RASTERIZER_STATE_METHODS };
static const GuComMethod m_sampler_state[] = { SAMPLER_STATE_METHODS };
static const GuComMethod m_query[] = { QUERY_METHODS };
static const GuComMethod m_counter[] = { COUNTER_METHODS };
static const GuComMethod m_predicate[] = { PREDICATE_METHODS };
static const GuComMethod m_class_linkage[] = { CLASS_LINKAGE_METHODS };
static const GuComMethod m_command_list[] = { COMMAND_LIST_METHODS };

/* Documented interface identifiers, GUID memory layout (Data1/2/3 little
 * endian, Data4 as bytes). IID_IUnknown is exercised by the offline tests;
 * the D3D11/DXGI identifiers are the documented values and are used only for
 * QueryInterface matching. An all-zero IID means "not recorded here" and can
 * never match, so such an interface reports E_NOINTERFACE instead of a
 * guessed success. */
#define IID(d1, d2, d3, b0, b1, b2, b3, b4, b5, b6, b7) \
    { (uint8_t)((d1) & 0xff), (uint8_t)(((d1) >> 8) & 0xff), \
      (uint8_t)(((d1) >> 16) & 0xff), (uint8_t)(((d1) >> 24) & 0xff), \
      (uint8_t)((d2) & 0xff), (uint8_t)(((d2) >> 8) & 0xff), \
      (uint8_t)((d3) & 0xff), (uint8_t)(((d3) >> 8) & 0xff), \
      b0, b1, b2, b3, b4, b5, b6, b7 }
#define NO_IID { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }

#define IFACE(nm, iid, known, table) \
    { nm, iid, known, table, (uint32_t)(sizeof(table) / sizeof(table[0])) }

const GuComInterface gu_com_interfaces[] = {
    IFACE("IUnknown", IID(0x00000000u, 0x0000u, 0x0000u, 0xc0, 0x00, 0x00, 0x00,
                          0x00, 0x00, 0x00, 0x46), 1, m_iunknown),
    IFACE("IDXGIObject", IID(0xaec22fb8u, 0x76f3u, 0x4639u, 0x9b, 0xe0, 0x28,
                             0xeb, 0x43, 0xa6, 0x7a, 0x2e), 1, m_dxgi_object),
    IFACE("IDXGIDeviceSubObject", IID(0x3d3e0379u, 0xf9deu, 0x4d58u, 0xbb, 0x6c,
                                      0x18, 0xd6, 0x29, 0x92, 0xf1, 0xa6), 1,
          m_dxgi_device_sub_object),
    IFACE("IDXGIFactory", IID(0x7b7166ecu, 0x21c7u, 0x44aeu, 0xb2, 0x1a, 0xc9,
                              0xae, 0x32, 0x1a, 0xe3, 0x69), 1, m_dxgi_factory),
    IFACE("IDXGIFactory1", IID(0x770aae78u, 0xf26fu, 0x4dbau, 0xa8, 0x29, 0x25,
                               0x3c, 0x83, 0xd1, 0xb3, 0x87), 1, m_dxgi_factory1),
    IFACE("IDXGIAdapter", IID(0x2411e7e1u, 0x12acu, 0x4ccfu, 0xbd, 0x14, 0x97,
                              0x98, 0xe8, 0x53, 0x4d, 0xc0), 1, m_dxgi_adapter),
    IFACE("IDXGIAdapter1", IID(0x29038f61u, 0x3839u, 0x4626u, 0x91, 0xfd, 0x08,
                               0x68, 0x79, 0x01, 0x1a, 0x05), 1, m_dxgi_adapter1),
    IFACE("IDXGIOutput", IID(0xae02eedbu, 0xc735u, 0x4690u, 0x8d, 0x52, 0x5a,
                             0x8d, 0xc2, 0x02, 0x13, 0xaa), 1, m_dxgi_output),
    IFACE("IDXGISwapChain", IID(0x310d36a0u, 0xd2e7u, 0x4c0au, 0xaa, 0x04, 0x6a,
                                0x9d, 0x23, 0xb8, 0x88, 0x6a), 1,
          m_dxgi_swap_chain),
    IFACE("ID3D11DeviceChild", IID(0x1841e5c8u, 0x16b0u, 0x469bu, 0xa8, 0xcc,
                                   0x13, 0xbc, 0xf0, 0xe5, 0xa4, 0xe6), 1,
          m_device_child),
    IFACE("ID3D11Device", IID(0xdb6f6ddbu, 0xac77u, 0x4e88u, 0x82, 0x53, 0x81,
                              0x9d, 0xf9, 0xbb, 0xf1, 0x40), 1, m_device),
    IFACE("ID3D11DeviceContext", IID(0xc0bfa96cu, 0xe089u, 0x44fbu, 0x8e, 0xaf,
                                     0x26, 0xf8, 0x79, 0x61, 0x90, 0xda), 1,
          m_device_context),
    IFACE("ID3D11Resource", IID(0xdc8e63f3u, 0xd12bu, 0x4952u, 0xb4, 0x7b, 0x5e,
                                0x45, 0x02, 0x6a, 0x86, 0x2d), 1, m_resource),
    IFACE("ID3D11Buffer", NO_IID, 0, m_buffer),
    IFACE("ID3D11Texture1D", NO_IID, 0, m_texture1d),
    IFACE("ID3D11Texture2D", IID(0x6f15aaf2u, 0xd208u, 0x4e89u, 0x9a, 0xb4, 0x48,
                                 0x95, 0x35, 0xd3, 0x4f, 0x9c), 1, m_texture2d),
    IFACE("ID3D11Texture3D", NO_IID, 0, m_texture3d),
    IFACE("ID3D11View", IID(0x839d1216u, 0xbb2eu, 0x412bu, 0xb7, 0xf4, 0xa9, 0xdb,
                            0xeb, 0xe1, 0xa6, 0xbe), 1, m_view),
    IFACE("ID3D11ShaderResourceView", IID(0xb0e06fe0u, 0x8192u, 0x4e1au, 0xb1, 0xca,
                                          0x36, 0xd7, 0x41, 0x47, 0x10, 0xb2), 1,
          m_srv),
    IFACE("ID3D11RenderTargetView", IID(0xdf7110d6u, 0x48e2u, 0x4fb3u, 0xae, 0x23,
                                        0xea, 0x4c, 0x1d, 0x5c, 0x3d, 0x13), 1,
          m_rtv),
    IFACE("ID3D11DepthStencilView", IID(0x9fdac92au, 0x1876u, 0x48c3u, 0xaf, 0xad,
                                        0x25, 0xb9, 0x4f, 0x84, 0xa9, 0xb6), 1,
          m_dsv),
    IFACE("ID3D11UnorderedAccessView", IID(0x28acf509u, 0x7f5cu, 0x48f6u, 0x86,
                                           0x11, 0xf3, 0x16, 0x01, 0x0a, 0x63,
                                           0x80), 1, m_uav),
    IFACE("ID3D11VertexShader", IID(0x3b301d64u, 0xd678u, 0x4289u, 0x88, 0x97,
                                    0x22, 0xf8, 0x92, 0x8b, 0x72, 0xf3), 1,
          m_shader),
    IFACE("ID3D11HullShader", NO_IID, 0, m_shader),
    IFACE("ID3D11DomainShader", NO_IID, 0, m_shader),
    IFACE("ID3D11GeometryShader", IID(0x38325b96u, 0xeffbu, 0x4022u, 0xba, 0x02,
                                      0x2e, 0x79, 0x5b, 0x70, 0x27, 0x5c), 1,
          m_shader),
    IFACE("ID3D11PixelShader", IID(0xea82e40du, 0x51dcu, 0x4f33u, 0x93, 0xd4,
                                   0xdb, 0x7c, 0x91, 0x25, 0xae, 0x8c), 1,
          m_shader),
    IFACE("ID3D11ComputeShader", IID(0x4f5b196eu, 0xc2bdu, 0x495eu, 0xbd, 0x01,
                                     0x1f, 0xde, 0xd3, 0x8e, 0x49, 0x69), 1,
          m_shader),
    IFACE("ID3D11InputLayout", IID(0xe4819ddcu, 0x4cf0u, 0x4025u, 0xbd, 0x26,
                                   0x5d, 0xe8, 0x2a, 0x3e, 0x07, 0xb7), 1,
          m_input_layout),
    IFACE("ID3D11BlendState", NO_IID, 0, m_blend_state),
    IFACE("ID3D11DepthStencilState", IID(0x03823efbu, 0x8d8fu, 0x4e1cu, 0x9e, 0xa1,
                                         0xf2, 0x95, 0x62, 0xb4, 0xcf, 0x7d), 1,
          m_depth_stencil_state),
    IFACE("ID3D11RasterizerState", IID(0x9bb4ab81u, 0xab1au, 0x4d8fu, 0xb5, 0x06,
                                       0xfc, 0x04, 0x20, 0x0b, 0x6e, 0xe7), 1,
          m_rasterizer_state),
    IFACE("ID3D11SamplerState", NO_IID, 0, m_sampler_state),
    IFACE("ID3D11Query", IID(0xd6c00747u, 0x87b7u, 0x425eu, 0xb8, 0x4d, 0x44,
                             0xd1, 0x08, 0x56, 0x0a, 0xfd), 1, m_query),
    IFACE("ID3D11Predicate", NO_IID, 0, m_predicate),
    IFACE("ID3D11Counter", NO_IID, 0, m_counter),
    IFACE("ID3D11ClassLinkage", NO_IID, 0, m_class_linkage),
    IFACE("ID3D11CommandList", NO_IID, 0, m_command_list)
};

const uint32_t gu_com_interface_count =
    (uint32_t)(sizeof(gu_com_interfaces) / sizeof(gu_com_interfaces[0]));
