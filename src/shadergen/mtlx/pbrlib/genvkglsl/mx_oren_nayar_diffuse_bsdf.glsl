#include "pbrlib/genglsl/lib/mx_microfacet_diffuse.glsl"

void mx_oren_nayar_diffuse_bsdf(float weight, vec3 color, float roughness, vec3 normal, out BSDF result)
{
    if (weight < M_FLOAT_EPS)
    {
        result = BSDF(0.0);
        return;
    }

    normal = mx_forward_facing_normal(normal, V);

    float NdotL = clamp(dot(normal, L), M_FLOAT_EPS, 1.0);

    result = color * weight * M_PI_INV;
    if (roughness > 0.0)
    {
        result *= mx_oren_nayar_diffuse(L, V, normal, NdotL, roughness);
    }
}
