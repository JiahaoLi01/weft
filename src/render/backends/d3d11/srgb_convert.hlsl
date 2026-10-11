// Encoded sRGB straight-alpha RGBA8 to linear premultiplied float, the conversion Renderer::prepare used to run on the
// CPU. The tables are the CPU's: tables[i] = srgb_to_linear(i / 255) and tables[256 + i] = i / 255, both computed in
// single precision on the host, and a single correctly rounded multiply per channel reproduces its texels exactly.
// The ENCODED_PREMULTIPLIED variant converts encoded premultiplied texels instead (Direct2D's BGRA8 targets).

// One triangle covering the viewport.
float4 vs_main(uint vertex : SV_VertexID) : SV_Position {
    const float2 corner = float2((vertex << 1) & 2, vertex & 2);
    return float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
}

#if ENCODED_PREMULTIPLIED
// IEC 61966-2-1 sRGB transfer function.
static const float SrgbLinearThreshold = 0.04045;
static const float SrgbLinearSlope = 12.92;
static const float SrgbOffset = 0.055;
static const float SrgbScale = 1.055;
static const float SrgbGamma = 2.4;

Texture2D encoded : register(t0);

float srgbToLinear(float value) {
    return value <= SrgbLinearThreshold ? value / SrgbLinearSlope : pow((value + SrgbOffset) / SrgbScale, SrgbGamma);
}

// Each texel decodes before any later filter; alpha and premultiplication are kept.
float4 ps_main(float4 position : SV_Position) : SV_Target {
    float4 value = encoded.Load(int3(int2(position.xy), 0));
    float3 straight = value.a > 0 ? saturate(value.rgb / value.a) : 0;
    float3 decoded = float3(srgbToLinear(straight.r), srgbToLinear(straight.g), srgbToLinear(straight.b));
    return float4(decoded * value.a, value.a);
}
#else
static const uint AlphaTableOffset = 256;

Texture2D<uint4> source : register(t0);
Buffer<float> tables : register(t1);

float4 ps_main(float4 position : SV_Position) : SV_Target {
    const uint4 value = source.Load(int3(int2(position.xy), 0));
    const float alpha = tables[AlphaTableOffset + value.a];
    return float4(tables[value.r] * alpha, tables[value.g] * alpha, tables[value.b] * alpha, alpha);
}
#endif
