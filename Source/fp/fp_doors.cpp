/**
 * @file fp/fp_doors.cpp
 *
 * Doorways and door leaves for the first-person view (see fp_doors.hpp).
 */
#include "fp/fp_doors.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "fp/fp_controls.hpp"
#include "fp/fp_textures.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "objects.h"

namespace devilution::fp {

namespace {

/** How far an open door's leaf is swung (radians) and how long the swing takes (ms). */
constexpr float OpenAngle = 1.52F;
constexpr float SwingMilliseconds = 450.0F;
/** How much wider than its art a doorway is drawn, to keep the iso picture's proportions on the taller walls. */
constexpr float DoorwayWidthScale = 1.4F;

struct LevelKey {
	const std::byte *cels;
	int level;
	int type;
	bool isSetLevel;
	int setLevelNum;
	int objects;

	bool operator==(const LevelKey &) const = default;
};

struct Swing {
	bool known = false;
	bool open = false;
	float from = 0;   // the angle when the last swing started
	float startedAt = 0; // ms
	int side = 1;
};

std::vector<Doorway> Doorways;
std::vector<Swing> Swings;
std::vector<int16_t> DoorwayIndex; // per tile (y * MAXDUNX + x): index into Doorways of the door tile, -1 if none
std::vector<int16_t> SpanIndex;    // per tile: index into Doorways whose art reaches the tile's faces, -1 if none
LevelKey BuiltFor { nullptr, -1, -1, false, -1, -1 };

float NowMilliseconds()
{
	static const auto Start = std::chrono::steady_clock::now();
	return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - Start).count();
}

/** @brief The pieces a door shows when it is shut: its own and its partner's (-1 if it has none). */
void ClosedPieces(const Object &door, int &own, int &partner, Point &partnerTile)
{
	partner = -1;
	partnerTile = door.position;
	switch (door._otype) {
	case OBJ_L1LDOOR:
	case OBJ_L5LDOOR:
		own = door._oVar1 - 1;
		partner = door._oVar2 - 1;
		partnerTile = door.position + Displacement { 0, -1 };
		break;
	case OBJ_L1RDOOR:
	case OBJ_L5RDOOR:
		own = door._oVar1 - 1;
		partner = door._oVar2 - 1;
		partnerTile = door.position + Displacement { -1, 0 };
		break;
	case OBJ_L2LDOOR: own = 537; break;
	case OBJ_L2RDOOR: own = 539; break;
	case OBJ_L3LDOOR: own = 530; break;
	case OBJ_L3RDOOR: own = 533; break;
	default: own = dPiece[door.position.x][door.position.y]; break;
	}
}

/** @brief A plain arched opening across the middle of the face, for doors whose art shows none. */
void DefaultOpening(Doorway &door, int rows)
{
	const int faceHeight = FaceTextureHeight();
	const int springRow = faceHeight - rows * 55 / 100;
	const float centre = FaceTextureWidth / 2.0F;
	const float half = FaceTextureWidth * 0.3F;
	const float rise = static_cast<float>(rows) * 0.12F;
	door.opening.assign(FaceTextureWidth, faceHeight);
	for (int u = 0; u < FaceTextureWidth; u++) {
		const float x = (static_cast<float>(u) + 0.5F - centre) / half;
		if (std::abs(x) < 1.0F)
			door.opening[u] = springRow - static_cast<int>(rise * std::sqrt(1.0F - x * x));
	}
}

bool BuildDoorway(Doorway &door, int objectId)
{
	const Object &object = Objects[objectId];
	door.object = objectId;
	door.tile = object.position;
	const Point t = object.position;
	int own;
	int partner;
	Point partnerTile;
	ClosedPieces(object, own, partner, partnerTile);
	if (own < 0 || own >= MAXTILES)
		return false;
	const bool solidX = IsTileSolid(t + Displacement { -1, 0 }) && IsTileSolid(t + Displacement { 1, 0 });
	const bool solidY = IsTileSolid(t + Displacement { 0, -1 }) && IsTileSolid(t + Displacement { 0, 1 });
	if (solidX != solidY)
		door.alongX = solidX;
	else
		door.alongX = partner >= 0 ? partnerTile.y == t.y : solidX;
	door.hasPartner = partner >= 0 && partner < MAXTILES && InDungeonBounds(partnerTile)
	    && (door.alongX ? partnerTile.y == t.y : partnerTile.x == t.x);
	door.partner = partnerTile;

	// The closed art along the wall: the partner's face then the door's, in world-axis order.
	const int faceHeight = FaceTextureHeight();
	const int rows = LevelWallRows();
	const int width = door.hasPartner ? 2 * FaceTextureWidth : FaceTextureWidth;
	std::vector<int16_t> strip(static_cast<size_t>(width) * faceHeight, NoPixel);
	auto copyFace = [&](int piece, int offset) {
		const PieceTextures &textures = GetPieceTextures(static_cast<uint16_t>(piece));
		const std::vector<int16_t> &face = door.alongX ? textures.faceY : textures.faceX;
		for (int row = 0; row < faceHeight; row++)
			for (int u = 0; u < FaceTextureWidth; u++)
				strip[static_cast<size_t>(row) * width + offset + u] = face[static_cast<size_t>(row) * FaceTextureWidth + u];
	};
	if (door.hasPartner) {
		copyFace(partner, 0);
		copyFace(own, FaceTextureWidth);
	} else {
		copyFace(own, 0);
	}
	// Where holes are in the art (no pixel), the plain wall shows: the doorway's frame is wall.
	const std::vector<int16_t> &fallback = GetFallbackFace(door.alongX ? t.x : t.y);
	for (int row = 0; row < faceHeight; row++)
		for (int u = 0; u < width; u++) {
			int16_t &texel = strip[static_cast<size_t>(row) * width + u];
			if (texel == NoPixel && row >= faceHeight - rows)
				texel = fallback[static_cast<size_t>(row) * FaceTextureWidth + u % FaceTextureWidth];
		}

	// The painted doorway: of the arch spans found, the one nearest the middle of the strip.
	std::vector<int> top;
	int first = -1;
	int last = -1;
	if (FindArchOutline(strip, width, rows, top)) {
		float bestDistance = 1e9F;
		for (int u = 0; u < width;) {
			if (top[u] >= faceHeight) {
				u++;
				continue;
			}
			int end = u;
			while (end < width && top[end] < faceHeight)
				end++;
			const float distance = std::abs((u + end) / 2.0F - width / 2.0F);
			if (distance < bestDistance) {
				bestDistance = distance;
				first = u;
				last = end - 1;
			}
			u = end;
		}
	}
	int start = 0;
	if (first >= 0)
		start = std::clamp((first + last + 1) / 2 - FaceTextureWidth / 2, 0, width - FaceTextureWidth);
	door.face.assign(static_cast<size_t>(FaceTextureWidth) * faceHeight, NoPixel);
	for (int row = 0; row < faceHeight; row++)
		for (int u = 0; u < FaceTextureWidth; u++)
			door.face[static_cast<size_t>(row) * FaceTextureWidth + u] = strip[static_cast<size_t>(row) * width + start + u];
	door.opening.assign(FaceTextureWidth, faceHeight);
	int openCount = 0;
	if (first >= 0) {
		for (int u = std::max(first, start); u <= std::min(last, start + FaceTextureWidth - 1); u++) {
			door.opening[u - start] = top[u];
			openCount++;
		}
	}
	if (openCount < FaceTextureWidth / 3)
		DefaultOpening(door, rows);
	// Widen the art around the middle of the tile, as far as the neighbours along the wall are plain wall.
	const Displacement step = door.alongX ? Displacement { 1, 0 } : Displacement { 0, 1 };
	auto spills = [&](Point neighbour) { return IsTileSolid(neighbour) && !IsDoorwayTile(neighbour); };
	const float half = DoorwayWidthScale / 2.0F;
	door.artFrom = 0.5F - half;
	door.artTo = 0.5F + half;
	const float lowest = spills(t - step) ? -1.0F : 0.0F;
	const float highest = spills(t + step) ? 2.0F : 1.0F;
	door.openFrom = highest;
	door.openTo = lowest;
	for (int u = 0; u < FaceTextureWidth; u++) {
		if (door.opening[u] >= faceHeight)
			continue;
		const float from = door.artFrom + (door.artTo - door.artFrom) * static_cast<float>(u) / FaceTextureWidth;
		const float to = door.artFrom + (door.artTo - door.artFrom) * static_cast<float>(u + 1) / FaceTextureWidth;
		door.openFrom = std::min(door.openFrom, from);
		door.openTo = std::max(door.openTo, to);
	}
	door.openFrom = std::max(door.openFrom, lowest);
	door.openTo = std::min(door.openTo, highest);
	return door.openTo > door.openFrom;
}

float SwingAngle(const Swing &swing, float now)
{
	const float t = std::clamp((now - swing.startedAt) / SwingMilliseconds, 0.0F, 1.0F);
	const float eased = t * t * (3.0F - 2.0F * t);
	const float target = swing.open ? OpenAngle : 0.0F;
	return swing.from + (target - swing.from) * eased;
}

} // namespace

