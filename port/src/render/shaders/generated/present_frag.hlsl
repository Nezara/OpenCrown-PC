Texture2D<float4> _13 : register(t0, space2);
SamplerState __13_sampler : register(s0, space2);

static float4 _9;
static float2 _17;

struct SPIRV_Cross_Input
{
    float2 _17 : TEXCOORD0;
};

struct SPIRV_Cross_Output
{
    float4 _9 : SV_Target0;
};

void frag_main()
{
    _9 = float4(_13.Sample(__13_sampler, _17).xyz, 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    _17 = stage_input._17;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output._9 = _9;
    return stage_output;
}
