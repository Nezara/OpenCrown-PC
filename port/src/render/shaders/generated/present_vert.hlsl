static float4 gl_Position;
static int gl_VertexIndex;
static float2 _48;

struct SPIRV_Cross_Input
{
    uint gl_VertexIndex : SV_VertexID;
};

struct SPIRV_Cross_Output
{
    float2 _48 : TEXCOORD0;
    float4 gl_Position : SV_Position;
};

void vert_main()
{
    float _18 = float((gl_VertexIndex << 1) & 2);
    float _21 = float(gl_VertexIndex & 2);
    gl_Position = float4((_18 * 2.0f) - 1.0f, 1.0f - (_21 * 2.0f), 0.0f, 1.0f);
    _48 = float2(_18, _21);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    gl_VertexIndex = int(stage_input.gl_VertexIndex);
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output._48 = _48;
    return stage_output;
}
