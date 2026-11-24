// Integrate only the visible part of each ray. The second channel carries
// its scene depth so reconstruction can reject samples across silhouettes.
void writeLightIntegral(uint off, ivec2 pixel) {
    if (any(greaterThanEqual(pixel,ivec2(512)))) return;
    vec2 uv = (vec2(pixel)+0.5)/512.0;
    vec4 camera = lightRow(off,4u);
    vec3 dir = normalize(vec3(uv*camera.xy+camera.zw,1.0));
    vec2 extent = lightInterval(off,dir);
    float rawDepth = textureLod(sampler2D(cs_img3,cs_samp0),uv,0.0).x;
    float z = min(lightParam(off,127u),lightParam(off,51u)/(rawDepth-lightParam(off,50u)));
    float end = min(extent.y,z/dir.z);
    vec3 lightDir = lightRow(off,12u).xyz;
    int cascades = clamp(int(ssbo_1_1.data[off+11u]),1,8);
    float visibility = 0.0;
    vec2 shadowTexel=1.0/vec2(textureSize(cs_img1,0).xy);
    if (end>extent.x) {
        // Adjacent rays use different positions inside each integration interval.
        // The fixed screen-space pattern also works in a paused photo.
        float phase=fract(0.5+float(pixel.x)*0.754877666+float(pixel.y)*0.569840296);
        for (int i=0; i<96; ++i) {
            vec3 pos = dir*mix(extent.x,end,(float(i)+phase)/96.0);
            int cascade = 0;
            while (cascade+1<cascades && pos.z>lightParam(off,120u+uint(cascade))) ++cascade;
            uint base=128u+20u*uint(cascade);
            vec4 pt=vec4(pos,1.0);
            vec2 st=vec2(dot(lightRow(off,base),pt),dot(lightRow(off,base+4u),pt));
            // Sample across the shadow texel footprint as the ray advances.
            vec2 offset=fract((float(i)+phase)*vec2(0.618034,0.414214))-0.5;
            st+=2.0*offset*shadowTexel;
            float shadow=textureLod(sampler2DArray(cs_img1,cs_samp0),vec3(st,float(cascade)),0.0).x;
            vec4 coord=vec4(st,shadow,1.0);
            vec3 blocker=vec3(dot(lightRow(off,base+8u),coord),dot(lightRow(off,base+12u),coord),
                              dot(lightRow(off,base+16u),coord));
            float separation=dot(lightDir,blocker-pos);
            visibility += smoothstep(-0.022,0.022,separation)/96.0;
        }
    }
    imageStore(cs_img4,pixel,vec4(-2.0-visibility,z,0,0));
}
