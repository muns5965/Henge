# Green frame: evidence that it came from the Half4 misread

Status: cause strongly indicated; post-fix screen NOT verified.

## What was observed (run /tmp/x231501/dump)
1. PSO 12 (pipe_ref=12, hash bd533ca461f8840a) is the pass-1 draw n=0 (indexed, 6 indices, 0,1,2,2,3,0, stride 48).
   Its serialized vertex descriptor is Float4@0, Float2@16, Float2@24, Half4(fmt 27)@32, all on buffer 1, stride 48
   (structural decoder; tests/arm64-vertex-desc-real-unit.c, fixture tests/fixtures/vtxdesc_pso_12.bin).
2. Raw vertex bytes at offset 32 of each of the 4 vertices of draw n=0 (buffer id=1 / blob 3, vtx_off=288):
   `00 00 00 00 00 00 00 3c  00 00 00 00 00 00 00 00` (16 bytes, identical for all four vertices).
   - As Half4 (the PSO's format): (0, 0, 0, 0x3c00 = 1.0) = opaque black.
   - As Float4 (what the old float tight-pack reads): float0 = 0x00000000 = 0.0, float1 = 0x3c000000 = 0.0078125, float2 = float3 = 0
     = (0, 0.0078125, 0, 0).
3. Pass-1 post image of attachment 0 (cap_img id=2, cap_ae42d33a2676a383.bin, RGBA16Float, 1920x1080, capture truncated to the first
   8388608 of 16588800 bytes = 546 of 1080 rows): 1048320 of the 1048576 captured pixels are the 8-byte value
   `00 00 00 20 00 00 00 00` = half (0, 0.0078125, 0, 0); the other 256 pixels are 0. 1048320 = 546 * 1920, i.e. every captured pixel of the
   covered rows except 256.
4. g7_frame_0.png (1920x1080 RGB): first pixels RGB (0, 2, 0). 0.0078125 * 255 = 1.99, i.e. 2 after 8-bit quantisation, so the screen
   value (0,2,0) is the same colour as item 3.

## Why this points at the Half4 misread
The float reinterpretation of the colour attribute (item 2) is exactly (0, 0.0078125, 0, 0); the rendered pixel (item 3) is exactly
(0, 0.0078125, 0, 0), and the screen value (item 4) is its 8-bit form. A correctly decoded Half4 would have given (0,0,0,1). The match is to
the bit pattern, not approximate.

## Not established
- Post-fix screen NOT verified: no device run with the structural decoder and Half4 -> VK_FORMAT_R16G16B16A16_SFLOAT has been looked at yet.
  The expected result (black / correct composite instead of (0,2,0)) is a prediction.
- The fragment shader was not traced to prove the vertex colour reaches the output unchanged; the identity of the values is circumstantial
  (though exact). Other draws in the frame were not examined for this pass.
- The captured image is truncated (546 of 1080 rows); rows below were not seen.
- Pixel interpretation as RGBA16Float assumes vkfmt=97 as logged; PNG frame channel order not independently checked.
