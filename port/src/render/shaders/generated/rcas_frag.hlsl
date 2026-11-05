cbuffer _663_665 : register(b0, space3)
{
    uint4 _665_m0 : packoffset(c0);
    int4 _665_m1 : packoffset(c1);
};

Texture2D<float4> _123 : register(t0, space2);
SamplerState __123_sampler : register(s0, space2);

static float4 gl_FragCoord;
static float4 _685;

struct SPIRV_Cross_Input
{
    float4 gl_FragCoord : SV_Position;
};

struct SPIRV_Cross_Output
{
    float4 _685 : SV_Target0;
};

uint2 spvTextureSize(Texture2D<float4> Tex, uint Level, out uint Param)
{
    uint2 ret;
    Tex.GetDimensions(Level, ret.x, ret.y, Param);
    return ret;
}

void frag_main()
{
    int2 _670 = int2(gl_FragCoord.xy) - _665_m1.xy;
    uint _1193_dummy_parameter;
    float4 _1198 = _123.Load(int3(clamp(_670 + int2(0, -1), int2(0, 0), int2(spvTextureSize(_123, uint(0), _1193_dummy_parameter)) - int2(1, 1)), 0));
    uint _1205_dummy_parameter;
    float4 _1210 = _123.Load(int3(clamp(_670 + int2(-1, 0), int2(0, 0), int2(spvTextureSize(_123, uint(0), _1205_dummy_parameter)) - int2(1, 1)), 0));
    uint _1217_dummy_parameter;
    float4 _1222 = _123.Load(int3(clamp(_670, int2(0, 0), int2(spvTextureSize(_123, uint(0), _1217_dummy_parameter)) - int2(1, 1)), 0));
    uint _1229_dummy_parameter;
    float4 _1234 = _123.Load(int3(clamp(_670 + int2(1, 0), int2(0, 0), int2(spvTextureSize(_123, uint(0), _1229_dummy_parameter)) - int2(1, 1)), 0));
    uint _1241_dummy_parameter;
    float4 _1246 = _123.Load(int3(clamp(_670 + int2(0, 1), int2(0, 0), int2(spvTextureSize(_123, uint(0), _1241_dummy_parameter)) - int2(1, 1)), 0));
    float _846 = _1198.x;
    float _848 = _1198.y;
    float _850 = _1198.z;
    float _852 = _1210.x;
    float _854 = _1210.y;
    float _856 = _1210.z;
    float _858 = _1222.x;
    float _860 = _1222.y;
    float _862 = _1222.z;
    float _864 = _1234.x;
    float _866 = _1234.y;
    float _868 = _1234.z;
    float _870 = _1246.x;
    float _872 = _1246.y;
    float _874 = _1246.z;
    float _1002 = min(min(_846, min(_852, _864)), _870);
    float _1008 = min(min(_848, min(_854, _866)), _872);
    float _1014 = min(min(_850, min(_856, _868)), _874);
    float _1020 = max(max(_846, max(_852, _864)), _870);
    float _1026 = max(max(_848, max(_854, _866)), _872);
    float _1032 = max(max(_850, max(_856, _868)), _874);
    float _1122 = max(-0.1875f, min(max(max(-(min(_1002, _858) * (0.25f / _1020)), (1.0f - max(_1020, _858)) * (1.0f / ((4.0f * _1002) + (-4.0f)))), max(max(-(min(_1008, _860) * (0.25f / _1026)), (1.0f - max(_1026, _860)) * (1.0f / ((4.0f * _1008) + (-4.0f)))), max(-(min(_1014, _862) * (0.25f / _1032)), (1.0f - max(_1032, _862)) * (1.0f / ((4.0f * _1014) + (-4.0f)))))), 0.0f)) * asfloat(_665_m0.x);
    float _1127 = (4.0f * _1122) + 1.0f;
    float _1508 = asfloat(2129764351u - asuint(_1127));
    float _1516 = _1508 * (((-_1508) * _1127) + 2.0f);
    _685 = float4(((_1122 * (((_846 + _852) + _870) + _864)) + _858) * _1516, ((_1122 * (((_848 + _854) + _872) + _866)) + _860) * _1516, ((_1122 * (((_850 + _856) + _874) + _868)) + _862) * _1516, 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    gl_FragCoord = stage_input.gl_FragCoord;
    gl_FragCoord.w = 1.0 / gl_FragCoord.w;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output._685 = _685;
    return stage_output;
}
