#include "ambient_occlusion.hpp"
#include "Globals.hpp"
#include "block_face.hpp"
#include "gbh_graphics.hpp"
#include "map_0x370.hpp"
#include "settings.hpp"
#include <string.h>

#if defined(GTA2_PORT_SDL)
void port_SetOcclusion(const u8* pCells, s32 width, s32 height, s32 depth, f32 strength);
void port_SetGameVerts(const void* pTileVerts, const void* pSpriteVerts);
#endif
EXTERN_GLOBAL_ARRAY(Vert, gTileVerts, 8);
EXTERN_GLOBAL_ARRAY(Vert, gSpriteVerts, 8);

namespace PortAmbientOcclusion
{
namespace
{
// The map's 8 levels, then empty sky up to a power of two.
const s32 kSize = 256;
const s32 kLevels = 8;
const s32 kDepth = 16;
// Rows of columns classified again per frame, to follow the blocks scripts change (the map in about 2 seconds).
const s32 kRowsPerFrame = 2;

// How much of the ambient light the most occluded surfaces lose.
const f32 kStrength = 0.6f;

u8 sCells[kDepth][kSize][kSize];
u8 sRow[kDepth][kSize];
const void* spBlocks;
s32 sNextRow;
bool sOn; // the renderer has the map's volume

bool HasFace(u16 word)
{
    return (word & block_face::tile_idx_mask) != 0 && (word & block_face::flat) == 0;
}

// One column into rows[z][x]: 255 solid, a fraction for diagonal and part filled blocks.
// Building interiors are empty blocks under a roof with no lid below; under a bridge or in a tunnel,
// the ground's lid is below. A block with walls on all four sides is a pillar.
void ClassifyColumn(s32 x, s32 y, u8 (*pRows)[kSize])
{
    gmp_block_info* pBlocks[kLevels];
    bool lid[kLevels];
    for (s32 z = 0; z < kLevels; z++)
    {
        pBlocks[z] = gMap_0x370->GetBlockOrNull(static_cast<u8>(x), static_cast<u8>(y), static_cast<u8>(z));
        lid[z] = pBlocks[z] && HasFace(pBlocks[z]->mLid);
    }
    for (s32 z = 0; z < kLevels; z++)
    {
        bool lidAbove = false;
        bool lidBelow = false;
        for (s32 k = z + 1; k < kLevels; k++)
        {
            lidAbove |= lid[k];
        }
        for (s32 k = 0; k < z; k++)
        {
            lidBelow |= lid[k];
        }
        bool solid = lid[z] || (lidAbove && !lidBelow);
        u8 value = solid ? 255 : 0;
        const gmp_block_info* pBlock = pBlocks[z];
        if (pBlock)
        {
            const s32 walls = HasFace(pBlock->mLeft) + HasFace(pBlock->mRight) + HasFace(pBlock->mTop) + HasFace(pBlock->mBottom);
            if (walls == 4)
            {
                solid = true;
                value = 255;
            }
            const u8 slope = GET_SLOPE_BITS(pBlock->mSlopeType);
            if (IS_GRADIENT_SLOPE(pBlock->mSlopeType))
            {
                // Ramps and pitched roofs would shade their own surface: they occlude nothing.
                value = 0;
            }
            else if (slope >= DIAGONAL_WALL_UP_LEFT && slope <= TRIANGULAR_SIDES_DIAGONAL_DOWN_RIGHT)
            {
                value = walls || lid[z] ? 128 : 0;
            }
            else if (IS_PARTIAL_BLOCK(slope))
            {
                value = walls || lid[z] ? (slope == PARTIAL_CENTRE_BLOCK ? 64 : 96) : 0;
            }
        }
        pRows[z][x] = value;
    }
}

void ClassifyRow(s32 y, u8 (*pRows)[kSize])
{
    for (s32 x = 0; x < kSize; x++)
    {
        ClassifyColumn(x, y, pRows);
    }
}
} // namespace

void Update()
{
    if (!PortSettings::AmbientOcclusion())
    {
        // Off: the volume is read again from the map when it is turned back on.
        if (sOn)
        {
            sOn = false;
            spBlocks = NULL;
#if defined(GTA2_PORT_SDL)
            port_SetOcclusion(NULL, 0, 0, 0, 0.0f);
#endif
        }
        return;
    }
    if (!gMap_0x370 || !gMap_0x370->mpDmap || !gMap_0x370->mpDmap->mBlock)
    {
        return;
    }
    bool changed = false;
    if (spBlocks != gMap_0x370->mpDmap->mBlock)
    {
        spBlocks = gMap_0x370->mpDmap->mBlock;
        for (s32 y = 0; y < kSize; y++)
        {
            ClassifyRow(y, sRow);
            for (s32 z = 0; z < kDepth; z++)
            {
                memcpy(sCells[z][y], sRow[z], kSize);
            }
        }
        sNextRow = 0;
        changed = true;
    }
    else
    {
        for (s32 i = 0; i < kRowsPerFrame; i++, sNextRow = (sNextRow + 1) % kSize)
        {
            ClassifyRow(sNextRow, sRow);
            for (s32 z = 0; z < kLevels; z++)
            {
                if (memcmp(sCells[z][sNextRow], sRow[z], kSize))
                {
                    memcpy(sCells[z][sNextRow], sRow[z], kSize);
                    changed = true;
                }
            }
        }
    }
#if defined(GTA2_PORT_SDL)
    if (changed)
    {
        // The renderer knows the map's tiles and the sprites by these arrays when LIGHTING (NOON) flags none of them.
        port_SetGameVerts(gTileVerts, gSpriteVerts);
        port_SetOcclusion(&sCells[0][0][0], kSize, kSize, kDepth, kStrength);
        sOn = true;
    }
#else
    (void)changed;
#endif
}
} // namespace PortAmbientOcclusion
