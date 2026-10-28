cbuffer _33_35 : register(b0, space3)
{
    float4 _35_m0 : packoffset(c0);
    float4 _35_m1 : packoffset(c1);
    float4 _35_m2[3] : packoffset(c2);
    float4 _35_m3[16] : packoffset(c5);
    float _35_m4 : packoffset(c21);
    int _35_m5 : packoffset(c21.y);
};

Texture2D<float4> _204 : register(t0, space2);
SamplerState __204_sampler : register(s0, space2);
Texture2D<float4> _262 : register(t1, space2);
SamplerState __262_sampler : register(s1, space2);

static float4 _189;
static float2 _208;
static float4 _220;
static float2 _264;

struct SPIRV_Cross_Input
{
    float2 _208 : TEXCOORD0;
    float2 _264 : TEXCOORD1;
    float4 _220 : TEXCOORD2;
};

struct SPIRV_Cross_Output
{
    float4 _189 : SV_Target0;
};

void frag_main()
{
    do
    {
        if ((_35_m5 & 4) != 0)
        {
            _189 = _35_m1;
            break;
        }
        if ((_35_m5 & 64) != 0)
        {
            _189 = float4(_204.Sample(__204_sampler, _208).xyz, 1.0f);
            break;
        }
        float4 _871;
        if ((_35_m5 & 1) != 0)
        {
            _871 = _220 * _204.Sample(__204_sampler, _208);
        }
        else
        {
            _871 = _220;
        }
        float4 _872;
        if ((_35_m5 & 16) != 0)
        {
            float3 _245 = _871.xyz * _204.Sample(__204_sampler, _208).xyz;
            float4 _839 = _871;
            _839.x = _245.x;
            _839.y = _245.y;
            _839.z = _245.z;
            _872 = _839;
        }
        else
        {
            _872 = _871;
        }
        float4 _873;
        if ((_35_m5 & 32) != 0)
        {
            _873 = _872 * _262.Sample(__262_sampler, _264);
        }
        else
        {
            _873 = _872;
        }
        float4 _874;
        if ((_35_m5 & 256) != 0)
        {
            float3 _295 = _873.xyz + (((((_873.x + _873.y) + _873.z) * 0.3333333432674407958984375f).xxx - _873.xyz) * _35_m4);
            float4 _848 = _873;
            _848.x = _295.x;
            _848.y = _295.y;
            _848.z = _295.z;
            _874 = _848;
        }
        else
        {
            _874 = _873;
        }
        float4 _875;
        if ((_35_m5 & 128) != 0)
        {
            float4 _854 = _874;
            _854.x = dot(_35_m2[0].xyz, _874.xyz) * _35_m2[0].w;
            _854.y = dot(_35_m2[1].xyz, _874.xyz) * _35_m2[1].w;
            _854.z = dot(_35_m2[2].xyz, _874.xyz) * _35_m2[2].w;
            _875 = _854;
        }
        else
        {
            _875 = _874;
        }
        float4 _880;
        if ((_35_m5 & 512) != 0)
        {
            float3 _350 = _875.xyz;
            float3 _417;
            for (int _876 = 0; _876 < 3; _876++)
            {
                int _504 = _876 >> 2;
                int _506 = _876 & 3;
                int _512 = (8 + _876) >> 2;
                float _886;
                do
                {
                    if (_35_m3[_512][_506] != 0.0f)
                    {
                        _886 = clamp((_350[_876] - _35_m3[_504][_506]) / _35_m3[_512][_506], 0.0f, 1.0f);
                        break;
                    }
                    _886 = float((_350[_876] > 0.0f) && (_35_m3[_504][_506] < 0.0f));
                    break;
                } while(false);
                int _543 = (4 + _876) >> 2;
                int _551 = (12 + _876) >> 2;
                float _888;
                do
                {
                    if (_35_m3[_551][_506] != 0.0f)
                    {
                        _888 = clamp((_350[_876] - _35_m3[_543][_506]) / _35_m3[_551][_506], 0.0f, 1.0f);
                        break;
                    }
                    _888 = float((_350[_876] > 0.0f) && (_35_m3[_543][_506] < 0.0f));
                    break;
                } while(false);
                _417[_876] = _350[_876] + ((((_35_m3[(16 + _876) >> 2][_506] + (_35_m3[(20 + _876) >> 2][_506] * _886)) + (_35_m3[(24 + _876) >> 2][_506] * _888)) - _350[_876]) * (_35_m3[(28 + _876) >> 2][_506] * _35_m3[7].w));
            }
            float3 _355 = _417;
            float3 _636;
            for (int _877 = 0; _877 < 3; _877++)
            {
                int _723 = (32 + _877) >> 2;
                int _725 = _877 & 3;
                int _731 = (40 + _877) >> 2;
                float _882;
                do
                {
                    if (_35_m3[_731][_725] != 0.0f)
                    {
                        _882 = clamp((_355[_877] - _35_m3[_723][_725]) / _35_m3[_731][_725], 0.0f, 1.0f);
                        break;
                    }
                    _882 = float((_355[_877] > 0.0f) && (_35_m3[_723][_725] < 0.0f));
                    break;
                } while(false);
                int _762 = (36 + _877) >> 2;
                int _770 = (44 + _877) >> 2;
                float _884;
                do
                {
                    if (_35_m3[_770][_725] != 0.0f)
                    {
                        _884 = clamp((_355[_877] - _35_m3[_762][_725]) / _35_m3[_770][_725], 0.0f, 1.0f);
                        break;
                    }
                    _884 = float((_355[_877] > 0.0f) && (_35_m3[_762][_725] < 0.0f));
                    break;
                } while(false);
                _636[_877] = _355[_877] + ((((_35_m3[(48 + _877) >> 2][_725] + (_35_m3[(52 + _877) >> 2][_725] * _882)) + (_35_m3[(56 + _877) >> 2][_725] * _884)) - _355[_877]) * (_35_m3[(60 + _877) >> 2][_725] * _35_m3[15].w));
            }
            float4 _860 = _875;
            _860.x = _636.x;
            _860.y = _636.y;
            _860.z = _636.z;
            _880 = _860;
        }
        else
        {
            _880 = _875;
        }
        float4 _881;
        if ((_35_m5 & 2) != 0)
        {
            float3 _381 = _880.xyz + ((_35_m0.xyz - _880.xyz) * _35_m0.w);
            float4 _866 = _880;
            _866.x = _381.x;
            _866.y = _381.y;
            _866.z = _381.z;
            _881 = _866;
        }
        else
        {
            _881 = _880;
        }
        _189 = _881;
        break;
    } while(false);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    _208 = stage_input._208;
    _220 = stage_input._220;
    _264 = stage_input._264;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output._189 = _189;
    return stage_output;
}
