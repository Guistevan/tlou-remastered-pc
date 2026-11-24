vec4 spotRow(uint off,uint i){
    return vec4(uintBitsToFloat(ssbo_1_1.data[off+i]),uintBitsToFloat(ssbo_1_1.data[off+i+1u]),uintBitsToFloat(ssbo_1_1.data[off+i+2u]),uintBitsToFloat(ssbo_1_1.data[off+i+3u]));
}
void integrateSpot(uint off,ivec2 pixel){
    vec2 uv=(vec2(pixel)+0.5)/512.0;
    vec4 projection=spotRow(off,32u), source=spotRow(off,0u), axis=spotRow(off,4u);
    vec3 direction=normalize(vec3((uv-projection.zw)/projection.xy,1.0));
    vec4 depthParams=spotRow(off,28u);
    float rawDepth=textureLod(sampler2D(cs_img2,cs_samp0),uv,0.0).x;
    float z=clamp(depthParams.w/(rawDepth-depthParams.z),0.0,60000.0);
    float outer=spotRow(off,16u).w;
    float dv=dot(direction,axis.xyz),ov=dot(source.xyz,axis.xyz);
    float od=dot(source.xyz,direction),oo=dot(source.xyz,source.xyz);
    float a=dv*dv-outer*outer,b=dv*ov-od*outer*outer,c=ov*ov-oo*outer*outer;
    float t0=0.0,t1=0.0;
    float discr=b*b-a*c,sphere=od*od-oo+source.w*source.w;
    if(discr>0.0 && abs(a)>1e-7 && sphere>0.0){
        float x=(b-sqrt(discr))/a,y=(b+sqrt(discr))/a;
        float cap=dv>0.0?10000.0:0.0;
        if(x*dv<ov)x=cap;
        if(y*dv<ov)y=cap;
        t0=max(0.0,max(min(x,y),od-sqrt(sphere)));
        t1=min(z/direction.z,min(max(x,y),od+sqrt(sphere)));
    }
    float sum=0.0,weights=0.0;
    if(t1>t0){
        vec4 px=spotRow(off,36u),py=spotRow(off,40u),pw=spotRow(off,44u);
        vec2 texel=1.0/vec2(textureSize(cs_img1,0));
        float phase=fract(0.5+dot(vec2(pixel),vec2(0.754877666,0.569840296)));
        for(int i=0;i<96;++i){
            vec4 pt=vec4(direction*mix(t0,t1,(float(i)+phase)/96.0),1.0);
            vec3 fromLight=pt.xyz-source.xyz;
            float distance2=dot(fromLight,fromLight);
            float forward=dot(axis.xyz,fromLight);
            float w=max(0.0,forward*inversesqrt(max(distance2,1e-8))-outer)/max(distance2,0.04*source.w*source.w);
            float clipW=dot(pw,pt);
            if(clipW<=0.0) continue;
            vec2 st=vec2(dot(px,pt),dot(py,pt))/clipW;
            vec2 offset=fract((float(i)+phase)*vec2(0.618034,0.414214))-0.5;
            float raw=textureLod(sampler2D(cs_img1,cs_samp0),clamp(st+offset*texel,0.0,1.0),0.0).x;
            float blocker=depthParams.y/(raw-depthParams.x);
            sum+=w*smoothstep(-0.035,0.035,blocker-forward);
            weights+=w;
        }
    }
    imageStore(cs_img3,pixel,vec4(-2.0-sum/max(weights,1e-15),z,0.0,0.0));
}
