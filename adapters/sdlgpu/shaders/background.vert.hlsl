struct VSOutput
{
    float4 Position : SV_Position;
    float2 Uv : TEXCOORD0;
};

VSOutput main(uint vertexID : SV_VertexID)
{
    // Fullscreen triangle; no vertex buffer. SDL_GPU uses top-left viewport
    // coordinates and converts the NDC convention per backend.
    const float2 positions[3] = {
        float2(-1.0f, -1.0f),
        float2(3.0f, -1.0f),
        float2(-1.0f, 3.0f)
    };
    VSOutput output;
    output.Position = float4(positions[vertexID], 0.0f, 1.0f);
    output.Uv = positions[vertexID] * 0.5f + 0.5f;
    return output;
}
