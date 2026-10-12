// The parameter the time names, as linear premultiplied color: an instance per parameter checks the record field that
// carries each one, and the time.
float4 effect(EffectInput input) {
    return input.parameters[(uint)input.time];
}