void PrepareDoorways()
{
	const LevelKey key { pDungeonCels.get(), currlevel, static_cast<int>(leveltype), setlevel, static_cast<int>(setlvlnum), ActiveObjectCount };
	if (key == BuiltFor)
		return;
	BuiltFor = key;
	Doorways.clear();
	Swings.clear();
	DoorwayIndex.assign(static_cast<size_t>(MAXDUNX) * MAXDUNY, -1);
	SpanIndex.assign(static_cast<size_t>(MAXDUNX) * MAXDUNY, -1);
	if (leveltype == DTYPE_TOWN)
		return;
	// Door tiles first: a doorway does not reach into a neighbouring doorway.
	for (int i = 0; i < ActiveObjectCount; i++) {
		const Object &object = Objects[ActiveObjects[i]];
		if (object.isDoor() && InDungeonBounds(object.position))
			DoorwayIndex[static_cast<size_t>(object.position.y) * MAXDUNX + object.position.x] = 0;
	}
	for (int i = 0; i < ActiveObjectCount; i++) {
		const int id = ActiveObjects[i];
		const Object &object = Objects[id];
		if (!object.isDoor() || !InDungeonBounds(object.position))
			continue;
		Doorway door;
		const size_t at = static_cast<size_t>(object.position.y) * MAXDUNX + object.position.x;
		if (!BuildDoorway(door, id)) {
			DoorwayIndex[at] = -1;
			continue;
		}
		const int16_t index = static_cast<int16_t>(Doorways.size());
		DoorwayIndex[at] = index;
		const Displacement step = door.alongX ? Displacement { 1, 0 } : Displacement { 0, 1 };
		for (const int offset : { -1, 0, 1 }) {
			const Point tile = door.tile + step * offset;
			const float from = std::max(door.artFrom, static_cast<float>(offset));
			const float to = std::min(door.artTo, static_cast<float>(offset + 1));
			if (InDungeonBounds(tile) && (offset == 0 || (from < to && IsTileSolid(tile))))
				SpanIndex[static_cast<size_t>(tile.y) * MAXDUNX + tile.x] = index;
		}
		Doorways.push_back(std::move(door));
	}
	Swings.resize(Doorways.size());
}

