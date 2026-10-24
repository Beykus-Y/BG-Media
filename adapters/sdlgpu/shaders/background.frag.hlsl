struct VSOutput
{
    float4 Position : SV_Position;
    float2 Uv : TEXCOORD0;
};

float4 main(VSOutput input) : SV_Target0
{
    const float2 centered = input.Uv * 2.0f - 1.0f;
    const float vignette = saturate(1.0f - dot(centered, centered) * 0.32f);
    const float3 top = float3(0.045f, 0.065f, 0.105f);
    const float3 bottom = float3(0.015f, 0.022f, 0.038f);
    const float3 color = lerp(bottom, top, saturate(input.Uv.y * 0.85f + 0.15f));
    // A soft stage glow behind the character.
    const float2 glowOffset = input.Uv - float2(0.5f, 0.58f);
    const float glow = saturate(1.0f - length(glowOffset) * 2.3f);
    const float3 lit = color + float3(0.09f, 0.16f, 0.14f) * glow * glow;
    return float4(lit * vignette, 1.0f);
}
