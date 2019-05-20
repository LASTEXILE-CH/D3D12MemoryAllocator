Texture2D t0 : register(t0);
SamplerState s0 : register(s0);

struct VS_OUTPUT
{
    float4 pos: SV_POSITION;
    float2 texCoord: TEXCOORD;
    float4 color: COLOR;
};

float4 main(VS_OUTPUT input) : SV_TARGET
{
    return t0.Sample(s0, input.texCoord) * input.color;
}
