// The resolve of an export image (include/weft/render_export_texture.h, docs/ingame-ui.md): a fullscreen triangle over
// the image's top level that reads the scene pass's scratch target at its pixel, encoded premultiplied sRGB as every
// scene target holds, and writes it linear premultiplied: straight, decoded through the sRGB transfer function and
// premultiplied again, as srgb_convert.hlsl's ENCODED_PREMULTIPLIED variant converts Direct2D's targets. An sRGB image
// encodes the result on the write, a float image stores it.

Texture2D encoded : register(t0);

struct Fullscreen {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Fullscreen vs_main(uint vertex : SV_VertexID) {
    Fullscreen v;
    v.uv = float2((vertex << 1) & 2, vertex & 2);
    v.position = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return v;
}

// IEC 61966-2-1 sRGB transfer function, encoded to linear. A scalar condition: DXC takes no vector condition in ?:.
float toLinear(float c) {
    return c <= .04045 ? c / 12.92 : pow(max((c + .055) / 1.055, 0), 2.4);
}

float4 ps_main(Fullscreen v) : SV_Target {
    const float4 c = encoded.Load(int3(int2(v.position.xy), 0));
    if (c.a <= 0) {
        return float4(0, 0, 0, 0);
    }
    const float3 straight = saturate(c.rgb / c.a);
    return float4(toLinear(straight.r) * c.a, toLinear(straight.g) * c.a, toLinear(straight.b) * c.a, c.a);
}
