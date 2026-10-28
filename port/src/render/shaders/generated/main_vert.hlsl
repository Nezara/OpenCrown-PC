cbuffer _12_14 : register(b0, space1)
{
    row_major float4x4 _14_m0 : packoffset(c0);
    int _14_m1 : packoffset(c4);
};


static float4 gl_Position;
static float4 _25;
static float2 _57;
static float2 _59;
static float2 _61;
static float2 _62;
static float4 _64;
static float4 _65;

struct SPIRV_Cross_Input
{
    float4 _25 : TEXCOORD0;
    float4 _65 : TEXCOORD1;
    float2 _59 : TEXCOORD2;
    float2 _62 : TEXCOORD3;
};

struct SPIRV_Cross_Output
{
    float2 _57 : TEXCOORD0;
    float2 _61 : TEXCOORD1;
    float4 _64 : TEXCOORD2;
    float4 gl_Position : SV_Position;
};

void vert_main()
{
    float4 _71;
    if (_14_m1 == 1)
    {
        _71 = _25;
    }
    else
    {
        _71 = mul(_25, _14_m0);
    }
    float4 _70 = _71;
    _70.z = (_71.z + _71.w) * 0.5f;
    gl_Position = _70;
    _57 = _59;
    _61 = _62;
    _64 = _65;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    _25 = stage_input._25;
    _59 = stage_input._59;
    _62 = stage_input._62;
    _65 = stage_input._65;
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output._57 = _57;
    stage_output._61 = _61;
    stage_output._64 = _64;
    return stage_output;
}
