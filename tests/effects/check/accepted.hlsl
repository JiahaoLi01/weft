// Accepted: comments may name anything, Texture2D noise : register(t9); and SamplerState too; static globals, types
// and helper functions are the effect's own.
/* cbuffer Tint { float4 tint; };
   Texture2D<float4> noise; */
#define TAU 6.2831853

static const float2 offsets[2] = {float2(0, 0), float2(.5, .5)};

struct Wave {
    float phase;
};

float wave(Wave w, float scale = 1) {
    return sin(w.phase * TAU) * scale;
}

float4 effect(EffectInput input) {
    // effectTexture0 and effectRepeat are the shared shader's, not declarations of the effect.
    Wave w = {input.time};
    return effectTexture0.Sample(effectRepeat, input.uv + offsets[1]) * wave(w);
}
