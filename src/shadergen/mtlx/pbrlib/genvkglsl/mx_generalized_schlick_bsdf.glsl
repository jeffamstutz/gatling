#include "pbrlib/genglsl/lib/mx_microfacet_specular.glsl"

void mx_generalized_schlick_bsdf(float weight, vec3 color0, vec3 color90, float exponent, vec2 roughness, vec3 N, vec3 X, int distribution, int scatter_mode, BSDF base, thinfilm tf, out BSDF result)
{
    if (weight < M_FLOAT_EPS)
    {
        result = base;
        return;
    }

    N = mx_forward_facing_normal(N, V);

    vec3 Y = normalize(cross(N, X));
    vec3 H = normalize(L + V);

    float NdotL = clamp(dot(N, L), M_FLOAT_EPS, 1.0);
    float NdotV = clamp(dot(N, V), M_FLOAT_EPS, 1.0);
    float NdotH = clamp(dot(N, H), M_FLOAT_EPS, 1.0);
    float VdotH = clamp(dot(V, H), M_FLOAT_EPS, 1.0);

    float avgRoughness = mx_average_roughness(roughness);

    FresnelData fd = mx_init_fresnel_schlick(color0, color90, exponent);
    vec3  F = mx_compute_fresnel(VdotH, fd);
    float D = mx_ggx_NDF(X, Y, H, NdotH, roughness.x, roughness.y);
    float G = mx_ggx_smith_G(NdotL, NdotV, avgRoughness);

    vec3 comp = mx_ggx_energy_compensation(NdotV, avgRoughness, F);
    vec3 dirAlbedo = mx_ggx_directional_albedo(NdotV, avgRoughness, color0, color90) * comp;
    float avgDirAlbedo = dot(dirAlbedo, vec3(1.0 / 3.0));

    // Note: NdotL is cancelled out
    result = D * F * G * comp * weight / (4 * NdotV)    // Top layer reflection
           + base * (1.0 - avgDirAlbedo * weight);                  // Base layer reflection attenuated by top layer
}
