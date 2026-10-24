struct VSInput
{
    float3 Position : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float2 Uv : TEXCOORD2;
    uint4 Joints : TEXCOORD3;
    float4 Weights : TEXCOORD4;
};

struct VSOutput
{
    float4 Position : SV_Position;
    float2 Uv : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float3 WorldPos : TEXCOORD2;
};

cbuffer VS : register(b0, space1)
{
    float4x4 ViewProj;
    float4x4 JointMatrices[192];
};

VSOutput main(VSInput input)
{
    float4 position = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float3 normal = float3(0.0f, 0.0f, 0.0f);

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        const float4x4 joint = JointMatrices[input.Joints[i]];
        position += mul(joint, float4(input.Position, 1.0f)) * input.Weights[i];
        normal += mul((float3x3)joint, input.Normal) * input.Weights[i];
    }

    VSOutput output;
    output.Position = mul(ViewProj, position);
    output.WorldPos = position.xyz;
    output.Normal = normal;
    output.Uv = input.Uv;
    return output;
}
