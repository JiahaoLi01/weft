// Encoded sRGB straight-alpha RGBA8 to linear premultiplied float, the conversion Renderer::prepare used to run on the
// CPU. The tables are the CPU's: tables[i] = srgb_to_linear(i / 255) and tables[256 + i] = i / 255, both computed in
// single precision on the host, and a single correctly rounded multiply per channel reproduces its texels exactly.

static const uint AlphaTableOffset = 256;

Texture2D<uint4> source : register(t0);
Buffer<float> tables : register(t1);

// One triangle covering the viewport.
float4 vs_main(uint vertex : SV_VertexID) : SV_Position {
    const float2 corner = float2((vertex << 1) & 2, vertex & 2);
    return float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 ps_main(float4 position : SV_Position) : SV_Target {
    const uint4 value = source.Load(int3(int2(position.xy), 0));
    const float alpha = tables[AlphaTableOffset + value.a];
    return float4(tables[value.r] * alpha, tables[value.g] * alpha, tables[value.b] * alpha, alpha);
}
