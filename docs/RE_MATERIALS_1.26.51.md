# RenderDragon material RE — MCPE 1.26.51 (ARM64)

## Uniform / flag names in libminecraftpe.so

- `HideGlowOutline`
- `ITEM_IN_HAND_EDGE_BRIGHTNESS` / `_TIGHTNESS` / `_SHARPNESS`
- `ENTITY_EDGE_BRIGHTNESS` / `_TIGHTNESS` / `_SHARPNESS` / `_LOD_SCALAR`
- `ItemInHandColor` / `ItemInHandColorGlint`
- `.material.bin`
- `Could not find specified material`

## Code xrefs (file offset == VA for this SO)

| String | Example code VA |
|--------|-----------------|
| HideGlowOutline | 0x106300d4, 0x10630bec, 0x10630c54 |
| ItemInHandColor | 0x10fe280c … |
| ItemInHandColorGlint | 0x10fe2a28 … |
| .material.bin | 0x11363b50, 0x113a0b74 |
| Could not find specified material | 0xf24b4d0 |

## Signatures added (SignatureId)

1. **HideGlowOutlineQuery** — frame near HideGlowOutline use (~0x1063007c)
2. **ItemInHandShaderSetup** — ItemInHandColor family (~0x10fe2650)
3. **MaterialBinPathBuilder** — .material.bin path (~0x11363a84)
4. **MaterialMissingError** — missing material path (~0xf24b4d0)
5. Existing **RenderMaterialGroupCommon**

## Phase plan

1. Resolve-only (1.16.0) — confirm addresses in status.txt  
2. Read-only hooks — log calls, do not change args  
3. MaterialUniformOverrides / ENTITY_EDGE_* write  
4. CreateMaterialImmediate + embed material.bin  
5. MaterialFilter swap on actor / ItemInHand draws  

## Note

Vibrant Visuals edge uniforms may only apply when that pipeline is active.  
Glow outline may be gated by `HideGlowOutline` option bit.
