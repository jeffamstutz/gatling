void mx_mix_bsdf(BSDF fg, BSDF bg, float w, out BSDF result)
{
    result = mix(bg, fg, clamp(w, 0.0, 1.0));
}