DoorwaySpan DoorwaySpanAt(Point tile)
{
	if (SpanIndex.empty() || !InDungeonBounds(tile))
		return {};
	const int index = SpanIndex[static_cast<size_t>(tile.y) * MAXDUNX + tile.x];
	if (index < 0)
		return {};
	const Doorway &door = Doorways[index];
	return { &door, door.alongX ? tile.x - door.tile.x : tile.y - door.tile.y };
}

bool IsDoorwayTile(Point tile)
{
	return !DoorwayIndex.empty() && InDungeonBounds(tile) && DoorwayIndex[static_cast<size_t>(tile.y) * MAXDUNX + tile.x] >= 0;
}

int DoorFaceColumn(const Doorway &door, float along)
{
	if (along < door.artFrom || along >= door.artTo)
		return -1;
	return std::clamp(static_cast<int>((along - door.artFrom) / (door.artTo - door.artFrom) * FaceTextureWidth), 0, FaceTextureWidth - 1);
}

bool IsDoorwayHole(const RayHit &hit, float height)
{
	const DoorwaySpan span = DoorwaySpanAt(hit.tile);
	if (span.door == nullptr || Objects[span.door->object]._oVar4 == 0) // DOOR_CLOSED
		return false;
	if (hit.xSide == span.door->alongX) {
		// A face across the wall between two of the doorway's tiles (see FaceForHit in fp_view.cpp).
		const float along = span.door->alongX ? hit.point.x - static_cast<float>(span.door->tile.x) : hit.point.y - static_cast<float>(span.door->tile.y);
		return along > span.door->openFrom + 0.01F && along < span.door->openTo - 0.01F;
	}
	const float along = (hit.xSide ? hit.point.y - static_cast<float>(hit.tile.y) : hit.point.x - static_cast<float>(hit.tile.x)) + static_cast<float>(span.offset);
	if (along < span.door->openFrom || along >= span.door->openTo)
		return false;
	const int column = DoorFaceColumn(*span.door, along);
	const int row = WallArtRow(height / VerticalScale());
	return column >= 0 && row >= span.door->opening[column] && row < FaceTextureHeight();
}

float DoorSwing(const Doorway &door, int &side)
{
	const size_t index = static_cast<size_t>(&door - Doorways.data());
	Swing &swing = Swings[index];
	const bool open = Objects[door.object]._oVar4 != 0; // DOOR_CLOSED is 0; blocked doors stand open
	const float now = NowMilliseconds();
	if (!swing.known) {
		swing.known = true;
		swing.open = open;
		swing.from = open ? OpenAngle : 0.0F;
		swing.startedAt = now - SwingMilliseconds;
	} else if (open != swing.open) {
		swing.from = SwingAngle(swing, now);
		swing.open = open;
		swing.startedAt = now;
		if (open) {
			// Away from whoever opened it: the camera's side of the wall.
			const ViewState &view = GetViewState();
			const float mine = door.alongX ? view.y - (static_cast<float>(door.tile.y) + 0.5F) : view.x - (static_cast<float>(door.tile.x) + 0.5F);
			swing.side = mine > 0 ? -1 : 1;
		}
	}
	side = swing.side;
	return SwingAngle(swing, now);
}

} // namespace devilution::fp
