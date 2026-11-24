float volumeVisibility(vec2 uv,float z) {
    vec2 coord=clamp(uv*512.0-0.5,vec2(0),vec2(511));
    ivec2 cell=ivec2(coord);
    vec2 f=fract(coord);
    float sum=0.0,weight=0.0;
    float limit=lightParam((bitfieldExtract(push_data.buf_offsets0.x,0,8)>>2u),127u);
    z=min(z,limit);
    for(int y=-2;y<=3;++y) for(int x=-2;x<=3;++x) {
        vec2 v=texelFetch(fs_img2,clamp(cell+ivec2(x,y),ivec2(0),ivec2(511)),0).rg;
        float delta=(v.y-z)/max(0.08,0.015*abs(z));
        // Filter visibility in screen space, respecting view depth at silhouettes.
        vec2 dist=vec2(x,y)-f;
        float w=exp(-0.5*dot(dist,dist)/1.44)/(1.0+delta*delta*delta*delta);
        sum+=w*(-2.0-v.x);
        weight+=w;
    }
    return clamp(sum/max(weight,1e-15),0.0,1.0);
}
