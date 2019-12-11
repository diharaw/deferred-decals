// ------------------------------------------------------------------
// OUTPUT VARIABLES  ------------------------------------------------
// ------------------------------------------------------------------

layout(location = 0) out vec3 FS_OUT_Albedo;
layout(location = 1) out vec3 FS_OUT_Normal;

// ------------------------------------------------------------------
// INPUT VARIABLES  -------------------------------------------------
// ------------------------------------------------------------------

in vec3 FS_IN_Normal;
in vec3 FS_IN_Tangent;
in vec3 FS_IN_Bitangent;
in vec2 FS_IN_TexCoord;

// ------------------------------------------------------------------
// UNIFORMS ---------------------------------------------------------
// ------------------------------------------------------------------

uniform sampler2D s_Albedo;
uniform sampler2D s_Normal;

// ------------------------------------------------------------------
// MAIN -------------------------------------------------------------
// ------------------------------------------------------------------

vec3 get_normal_from_map(vec3 tangent, vec3 bitangent, vec3 normal, vec2 tex_coord, sampler2D normal_map)
{
	// Create TBN matrix.
    mat3 TBN = mat3(normalize(tangent), normalize(bitangent), normalize(normal));

    // Sample tangent space normal vector from normal map and remap it from [0, 1] to [-1, 1] range.
    vec3 n = texture(normal_map, tex_coord).xyz;

    n.y = 1.0 - n.y;

    n = normalize(n * 2.0 - 1.0);

    // Multiple vector by the TBN matrix to transform the normal from tangent space to world space.
    n = normalize(TBN * n);

    return n;
}

void main()
{
    vec4 diffuse = texture(s_Albedo, FS_IN_TexCoord);

    vec3 N = normalize(FS_IN_Normal);
    vec3 T = normalize(FS_IN_Tangent);
    vec3 B = normalize(FS_IN_Bitangent);

    FS_OUT_Albedo = diffuse.xyz;
    FS_OUT_Normal = get_normal_from_map(T, B, N, FS_IN_TexCoord, s_Normal);
}

// ------------------------------------------------------------------
