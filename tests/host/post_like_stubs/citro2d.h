// Host stand-in for citro2d/libctru types used by the UI widget headers (tests/host/test_pocket_ui.cpp).
#pragma once
#include <cstdint>
#include <cstddef>
#include <sys/types.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
struct C3D_Tex;
struct Tex3DS_SubTexture {
	u16 width, height;
	float left, top, right, bottom;
};
struct C2D_Image {
	C3D_Tex *tex;
	const Tex3DS_SubTexture *subtex;
};
// recorded by the test's draw stub
bool C2D_DrawCircleSolid(float x, float y, float depth, float radius, u32 color);
bool C2D_DrawTriangle(float x0, float y0, u32 clr0, float x1, float y1, u32 clr1, float x2, float y2, u32 clr2,
                      float depth);
bool C2D_DrawRectSolid(float x, float y, float z, float w, float h, u32 clr);
// OpenTube r13 (ui/modern.cpp): vertical gradients
bool C2D_DrawRectangle(float x, float y, float z, float w, float h, u32 clr0, u32 clr1, u32 clr2, u32 clr3);
