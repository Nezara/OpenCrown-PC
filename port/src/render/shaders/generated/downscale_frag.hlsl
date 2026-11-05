cbuffer _19_21 : register(b0, space3)
{
    float4 _21_m0 : packoffset(c0);
    int4 _21_m1 : packoffset(c1);
};

Texture2D<float4> _101 : register(t0, space2);
SamplerState __101_sampler : register(s0, space2);

static float4 gl_FragCoord;
static float4 _117;

struct SPIRV_Cross_Input
{
    float4 gl_FragCoord : SV_Position;
};

struct SPIRV_Cross_Output
{
    float4 _117 : SV_Target0;
};

void frag_main()
{
    float2 _28 = float2(int2(gl_FragCoord.xy) - _21_m1.xy);
    int2 _48 = clamp(int2(ceil(_21_m0.xy)), int2(1, 1), int2(4, 4));
    int _136;
    float3 _137;
    _137 = 0.0f.xxx;
    _136 = 0;
    float3 _140;
    int _65;
    for (;;)
    {
        _65 = _48.y;
        bool _67 = _136 < _65;
        if (_67)
        {
            _140 = _137;
            for (int _138 = 0; _138 < _48.x; )
            {
                _140 += _101.Sample(__101_sampler, (_21_m0.xy * (_28 + ((float2(float(_138), float(_136)) + 0.5f.xx) / float2(_48)))) * _21_m0.zw).xyz;
                _138++;
                continue;
            }
            _137 = _140;
            _136++;
            continue;
        }
        else
        {
            break;
        }
    }
    _117 = float4(_137 / float(_48.x * _65).xxx, 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    gl_FragCoord = stage_input.gl_FragCoord;
    gl_FragCoord.w = 1.0 / gl_FragCoord.w;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output._117 = _117;
    return stage_output;
}
