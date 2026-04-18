#version 460 core
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// 4-panel atlas display:
// Top row:    SR Forward | GT
// Bottom row: LR Texture | LR Gradient

#include "render/shaders/common/render_compatibility_common.h"
#include "render/shaders/common/render_post_process_structs_common.h"

layout(set = 0, binding = 0) uniform sampler uSampler;
layout(set = 0, binding = 1) uniform texture2D uSRForward;   // debugOutput (1024x1024)
layout(set = 0, binding = 2) uniform texture2D uGT;          // color (1024x1024)
layout(set = 0, binding = 3) uniform texture2D uLRTexture;   // lr_texture (512x512)
layout(set = 0, binding = 4) uniform texture2D uLRGradient;  // lr_gradient (512x512)

layout(push_constant, std430) uniform uPostProcessPushConstant
{
    LocalPostProcessPushConstantStruct uPc;
};

layout (location = 0) in vec2 inUv;
layout (location = 0) out vec4 outColor;

void main(void)
{
    vec2 uv = inUv.xy;
    vec2 texUv;
    
    // 4-panel layout in 1024x1024 window (each panel 512x512)
    if (uv.y < 0.5) {
        // Top row
        if (uv.x < 0.5) {
            // Top-left: SR Forward (debugOutput 1024x1024)
            texUv = vec2(uv.x * 2.0, uv.y * 2.0);
            outColor = textureLod(sampler2D(uSRForward, uSampler), texUv, 0);
        } else {
            // Top-right: GT (color 1024x1024)
            texUv = vec2((uv.x - 0.5) * 2.0, uv.y * 2.0);
            outColor = textureLod(sampler2D(uGT, uSampler), texUv, 0);
        }
    } else {
        // Bottom row
        if (uv.x < 0.5) {
            // Bottom-left: LR Texture (512x512)
            texUv = vec2(uv.x * 2.0, (uv.y - 0.5) * 2.0);
            outColor = textureLod(sampler2D(uLRTexture, uSampler), texUv, 0);
        } else {
            // Bottom-right: LR Gradient (512x512)
            texUv = vec2((uv.x - 0.5) * 2.0, (uv.y - 0.5) * 2.0);
            outColor = textureLod(sampler2D(uLRGradient, uSampler), texUv, 0);
        }
    }
}