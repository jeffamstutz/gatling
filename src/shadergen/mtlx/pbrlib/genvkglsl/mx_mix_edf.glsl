void mx_mix_edf(EDF fg, EDF bg, float w, out EDF result)
{
    result = mix(bg, fg, clamp(w, 0.0, 1.0));
}
