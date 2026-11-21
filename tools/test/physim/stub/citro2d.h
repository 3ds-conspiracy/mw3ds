#pragma once
// citro2d for the host harness: the game's UI headers declare a few of its types, nothing here runs them
#include "citro3d.h"

typedef struct C2D_Sprite { C3D_Tex* tex; } C2D_Sprite;
typedef struct C2D_SpriteSheet { int count; } C2D_SpriteSheet;
typedef struct C2D_Text { char stub; } C2D_Text;
typedef struct C2D_Font { int stub; } C2D_Font;
typedef struct C2D_Image { C3D_Tex* tex; } C2D_Image;
