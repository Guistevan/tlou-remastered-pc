        float _711, _667;
        if (texelFetch(fs_img2,ivec2(0),0).x <= -1.99) {
            vec2 extent=lightInterval(buf0_dword_off,vec3(_221,_224,_164));
            float lengthInVolume=max(0.0,extent.y-extent.x);
            _667=clamp(_294-extent.x,0.0,lengthInVolume);
            _711=volumeVisibility(vec2(_141,_143),_292);
        } else {
        // Reconstruct at the scene pixel's depth. Each epipolar line has its
        // own basis and angular step, so neighboring rows need their own UV.
        vec3 viewDirection = vec3(_221, _224, _164);
        float sceneDepth = _292;
        float rayCount = _425;
        // A full sweep covers unoriented epipolar planes (period pi). Use
        // a periodic grid whose interval count divides the sweep exactly.
        float angleScale = uintBitsToFloat(ssbo_1_1.data[25u + buf0_dword_off]);
        bool periodic = abs(abs(angleScale) * 3.14159265 - 1.0) < 0.001;
        float rayCoordinate = periodic ? rayCount * fract(_423) : _426;
        float gridCount = max(1.0, round(rayCount / max(1.0, round(rayCount / 170.0))));
        float spacing = rayCount / gridCount;
        float gridPosition = rayCoordinate / max(spacing, 1e-6);
        float sumLight = 0.0;
        float sumWeight = 0.0;
        float totalAngularWeight = 0.0;
        for (int offset = -5; offset <= 5; ++offset) {
            float gridPoint = floor(gridPosition + 0.5) + float(offset);
            float lookup = periodic ? mod(gridPoint, gridCount) : gridPoint;
            int row = int(clamp(round(lookup * spacing), 0.0, max(0.0, rayCount - 1.0)));
            vec4 axis = texelFetch(fs_img1, ivec2(row, 2), 0);
            vec3 tangent = texelFetch(fs_img1, ivec2(row, 3), 0).xyz;
            if (!(axis.w > 0.0)) continue;
            float x = dot(axis.xyz, viewDirection);
            float y = dot(tangent, viewDirection);
            float angle = x == 0.0 ? (y == 0.0 ? 0.0 : 1.57079632679) : atan(abs(y), x);
            float column = clamp(angle / axis.w - 0.5, 0.0, 511.0);
            int left = int(column);
            vec2 a = texelFetch(fs_img2, ivec2(left, row), 0).rg;
            vec2 b = texelFetch(fs_img2, ivec2(min(left + 1, 511), row), 0).rg;
            // Keep depth confidence through the angular accumulation. Normalizing
            // every row separately gives a background-only row full voting weight.
            float tolerance = max(0.12, 0.045 * abs(sceneDepth));
            vec2 delta = (vec2(sceneDepth) - vec2(a.y, b.y)) / tolerance;
            vec2 confidence = 1.0 / (1.0 + delta * delta);
            float f = fract(column);
            vec2 weights = vec2(1.0 - f, f) * confidence;
            float light = dot(weights, vec2(a.x, b.x));
            if (isnan(light) || isinf(light)) continue;
            float dist = abs(gridPoint - gridPosition);
            float angularWeight = exp(-0.125 * dist * dist) * (1.0 - smoothstep(4.5, 5.5, dist));
            sumLight += light * angularWeight;
            sumWeight += (weights.x + weights.y) * angularWeight;
            totalAngularWeight += angularWeight;
        }
        _711 = sumLight / max(1e-20, max(sumWeight, 0.08 * totalAngularWeight));
        _667 = _297 - _298;
        }
