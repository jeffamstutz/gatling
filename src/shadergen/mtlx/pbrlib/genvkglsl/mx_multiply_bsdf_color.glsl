void mx_multiply_bsdf_color(BSDF in1, vec3 in2, out BSDF result)
{
    result = in1 * clamp(in2, 0.0, 1.0);
}
