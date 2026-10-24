cbuffer VS : register(b0, space1)
{
    float4x4 ViewProj;
    float4 FloorParams; // x: size, y: model height
};

struct VSOutput
{
    float4 Position : SV_Position;
    float3 WorldPos : TEXCOORD0;
};

VSOutput main(uint vertexID : SV_VertexID)
{
    const float2 corners[6] = {
        float2(-1.0f, -1.0f),
        float2( 1.0f, -1.0f),
        float2( 1.0f,  1.0f),
        float2(-1.0f, -1.0f),
        float2( 1.0f,  1.0f),
        float2(-1.0f,  1.0f)
    };
    const float halfSize = max(2.0f, FloorParams.x);
    const float3 position = float3(corners[vertexID].x * halfSize, 0.0f,
                                   corners[vertexID].y * halfSize);
    VSOutput output;
    output.Position = mul(ViewProj, float4(position, 1.0f));
    output.WorldPos = position;
    return output;
}
