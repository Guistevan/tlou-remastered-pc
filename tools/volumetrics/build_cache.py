"""Build a separate Vulkan shader cache for issue #55. See README.md."""
import argparse
import ctypes
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
PROJECTOR = 0xcdecdf59
SUN, MOON, DIRECTIONAL, SPOT, SPOT_RESOLVE = (
    0xbb1d430e, 0x05866787, 0xad16e118, 0x06e1a806, 0xca25dbad
)
# SPIRV-Cross temporary names depend on the exact translated module.
# Reject unknown inputs before changing anything.
MODULES = {
    (PROJECTOR, 0): "c061ee6e3d46c000251fffc370d5084a9a8df449103830b59b9ca8b65ce9fc82",
    (PROJECTOR, 1): "7024833cf068cfd1fafc2fefac807111e0b73a027338b6d70adf251a8d15f679",
    (SUN, 0): "a0d395ecf763efe56826cd1606f6b297ec7d9e85c6f906b4640f271ae789d9c4",
    (MOON, 0): "139bff09b81b07b86eb786a4a87cb00be99e90e66f5de3dc2c26790e60393905",
    (DIRECTIONAL, 0): "59d87c6cef0f459bad3366aa92ff89a27099eef7a682f2b88c0324e0d4a8968e",
    (SPOT, 0): "b736459a5ffb5046011bd68d30dddee451a310bb9d06edc65a402665610d7ece",
    (SPOT, 1): "230d41211922d9cd22a22c4f119a6d84469a7ab94ce23f4d5957d159b18b53a6",
    (SPOT_RESOLVE, 0): "d97a1010cb75cbfde219bf89c8910f01af9eb81c6601cd11a8abb58d52ab180d",
}


def key(shader, index):
    return shader ^ (index + 0x9e3779b9 + (shader << 12) + (shader >> 4))


def substitute(text, old, new):
    if text.count(old) != 1:
        raise ValueError("Unexpected SPIRV-Cross output at " + old)
    return text.replace(old, new)


def glsl(name):
    return (HERE / (name + ".glsl")).read_text(encoding="utf-8")


def edit(text, shader, index):
    bounds = glsl("directional_bounds")
    reconstruction = glsl("depth_filter")
    if shader in (SUN, MOON):
        text = substitute(text, "void main()", bounds + glsl("directional_integral") + "\nvoid main()")
        col = "_196" if shader == SUN else "_1675"
        invocation = "_196" if shader == SUN else "gl_GlobalInvocationID.x"
        anchor = "    imageStore(cs_img4, ivec2(uvec2(" + col + ","
        return substitute(text, anchor, """    if (all(equal(imageSize(cs_img4),ivec2(512)))) {
        for (uint y=gl_WorkGroupID.y; y<512u; y+=gl_NumWorkGroups.y)
            writeLightIntegral(buf0_dword_off,ivec2(INVOCATION,y));
    } else
""".replace("INVOCATION", invocation) + anchor)
    if shader == DIRECTIONAL:
        text = substitute(text, "void main()", bounds + reconstruction + "\nvoid main()")
        start = text.index("        precise float _427 =")
        end = text.index("        precise float _712 =", start)
        return text[:start] + glsl("directional_resolve") + text[end:]
    if shader == SPOT_RESOLVE:
        a = reconstruction.index("    float limit=")
        b = reconstruction.index("    for(", a)
        reconstruction = reconstruction[:a] + "    z=min(z,60000.0);\n" + reconstruction[b:]
        text = substitute(text, "void main()", reconstruction + "\nvoid main()")
        return substitute(text, "precise float _824 = (1.0 / _818) * _822;",
                          "float _824 = texelFetch(fs_img2,ivec2(0),0).x <= -1.99 ? "
                          "volumeVisibility(vec2(_153,_155),_267) : (1.0 / _818)*_822;")
    integration = glsl("spot_integral")
    output_image = "cs_img3"
    if shader == PROJECTOR:
        output_image = "cs_img4"
        integration = substitute(integration, "cs_img3,pixel", "cs_img4,pixel")
        integration = substitute(integration, "cs_img2,cs_samp0),uv", "cs_img3,cs_samp0),uv")
        integration = substitute(integration, "sum+=w*smoothstep",
            "vec3 pattern=textureLod(sampler2D(cs_img2,cs_samp0),clamp(st,0.0,1.0),0.0).rgb;\n"
            "            sum+=dot(pattern,vec3(0.333))*w*smoothstep")
    if "uniform texture2DArray cs_img1;" in text:
        integration = substitute(integration, "vec2(textureSize(cs_img1,0))",
                                 "vec2(textureSize(cs_img1,0).xy)")
        integration = substitute(integration,
            "sampler2D(cs_img1,cs_samp0),clamp(st+offset*texel,0.0,1.0)",
            "sampler2DArray(cs_img1,cs_samp0),vec3(clamp(st+offset*texel,0.0,1.0),"
            "roundEven(spotRow(off,24u).x))")
    text = substitute(text, "void main()", integration + "\nvoid main()")
    at = text.index(";", text.index("    uint buf0_dword_off")) + 1
    return text[:at] + """
    if(all(equal(imageSize(OUTPUT_IMAGE),ivec2(512))) && gl_NumWorkGroups.x==1u){
        for(uint y=gl_WorkGroupID.y;y<512u;y+=gl_NumWorkGroups.y)
            integrateSpot(buf0_dword_off,ivec2(gl_LocalInvocationID.x,y));
        return;
    }
""".replace("OUTPUT_IMAGE", output_image) + text[at:]


