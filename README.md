# BactroNative 1.6.3

## What works on Levi Android (PL hooks only)
- Performance (VSync, fullbright)
- Motion Blur (framebuffer blend)
- Hand hide
- Item Glint: **white enchantment foil** on held items when the game draws the foil layer

## Why Phase-style galaxy + white silhouette outline is harder
Phase (Windows Bedrock) can inject **RenderDragon materials** (`.material.bin`) and rewrite `iteminhand` shaders.

On Android, that needs a **MaterialBinLoader**-style hook (load materials from a resource pack). That is **not** in stock Levi PL yet — see LiteLDev/LeviLaunchroid#197 (port BetterRenderDragon material.bin redirect).

Without material.bin redirect:
- We cannot swap the hand fragment shader to draw galaxy / edge outline
- GLES double-draw hacks either miss the hand or glitch the world (we tried)

## Deeper roadmap (real path to Phase-like hand)
1. Hook ResourcePack / file read for `renderer/materials/*.material.bin` (Android signatures)
2. Ship a small pack that replaces `item_in_hand` material with outline + animated UV
3. Keep foil color + motion blur as native fallbacks

## TargetHUD / HealthCache
Intentionally removed. Do not restore from old zips.

## Wire Outline RE (1.26.51.01_RC0)

From `libminecraftpe.so` analysis:

- ECS: `AABBShapeComponent` { Vec3 min, Vec3 max, float width, float height }
- ECS: `StateVectorComponent` { Vec3 pos, posPrev, posDelta }
- Camera: `CameraAPI` / `CameraAPIComponent` / `CameraClientInstanceComponent`
- Symbols resolved via existing signatures: ActorIsPlayer, HitResultGetEntity,
  LevelGetHitResult, ActorFetchNearbyActorsSorted, ClientInstanceGetLocalPlayer

3D white wire = world AABB corners projected with view-projection, then GL_LINES.
Component access + camera matrix still required before safe draw (no 2D HUD cards).


## Camera matrix RE notes (1.26.51.01_RC0) — in progress

- Shader uniforms: `u_viewProj`, `u_prevViewProj`, `u_modelViewProj`, `u_invViewProj`
- Registration table @ file/VA `0x1c85158` → name `u_viewProj`
- BSS UniformHandle slot @ VA `0x131b3d18` (filled at runtime, zeros in file)
- GLES imports `glUniformMatrix4fv` — capture path for VP when matrix is uploaded
- Still need: absolute-address hook for `glUniformMatrix4fv` (or PLT), nearby-actor ABI, stable StateVector via entt

