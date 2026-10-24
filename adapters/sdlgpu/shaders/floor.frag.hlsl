cbuffer FS : register(b0, space3)
{
    float4 CameraPosition; // xyz, w unused
    float4 Shadow;         // x, z, radius, strength
};

struct VSOutput
{
    float4 Position : SV_Position;
    float3 WorldPos : TEXCOORD0;
};

float gridLine(float2 p)
{
    const float2 derivative = fwidth(p);
    const float2 grid = abs(frac(p - 0.5f) - 0.5f) / max(derivative, float2(1.0e-5f, 1.0e-5f));
    return 1.0f - saturate(min(grid.x, grid.y));
}

float4 main(VSOutput input) : SV_Target0
{
    const float2 p = input.WorldPos.xz;
    const float2 fromCamera = CameraPosition.xz - p;
    const float radial = saturate(1.0f - length(fromCamera) / 7.5f);
    const float minor = gridLine(p * 4.0f);
    const float major = gridLine(p * 0.5f);
    const float3 base = float3(0.030f, 0.045f, 0.060f);
    const float3 color = base + float3(0.080f, 0.230f, 0.205f) * major * 0.45f +
                         float3(0.045f, 0.110f, 0.105f) * minor * 0.28f;
    const float2 shadowDelta = p - Shadow.xy;
    const float shadow = exp(-dot(shadowDelta, shadowDelta) /
                             max(0.0001f, Shadow.z * Shadow.z));
    const float3 finalColor = lerp(color, float3(0.008f, 0.012f, 0.018f),
                                   shadow * Shadow.w) *
                              radial;
    return float4(finalColor, 1.0f);
}