def build(args):
    src = args.cache.resolve() / "CUSA00552"
    out = args.output.resolve()
    if out.exists() or args.cache.resolve() in out.parents:
        raise ValueError("Select a new output directory outside the input cache.")
    hash64 = ctypes.CDLL(str(args.xxhash.resolve())).XXH3_64bits
    hash64.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    hash64.restype = ctypes.c_uint64
    graphics = list(src.glob("g_*.key"))
    priority = []
    for (shader, index), expected in MODULES.items():
        name = f"0x{shader:016x}_{index}.spv"
        if hashlib.sha256((src / name).read_bytes()).hexdigest() != expected:
            raise ValueError(f"Unsupported translated shader: {name}")
        value = key(shader, index)
        if shader in (DIRECTIONAL, SPOT_RESOLVE):
            keys = [p.stem for p in graphics if struct.pack("<Q", value) in p.read_bytes()]
        else:
            p = src / f"c_0x{value:016x}.key"
            keys = [p.stem] if p.is_file() else []
        if not keys:
            raise ValueError(f"Missing warm-up pipeline key for {name}")
        priority.extend(keys)
    with tempfile.TemporaryDirectory(dir=out.parent) as tmp:
        work = Path(tmp)
        staged = work / "cache" / "CUSA00552"
        staged.mkdir(parents=True)
        for p in src.iterdir():
            if p.suffix in (".spv", ".meta", ".key") or p.name in ("profile.bin", "warmup-order.bin"):
                shutil.copyfile(p, staged / p.name)
        for shader, index in MODULES:
            stage = "frag" if shader in (DIRECTIONAL, SPOT_RESOLVE) else "comp"
            module = staged / f"0x{shader:016x}_{index}.spv"
            meta = staged / f"0x{key(shader,index):016x}.meta"
            header = bytearray(meta.read_bytes())
            old = module.read_bytes()
            if struct.unpack_from("<Q", header, 24)[0] != hash64(old, len(old)):
                raise ValueError(f"Metadata checksum mismatch for {module.name}")
            source = work / f"module.{stage}"
            subprocess.run([args.cross, str(module), "--vulkan-semantics", "--output", str(source)], check=True)
            source.write_text(edit(source.read_text(), shader, index))
            subprocess.run([args.glslang, "-V", "-Os", "--target-env", "vulkan1.3",
                            str(source), "-o", str(module)], check=True)
            data = module.read_bytes()
            struct.pack_into("<Q", header, 24, hash64(data, len(data)))
            meta.write_bytes(header)
        (staged / "volumetric-priority.txt").write_text("\n".join(dict.fromkeys(priority)) + "\n", encoding="ascii")
        os.rename(staged.parent, out)


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("cache", type=Path, help="Original populated cache root containing CUSA00552")
    cli.add_argument("output", type=Path, help="New cache root for the launcher")
    cli.add_argument("--xxhash", type=Path, required=True, help="Path to the runtime's xxHash shared library")
    cli.add_argument("--cross", default="spirv-cross")
    cli.add_argument("--glslang", default="glslangValidator")
    build(cli.parse_args())
