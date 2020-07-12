#ifndef MIF_FILE_H
#define MIF_FILE_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "ArenaTypes.h"
#include "../Math/Vector2.h"
#include "../World/VoxelUtils.h"

#include "components/utilities/Buffer2D.h"
#include "components/utilities/BufferView.h"
#include "components/utilities/BufferView2D.h"

// A MIF file contains map information. It defines the dimensions of a particular area 
// and which voxels have which IDs, as well as some other data. It is normally paired with 
// an INF file that tells which textures to use, among other things.

// It is composed of a map header and an array of levels.

class MIFFile
{
public:
	using VoxelID = uint16_t;

	class Level
	{
	private:
		std::string name, info; // Name of level and associated INF filename.
		int numf; // Number of floor textures.

		// Various data, not always present. FLOR and MAP1 are probably always present.
		Buffer2D<VoxelID> flor, map1, map2;
		std::vector<uint8_t> flat, inns, loot, stor;
		std::vector<ArenaTypes::MIFTarget> targ;
		std::vector<ArenaTypes::MIFLock> lock;
		std::vector<ArenaTypes::MIFTrigger> trig;
	public:
		Level();

		// Primary method for decoding .MIF level tag data. This method calls all the lower-
		// level loading methods for each tag as needed. The return value is the offset from 
		// the current LEVL tag to where the next LEVL tag would be.
		int load(const uint8_t *levelStart, WEInt levelWidth, SNInt levelDepth);

		// Loading methods for .MIF level tags that use level dimensions. The return value is
		// the offset from the current tag to where the next tag would be.
		static int loadFLOR(MIFFile::Level &level, const uint8_t *tagStart,
			WEInt levelWidth, SNInt levelDepth);
		static int loadMAP1(MIFFile::Level &level, const uint8_t *tagStart,
			WEInt levelWidth, SNInt levelDepth);
		static int loadMAP2(MIFFile::Level &level, const uint8_t *tagStart,
			WEInt levelWidth, SNInt levelDepth);

		// Loading methods for each .MIF level tag that don't need level dimensions.
		static int loadFLAT(MIFFile::Level &level, const uint8_t *tagStart);
		static int loadINFO(MIFFile::Level &level, const uint8_t *tagStart);
		static int loadINNS(MIFFile::Level &level, const uint8_t *tagStart);
		static int loadLOCK(MIFFile::Level &level, const uint8_t *tagStart);
		static int loadLOOT(MIFFile::Level &level, const uint8_t *tagStart);
		static int loadNAME(MIFFile::Level &level, const uint8_t *tagStart);
		static int loadNUMF(MIFFile::Level &level, const uint8_t *tagStart);
		static int loadSTOR(MIFFile::Level &level, const uint8_t *tagStart);
		static int loadTARG(MIFFile::Level &level, const uint8_t *tagStart);
		static int loadTRIG(MIFFile::Level &level, const uint8_t *tagStart);

		// Gets the height of the level in voxels. This value depends on extended blocks
		// in the MAP2 data, otherwise it drops back to a default value.
		int getHeight() const;

		const std::string &getName() const;
		const std::string &getInfo() const;
		int getNumf() const;

		BufferView2D<const VoxelID> getFLOR() const;
		BufferView2D<const VoxelID> getMAP1() const;
		BufferView2D<const VoxelID> getMAP2() const;

		BufferView<const uint8_t> getFLAT() const;
		BufferView<const uint8_t> getINNS() const;
		BufferView<const uint8_t> getLOOT() const;
		BufferView<const uint8_t> getSTOR() const;

		BufferView<const ArenaTypes::MIFTarget> getTARG() const;
		BufferView<const ArenaTypes::MIFLock> getLOCK() const;
		BufferView<const ArenaTypes::MIFTrigger> getTRIG() const;
	};
private:
	WEInt width;
	SNInt depth;
	int startingLevelIndex;
	std::array<OriginalInt2, 4> startPoints; // Entrance locations for the level (not always full).
	std::vector<MIFFile::Level> levels;
	std::string name;
	// Should a vector of levels be exposed, or does the caller want a nicer format?
	// VoxelGrid? Array of VoxelData?
public:
	bool init(const char *filename);

	// Gets the dimensions of the map. Width and depth are constant for all levels in a map,
	// and the height depends on MAP2 data in each level (if any -- default otherwise).
	WEInt getWidth() const;
	int getHeight(int levelIndex) const;
	SNInt getDepth() const;

	// Gets the starting level when the player enters the area.
	int getStartingLevelIndex() const;

	// Gets the name of the .MIF file.
	const std::string &getName() const;

	// Starting points for the player in special 'centimeter-like' units.
	const std::array<OriginalInt2, 4> &getStartPoints() const;

	// -- temp -- Get the levels associated with the .MIF file (I think we want the data 
	// to be in a nicer format before handing it over to the rest of the program).
	const std::vector<MIFFile::Level> &getLevels() const;
};

#endif
