#include "pbrlib/genglsl/lib/mx_microfacet_specular.glsl"

void mx_dielectric_bsdf(float weight, vec3 tint, float ior, vec2 roughness, vec3 N, vec3 X, int distribution, int scatter_mode, BSDF base, thinfilm tf, out BSDF result)
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

    FresnelData fd = tf.thickness > 0.0 ? mx_init_fresnel_dielectric_airy(ior, tf.thickness, tf.ior) : mx_init_fresnel_dielectric(ior);
    vec3  F = mx_compute_fresnel(VdotH, fd);
    float D = mx_ggx_NDF(X, Y, H, NdotH, roughness.x, roughness.y);
    float G = mx_ggx_smith_G(NdotL, NdotV, avgRoughness);

    float F0 = mx_ior_to_f0(ior);
    vec3 comp = mx_ggx_energy_compensation(NdotV, avgRoughness, F);
    vec3 dirAlbedo = mx_ggx_directional_albedo(NdotV, avgRoughness, F0, 1.0) * comp;

    // Note: NdotL is cancelled out
    result = D * F * G * comp * tint * weight / (4 * NdotV) // Top layer reflection
           + base * (1.0 - dirAlbedo * weight);                         // Base layer reflection attenuated by top layer
}
