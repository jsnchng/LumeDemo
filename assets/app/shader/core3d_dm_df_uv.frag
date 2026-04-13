#version 460 core
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// 可微渲染GBuffer - 添加UV输出

#include "3d/shaders/common/3d_dm_brdf_common.h"
#include "3d/shaders/common/3d_dm_shadowing_common.h"
#include "3d/shaders/common/3d_dm_structures_common.h"
#include "3d/shaders/common/3d_dm_target_packing_common.h"
#include "render/shaders/common/render_color_conversion_common.h"
#include "render/shaders/common/render_post_process_common.h"
#include "render/shaders/common/render_tonemap_common.h"

#include "3d/shaders/common/3d_dm_frag_layout_common.h"
#include "3d/shaders/common/3d_dm_lighting_common.h"
#define CORE3D_DM_DF_FRAG_INPUT 1
#include "3d/shaders/common/3d_dm_inout_common.h"
#include "3d/shaders/common/3d_dm_inplace_sampling_common.h"

// GBuffer输出 - 5个attachment
layout(location = 0) out vec4 outColor;           // 自发光
layout(location = 1) out vec4 outVelocityNormal;  // 速度 + 法线
layout(location = 2) out vec4 outBaseColor;       // 漫反射 + AO
layout(location = 3) out vec4 outMaterial;        // 材质参数
layout(location = 4) out vec4 outUV;              // UV坐标 (新增)

uint GetInstanceIndex()
{
    uint instanceIdx = 0U;
    if ((CORE_MATERIAL_FLAGS & CORE_MATERIAL_GPU_INSTANCING_BIT) == CORE_MATERIAL_GPU_INSTANCING_BIT) {
        instanceIdx = GetUnpackFlatIndicesInstanceIdx(inIndices);
    }
    return instanceIdx;
}

///////////////////////////////////////////////////////////////////////////////
// "main" functions

void UnlitBasic()
{
    const uint instanceIdx = GetInstanceIndex();
    
    // Get UV from input (inUv is vec4, use .xy)
    vec2 uv = inUv.xy;
    
    CORE_RELAXEDP vec4 baseColor = GetBaseColorSample(inUv) * GetUnpackBaseColor(instanceIdx) * inColor;
    baseColor.a = clamp(baseColor.a, 0.0, 1.0);
    if ((CORE_MATERIAL_FLAGS & CORE_MATERIAL_ADDITIONAL_SHADER_DISCARD_BIT) ==
        CORE_MATERIAL_ADDITIONAL_SHADER_DISCARD_BIT) {
        if (baseColor.a < GetUnpackAlphaCutoff(instanceIdx)) {
            discard;
        }
    }

    // 输出UV坐标 (RG通道)
    outUV = vec4(uv, 0.0, 1.0);
    
    // 原有输出
    outColor = GetPackColor(vec4(0.0, 0.0, 0.0, 1.0));
    const uint cameraIdx = GetUnpackFlatIndicesCameraIdx(inIndices);
    outVelocityNormal = GetPackVelocityAndNormal(
        GetFinalCalculatedVelocity(inPos.xyz, inPrevPosI.xyz, cameraIdx), 
        normalize(inNormal));
    outBaseColor = GetPackBaseColorWithAo(baseColor.xyz, 1.0);
    outMaterial = GetPackMaterialWithFlags(
        vec4(0.0, 1.0, 1.0, 0.04), 
        CORE_MATERIAL_UNLIT, 
        CORE_MATERIAL_FLAGS);
}

void UnlitShadowAlpha()
{
    // 简化实现，与原版保持一致
    UnlitBasic();
}

void PbrBasic()
{
    const uint instanceIdx = GetInstanceIndex();
    
    // Get UV from input (inUv is vec4)
    vec2 uv = inUv.xy;
    
    // 采样材质
    CORE_RELAXEDP vec4 baseColor = GetBaseColorSample(inUv, instanceIdx) * 
                                   GetUnpackBaseColor(instanceIdx) * inColor;
    baseColor.a = clamp(baseColor.a, 0.0, 1.0);
    
    if ((CORE_MATERIAL_FLAGS & CORE_MATERIAL_ADDITIONAL_SHADER_DISCARD_BIT) ==
        CORE_MATERIAL_ADDITIONAL_SHADER_DISCARD_BIT) {
        if (baseColor.a < GetUnpackAlphaCutoff(instanceIdx)) {
            discard;
        }
    }
    
    // 法线 - 使用输入法线（简化版本）
    vec3 normNormal = normalize(inNormal);
    
    // 材质参数
    CORE_RELAXEDP vec4 material = GetMaterialSample(inUv, instanceIdx) * 
                                  GetUnpackMaterial(instanceIdx);
    GetFinalCorrectedRoughness(normNormal, material.g);
    
    // AO
    const CORE_RELAXEDP float ao = clamp(
        GetAOSample(inUv, instanceIdx) * GetUnpackAO(instanceIdx), 0.0, 1.0);
    
    // 自发光
    CORE_RELAXEDP vec3 emissive = GetEmissiveSample(inUv, instanceIdx) * 
                                  GetUnpackEmissiveColor(instanceIdx);
    emissive = emissive * baseColor.a;
    
    // 输出UV坐标 (RG通道存储UV, BA通道保留)
    outUV = vec4(uv, 0.0, 1.0);
    
    // 原有输出
    outColor = GetPackColor(vec4(emissive, 1.0));
    const uint cameraIdx = GetUnpackFlatIndicesCameraIdx(inIndices);
    outVelocityNormal = GetPackVelocityAndNormal(
        GetFinalCalculatedVelocity(inPos.xyz, inPrevPosI.xyz, cameraIdx), 
        normNormal);
    outBaseColor = GetPackBaseColorWithAo(baseColor.xyz, ao);
    outMaterial = GetPackMaterialWithFlags(material, CORE_MATERIAL_TYPE, CORE_MATERIAL_FLAGS);
}

/*
 * fragment shader for basic pbr materials with UV output.
 */
void main(void)
{
    if (CORE_MATERIAL_TYPE == CORE_MATERIAL_UNLIT) {
        UnlitBasic();
    } else if (CORE_MATERIAL_TYPE == CORE_MATERIAL_UNLIT_SHADOW_ALPHA) {
        UnlitShadowAlpha();
    } else {
        PbrBasic();
    }
}
