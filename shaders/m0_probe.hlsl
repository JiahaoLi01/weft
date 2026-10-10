// M0 only: pixel-aligned quads and explicit linear-premultiplied materials.
// Material ids mirror ShaderMaterial in probes/probe_constants.hpp.
static const uint MaterialSolid = 0;
static const uint MaterialCoverage = 1;
static const uint MaterialSignedDistance = 3;
static const uint MaterialTextureBottomUp = 4;
static const uint MaterialEncodedBgra = 5;

// IEC 61966-2-1 sRGB transfer function.
static const float SrgbLinearThreshold = 0.04045;
static const float SrgbLinearSlope = 12.92;
static const float SrgbOffset = 0.055;
static const float SrgbScale = 1.055;
static const float SrgbGamma = 2.4;

// FreeType encodes contour zero at byte 128, not normalized 0.5.
static const float SdfByteMax = 255.0;
static const float SdfZeroByte = 128.0;
static const float MinimumAntialiasWidth = 0.0001;

cbuffer DrawConstants : register(b0) {
    float4 rect;
    float2 targetSize;
    uint material;
    float opacity;
    float4 linearPremultipliedColor;
    float4 uvRect;
    float4 sdfParams; // source-texel spread, source-texel stroke
    float4 linearPremultipliedOutline;
};

Texture2D sourceTexture : register(t0);
SamplerState linearClamp : register(s0);

struct Vertex {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float srgbToLinear(float encoded) {
    return encoded <= SrgbLinearThreshold ? encoded / SrgbLinearSlope : pow((encoded + SrgbOffset) / SrgbScale, SrgbGamma);
}

Vertex vs_main(uint id : SV_VertexID) {
    const float2 corners[6] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1)};
    float2 p = corners[id];
    Vertex v;
    v.position = float4((rect.xy + p * rect.zw) / targetSize * float2(2, -2) + float2(-1, 1), 0, 1);
    v.uv = uvRect.xy + p * uvRect.zw;
    return v;
}

float4 ps_main(Vertex v) : SV_Target {
    if (material == MaterialEncodedBgra) {
        // D2D writes encoded premultiplied BGRA8. Decode each texel BEFORE
        // the later UI bilinear filter; preserve alpha and premultiplication.
        float4 value = sourceTexture.Load(int3(int2(v.position.xy), 0));
        float3 encoded = value.a > 0 ? saturate(value.rgb / value.a) : 0;
        float3 decoded = float3(srgbToLinear(encoded.r), srgbToLinear(encoded.g), srgbToLinear(encoded.b));
        return float4(decoded * value.a, value.a);
    }
    if (material == MaterialSolid) {
        return linearPremultipliedColor * opacity;
    }
    if (material == MaterialCoverage) {
        return linearPremultipliedColor * sourceTexture.Sample(linearClamp, v.uv).r * opacity;
    }
    if (material == MaterialSignedDistance) {
        // Distance is linear data; no sRGB transfer, no coverage interpretation.
        float d = (sourceTexture.SampleLevel(linearClamp, v.uv, 0).r * SdfByteMax - SdfZeroByte) * sdfParams.x /
                  SdfZeroByte;
        float aa = max(fwidth(d), MinimumAntialiasWidth);
        float fill = saturate(0.5 + d / aa);
        float outer = saturate(0.5 + (d + sdfParams.y) / aa);
        return (linearPremultipliedColor * fill + linearPremultipliedOutline * (outer - fill)) * opacity;
    }
    float2 sampleUV = v.uv;
    if (material == MaterialTextureBottomUp) {
        sampleUV.y = 1.0 - sampleUV.y;
    }
    return sourceTexture.Sample(linearClamp, sampleUV) * opacity;
}
