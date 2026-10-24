Texture2D BaseTexture : register(t0, space2);
SamplerState BaseSampler : register(s0, space2);

cbuffer FS : register(b0, space3)
{
    float4 BaseColor;
    float4 ShadeColor;
    float4 Params;         // x: alpha cutoff, y: alpha mode (0/1/2), z: toon strength
    float4 LightDirection; // xyz: direction toward the key light, w: ambient
    float4 CameraPosition; // xyz
    float4 RimColor;
};

struct VSOutput
{
    float4 Position : SV_Position;
    float2 Uv : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float3 WorldPos : TEXCOORD2;
};

float4 main(VSOutput input, bool frontFace : SV_IsFrontFace) : SV_Target0
{
    const float4 texel = BaseTexture.Sample(BaseSampler, input.Uv);
    float4 color = texel * BaseColor;
    const float alphaMode = Params.y;
    if (alphaMode > 0.5f && alphaMode < 1.5f && color.a < Params.x)
    {
        discard;
    }

    float3 normal = normalize(input.Normal);
    if (!frontFace)
    {
        normal = -normal;
    }

    const float3 light = normalize(LightDirection.xyz);
    const float ndl = saturate(dot(normal, light));
    // A restrained two-step toon ramp. VRoid's textures already contain most
    // of the baked form shading, so MToon's shade colour is mixed in gently
    // instead of replacing the texture with a flat colour.
    const float band = smoothstep(0.18f, 0.62f, ndl);
    const float3 shaded = color.rgb * lerp(ShadeColor.rgb, float3(1.0f, 1.0f, 1.0f),
                                          band * 0.65f + 0.15f);
    const float3 lit = shaded * (0.84f + 0.16f * ndl);

    const float3 view = normalize(CameraPosition.xyz - input.WorldPos);
    const float rim = pow(1.0f - saturate(dot(normal, view)), 3.0f);
    const float3 finalColor = saturate(lit + RimColor.rgb * rim);

    if (alphaMode < 0.5f)
    {
        return float4(finalColor, 1.0f);
    }
    return float4(finalColor, color.a);
}
