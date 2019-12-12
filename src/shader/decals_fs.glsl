// ------------------------------------------------------------------
// OUTPUT VARIABLES  ------------------------------------------------
// ------------------------------------------------------------------

layout(location = 0) out vec3 FS_OUT_Albedo;
layout(location = 1) out vec3 FS_OUT_Normal;

// ------------------------------------------------------------------
// INPUT VARIABLES  ------------------------------------------------
// ------------------------------------------------------------------

in vec4 FS_IN_ClipPos;

// ------------------------------------------------------------------
// UNIFORMS ---------------------------------------------------------
// ------------------------------------------------------------------

layout(std140) uniform GlobalUniforms
{
    mat4 view_proj;
    mat4 inv_view_proj;
    vec4 cam_pos;
};

uniform sampler2D s_Depth;
uniform sampler2D s_Decal;
uniform sampler2D s_DecalNormal;

uniform vec4 u_DecalOverlayColor;
uniform mat4 u_DecalVP;
uniform mat4 u_DecalModel;
uniform vec2 u_AspectRatio;

// ------------------------------------------------------------------
// UNIFORM ----------------------------------------------------------
// ------------------------------------------------------------------

vec3 world_position_from_depth(vec2 screen_pos, float ndc_depth)
{
    // Remap depth to [-1.0, 1.0] range.
    float depth = ndc_depth * 2.0 - 1.0;

    // // Create NDC position.
    vec4 ndc_pos = vec4(screen_pos, depth, 1.0);

    // Transform back into world position.
    vec4 world_pos = inv_view_proj * ndc_pos;

    // Undo projection.
    world_pos = world_pos / world_pos.w;

    return world_pos.xyz;
}

// ------------------------------------------------------------------

vec3 get_normal_from_map(vec3 tangent, vec3 bitangent, vec3 normal, vec2 tex_coord, sampler2D normal_map)
{
    // Create TBN matrix.
    mat3 TBN = mat3(normalize(tangent), normalize(bitangent), normalize(normal));

    // Sample tangent space normal vector from normal map and remap it from [0, 1] to [-1, 1] range.
    vec3 n = texture(normal_map, tex_coord).xyz;
    n = normalize(n * 2.0 - 1.0);

    // Multiple vector by the TBN matrix to transform the normal from tangent space to world space.
    n = normalize(TBN * n);

    return n;
}


// ------------------------------------------------------------------
// MAIN -------------------------------------------------------------
// ------------------------------------------------------------------

void main()
{
    vec2 screen_pos = FS_IN_ClipPos.xy / FS_IN_ClipPos.w;
    vec2 tex_coords = screen_pos * 0.5 + 0.5;

    float depth     = texture(s_Depth, tex_coords).x;
    vec3  world_pos = world_position_from_depth(screen_pos, depth);

    vec4 ndc_pos = u_DecalVP * vec4(world_pos, 1.0);
    ndc_pos.xyz /= ndc_pos.w;

    ndc_pos.xy *= u_AspectRatio;

    if (ndc_pos.x < -1.0 || ndc_pos.x > 1.0 || ndc_pos.y < -1.0 || ndc_pos.y > 1.0)
        discard;

    vec2 decal_tex_coord = ndc_pos.xy * 0.5 + 0.5;
    decal_tex_coord.x    = 1.0 - decal_tex_coord.x;

    vec4 albedo = texture(s_Decal, decal_tex_coord) * u_DecalOverlayColor;

    if (albedo.a < 0.1)
        discard;

    vec3 map_normal = texture(s_DecalNormal, decal_tex_coord).xyz;
    
    FS_OUT_Albedo = albedo.rgb;
    FS_OUT_Normal = mat3(u_DecalModel) * map_normal;
}

// ------------------------------------------------------------------
