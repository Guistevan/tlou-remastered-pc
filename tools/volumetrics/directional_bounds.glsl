float lightParam(uint offset, uint index) {
    return uintBitsToFloat(ssbo_1_1.data[offset + index]);
}
vec4 lightRow(uint offset, uint index) {
    return vec4(lightParam(offset,index),lightParam(offset,index+1u),
                lightParam(offset,index+2u),lightParam(offset,index+3u));
}
vec2 lightInterval(uint offset, vec3 dir) {
    vec4 a = lightRow(offset,104u), b = lightRow(offset,108u), c = lightRow(offset,112u);
    vec3 rate = vec3(dot(a.xyz,dir),dot(b.xyz,dir),dot(c.xyz,dir));
    vec3 inv = 1.0 / mix(rate, vec3(1e-12), lessThan(abs(rate), vec3(1e-12)));
    float capB = lightParam(offset,116u), capC = lightParam(offset,117u);
    float ta = -a.w * inv.x;
    vec2 tb = vec2(-b.w,capB) * inv.y, tc = vec2(-c.w,capC) * inv.z;
    float start = max(0.0,max(inv.x > 0.0 ? -100000.0 : ta,max(min(tb.x,tb.y),min(tc.x,tc.y))));
    float end = min(lightParam(offset,127u)/dir.z,min(inv.x > 0.0 ? ta : 100000.0,
                    min(max(tb.x,tb.y),max(tc.x,tc.y))));
    return vec2(start, max(start,end));
}
