"""Build an isolated cache containing the RTX contact-occlusion fix for port v0.4.0."""
import argparse
import ctypes
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess

MODULE = "0x00000000aaae0737_0.spv"
ORIGINAL_HASH = "98c2da1d2f4244b0fdd870f3d6a6fc51b6719c8a85121de066fea4cb8b4d28ce"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_cache", type=Path, help="Existing cache root containing CUSA00552")
    parser.add_argument("destination", type=Path, help="New directory for the fixed cache root")
    parser.add_argument("--spirv-tools", type=Path, required=True,
                        help="Directory containing spirv-cross, glslangValidator, spirv-dis and spirv-as")
    parser.add_argument("--runtime-out", type=Path, required=True,
                        help="The port's gow3_pc/out directory, containing libxxhash.dll")
    args = parser.parse_args()
    tools = args.spirv_tools.resolve()
    original = args.source_cache.resolve() / "CUSA00552" / MODULE
    destination = args.destination.resolve()
    if destination.exists():
        parser.error("The destination must be a new directory.")
    if hashlib.sha256(original.read_bytes()).hexdigest() != ORIGINAL_HASH:
        parser.error("This cache does not contain the tested v0.4.0 shader variant.")
    import tempfile
    with tempfile.TemporaryDirectory(prefix="tlou-rtx58-") as temporary:
        compile_shader(Path(temporary), tools, original, destination, args.runtime_out.resolve())
    print(f"Select this cache directory in the launcher: {destination}")


def compile_shader(trial, tools, original, destination, runtime):
    source = trial / "contact-occlusion.comp"
    subprocess.run([str(tools / "spirv-cross.exe"), str(original), "--vulkan-semantics",
                    "--output", str(source)], check=True)
    code = source.read_text()
    anchor = "void main()"
    helper = """// Resolve the center of the AO pixel in the source texture with integer arithmetic.
// At an even downsample ratio, a normalized coordinate falls between two source pixels.
ivec2 sourcePixel(uvec2 pixel, ivec2 sourceSize)
{
    uvec2 aoSize = uvec2(imageSize(cs_img4));
    uvec2 position = ((2u * pixel + 1u) * uvec2(sourceSize)) / (2u * aoSize);
    return ivec2(min(position, uvec2(sourceSize) - 1u));
}

"""
    if code.count(anchor) != 1:
        raise RuntimeError("Unexpected compute shader entry point")
    code = code.replace(anchor, helper + anchor)
    for name in ("cs_img1", "cs_img2"):
        old = f"textureLod(sampler2D({name}, cs_samp0), vec2(_158, _160), 0.0)"
        new = f"texelFetch({name}, sourcePixel(uvec2(_108, _109), textureSize({name}, 0)), 0)"
        if code.count(old) != 1:
            raise RuntimeError(f"Unexpected center sample in {name}")
        code = code.replace(old, new)
    source.write_text(code, encoding="utf-8")
    output = trial / "contact-occlusion.spv"
    subprocess.run([str(tools / "glslangValidator.exe"), "-V", "--target-env", "vulkan1.3",
                    "-o", str(output), str(source)], check=True)
    assembly = subprocess.check_output([str(tools / "spirv-dis.exe"), str(output)], text=True)
    assembly = assembly.replace("OpCapability Shader", "OpCapability Shader\n"
                                "               OpCapability SignedZeroInfNanPreserve", 1)
    assembly = assembly.replace("OpExecutionMode %main LocalSize 8 8 1",
                                "OpExecutionMode %main LocalSize 8 8 1\n"
                                "               OpExecutionMode %main SignedZeroInfNanPreserve 32", 1)
    listing = trial / "contact-occlusion.spvasm"
    listing.write_text(assembly, encoding="ascii")
    subprocess.run([str(tools / "spirv-as.exe"), "--target-env", "vulkan1.3",
                    "-o", str(output), str(listing)], check=True)
    prepare_cache(destination, original, output, runtime)


def prepare_cache(cache, original, output, dll):
    destination = cache / "CUSA00552"
    if destination.exists():
        raise RuntimeError("Trial cache already exists")
    with os.add_dll_directory(str(dll)):
        hash64 = ctypes.CDLL(str(dll / "libxxhash.dll")).XXH3_64bits
    hash64.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    hash64.restype = ctypes.c_uint64
    shader = 0xaaae0737
    meta_id = shader ^ (0x9e3779b9 + (shader << 12) + (shader >> 4))
    meta_name = f"0x{meta_id:016x}.meta"
    header = bytearray((original.parent / meta_name).read_bytes())
    binary = original.read_bytes()
    if struct.unpack_from("<Q", header, 24)[0] != hash64(binary, len(binary)):
        raise RuntimeError("Original shader does not match its cache metadata")
    destination.mkdir(parents=True)
    for item in original.parent.iterdir():
        if item.suffix in (".spv", ".meta", ".key") or item.name in ("profile.bin", "warmup-order.bin"):
            shutil.copyfile(item, destination / item.name)
    replacement = output.read_bytes()
    struct.pack_into("<Q", header, 24, hash64(replacement, len(replacement)))
    (destination / meta_name).write_bytes(header)
    (destination / MODULE).write_bytes(replacement)
    priority_file = original.parent / "volumetric-priority.txt"
    priority = priority_file.read_text(encoding="ascii").splitlines() if priority_file.exists() else []
    keys = dict.fromkeys([f"c_0x{meta_id:016x}", *priority])
    (destination / "volumetric-priority.txt").write_text("\n".join(keys) + "\n", encoding="ascii")


if __name__ == "__main__":
    main()
