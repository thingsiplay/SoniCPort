#pragma once

#include "Object.h"

#include <stddef.h>

//Level constants
#define RESERVED_OBJECTS 0x20
#define LEVEL_OBJECTS    0x60
#define OBJECTS          (RESERVED_OBJECTS + LEVEL_OBJECTS)

//Level macros
#define LEVEL_ID(zone, level) (((zone) << 8) | (level))

//Level types
typedef enum
{
	ZoneId_GHZ,
	ZoneId_LZ,
	ZoneId_MZ,
	ZoneId_SLZ,
	ZoneId_SYZ,
	ZoneId_SBZ,
	ZoneId_EndZ,
	ZoneId_SS,
	ZoneId_Num,
} ZoneId;

//Level globals
extern uint16_t level_id;

extern uint8_t level_map256[0xA400];
extern uint8_t level_map16[0x1800];
extern uint8_t level_layout[8][2][0x40];

extern Object reserved_objects[RESERVED_OBJECTS];
extern Object level_objects[LEVEL_OBJECTS];

//Level functions
void LoadLevelMaps(ZoneId zone);
void LoadLevelLayout(uint16_t level);
void LoadMap16(ZoneId zone);
void LoadMap256(ZoneId zone);
void LevelSizeLoad();
void DrawChunks(int16_t sx, int16_t sy, uint8_t *layout, size_t offset);