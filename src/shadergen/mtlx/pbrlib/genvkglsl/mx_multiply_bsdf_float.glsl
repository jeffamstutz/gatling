void mx_multiply_bsdf_float(BSDF in1, float in2, out BSDF result)
{
    result = in1 * clamp(in2, 0.0, 1.0);
}
