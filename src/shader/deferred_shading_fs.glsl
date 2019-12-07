// ------------------------------------------------------------------
// INPUT VARIABLES  -------------------------------------------------
// ------------------------------------------------------------------

in vec2 FS_IN_TexCoord;

// ------------------------------------------------------------------
// OUTPUT VARIABLES  ------------------------------------------------
// ------------------------------------------------------------------

out vec4 FS_OUT_Color;

// ------------------------------------------------------------------
// UNIFORMS  --------------------------------------------------------
// ------------------------------------------------------------------

layout(std140) uniform GlobalUniforms
{
    mat4 view_proj;
    vec4 cam_pos;
};

uniform sampler2D s_Depth;
uniform sampler2D s_Normals;
uniform sampler2D s_Albedo;

// ------------------------------------------------------------------
// FUNCTIONS  -------------------------------------------------------
// ------------------------------------------------------------------

// ------------------------------------------------------------------
// MAIN  ------------------------------------------------------------
// ------------------------------------------------------------------

const float kAmbient = 0.1;

void main(void)
{
    vec3 dir = normalize(vec3(0.0, 1.0, 1.0));

    vec3 albedo  = texture(s_Albedo, FS_IN_TexCoord).rgb;
    vec3 normal  = texture(s_Normals, FS_IN_TexCoord).rgb;

    vec3 color = albedo * max(dot(normal, dir), 0.0) + albedo * kAmbient;

    FS_OUT_Color = vec4(color, 1.0);
}

// ------------------------------------------------------------------
