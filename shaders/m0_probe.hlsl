// M0 only: pixel-aligned quads and explicit linear-premultiplied materials.
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
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex vs_main(uint id : SV_VertexID) {
    const float2 corners[6] = {
        float2(0,0), float2(1,0), float2(0,1),
        float2(0,1), float2(1,0), float2(1,1)
    };
    float2 p = corners[id];
    Vertex v;
    v.position = float4((rect.xy + p * rect.zw) / targetSize * float2(2,-2) + float2(-1,1), 0, 1);
    v.uv = uvRect.xy + p * uvRect.zw;
    return v;
}
float4 ps_main(Vertex v) : SV_Target {
    if (material == 5) {
        // D2D writes encoded premultiplied BGRA8. Decode each texel BEFORE
        // the later UI bilinear filter; preserve alpha and premultiplication.
        float4 value = sourceTexture.Load(int3(int2(v.position.xy),0));
        float3 encoded = value.a > 0 ? saturate(value.rgb / value.a) : 0;
        float3 decoded = float3(encoded.r <= 0.04045 ? encoded.r / 12.92 : pow((encoded.r + 0.055)/1.055,2.4),
                               encoded.g <= 0.04045 ? encoded.g / 12.92 : pow((encoded.g + 0.055)/1.055,2.4),
                               encoded.b <= 0.04045 ? encoded.b / 12.92 : pow((encoded.b + 0.055)/1.055,2.4));
        return float4(decoded * value.a,value.a);
    }
    if (material == 0) return linearPremultipliedColor * opacity;
    if (material == 1) return linearPremultipliedColor * sourceTexture.Sample(linearClamp, v.uv).r * opacity;
    if (material == 3) {
        // FreeType encodes contour zero at byte 128, not normalized 0.5.
        // Distance is linear data; no sRGB transfer, no coverage interpretation.
        float d = (sourceTexture.SampleLevel(linearClamp, v.uv, 0).r * 255.0 - 128.0) * sdfParams.x / 128.0;
        float aa = max(fwidth(d), 0.0001);
        float fill = saturate(0.5 + d / aa);
        float outer = saturate(0.5 + (d + sdfParams.y) / aa);
        return (linearPremultipliedColor * fill + linearPremultipliedOutline * (outer - fill)) * opacity;
    }
    float2 sampleUV = v.uv;
    if (material == 4) sampleUV.y = 1.0 - sampleUV.y;
    return sourceTexture.Sample(linearClamp, sampleUV) * opacity;
}
