#include <algorithm>
#include <cmath>
#include <emmintrin.h>
#include <immintrin.h>
#include <limits>
#include <smmintrin.h>

#include "SoftwareRenderer.h"
#include "Surface.h"
#include "../Entities/EntityAnimationData.h"
#include "../Entities/EntityType.h"
#include "../Math/Constants.h"
#include "../Math/MathUtils.h"
#include "../Media/Color.h"
#include "../Media/Palette.h"
#include "../Utilities/Platform.h"
#include "../World/VoxelDataType.h"
#include "../World/VoxelGrid.h"

#include "components/debug/Debug.h"

namespace
{
	// Hardcoded graphics options (will be loaded at runtime at some point).
	constexpr int TextureFilterMode = 0;

	// Hardcoded palette indices with special behavior in the original game's renderer.
	constexpr uint8_t PALETTE_INDEX_LIGHT_LEVEL_LOWEST = 1;
	constexpr uint8_t PALETTE_INDEX_LIGHT_LEVEL_HIGHEST = 13;
	constexpr uint8_t PALETTE_INDEX_LIGHT_LEVEL_DIVISOR = 14;
	constexpr uint8_t PALETTE_INDEX_RED_SRC1 = 14;
	constexpr uint8_t PALETTE_INDEX_RED_SRC2 = 15;
	constexpr uint8_t PALETTE_INDEX_RED_DST1 = 158;
	constexpr uint8_t PALETTE_INDEX_RED_DST2 = 159;
	constexpr uint8_t PALETTE_INDEX_NIGHT_LIGHT = 113;
}

SoftwareRenderer::VoxelTexel::VoxelTexel()
{
	this->r = 0.0;
	this->g = 0.0;
	this->b = 0.0;
	this->emission = 0.0;
	this->transparent = false;
}

SoftwareRenderer::VoxelTexel SoftwareRenderer::VoxelTexel::makeFrom8Bit(
	uint8_t texel, const Palette &palette)
{
	// Convert ARGB color from integer to double-precision format. This does waste
	// an extreme amount of memory (32 bytes per pixel!), but it's not a big deal
	// for Arena's textures (eight textures is a megabyte).
	const uint32_t srcARGB = palette.get()[texel].toARGB();
	const Double4 srcTexel = Double4::fromARGB(srcARGB);
	VoxelTexel voxelTexel;
	voxelTexel.r = srcTexel.x;
	voxelTexel.g = srcTexel.y;
	voxelTexel.b = srcTexel.z;
	voxelTexel.transparent = srcTexel.w == 0.0;
	return voxelTexel;
}

SoftwareRenderer::FlatTexel::FlatTexel()
{
	this->r = 0.0;
	this->g = 0.0;
	this->b = 0.0;
	this->a = 0.0;
}

SoftwareRenderer::FlatTexel SoftwareRenderer::FlatTexel::makeFrom8Bit(
	uint8_t texel, const Palette &palette)
{
	// Palette indices 1-13 are used for light level diminishing in the original game.
	// These texels do not have any color and are purely for manipulating the previously
	// rendered color in the frame buffer.
	FlatTexel flatTexel;

	if ((texel >= PALETTE_INDEX_LIGHT_LEVEL_LOWEST) &&
		(texel <= PALETTE_INDEX_LIGHT_LEVEL_HIGHEST))
	{
		flatTexel.r = 0.0;
		flatTexel.g = 0.0;
		flatTexel.b = 0.0;
		flatTexel.a = static_cast<double>(texel) /
			static_cast<double>(PALETTE_INDEX_LIGHT_LEVEL_DIVISOR);
	}
	else
	{
		// Check if the color is hardcoded to another palette index. Otherwise,
		// color the texel normally.
		const int paletteIndex = (texel == PALETTE_INDEX_RED_SRC1) ? PALETTE_INDEX_RED_DST1 :
			((texel == PALETTE_INDEX_RED_SRC2) ? PALETTE_INDEX_RED_DST2 : texel);

		const uint32_t srcARGB = palette.get()[paletteIndex].toARGB();
		const Double4 dstTexel = Double4::fromARGB(srcARGB);
		flatTexel.r = dstTexel.x;
		flatTexel.g = dstTexel.y;
		flatTexel.b = dstTexel.z;
		flatTexel.a = dstTexel.w;
	}

	return flatTexel;
}

SoftwareRenderer::SkyTexel::SkyTexel()
{
	this->r = 0.0;
	this->g = 0.0;
	this->b = 0.0;
	this->a = 0.0;
}

SoftwareRenderer::SkyTexel SoftwareRenderer::SkyTexel::makeFrom8Bit(
	uint8_t texel, const Palette &palette)
{
	// Same as flat texels but for sky objects and without some hardcoded indices.
	SkyTexel skyTexel;

	if ((texel >= 1) && (texel <= 13))
	{
		skyTexel.r = 0.0;
		skyTexel.g = 0.0;
		skyTexel.b = 0.0;
		skyTexel.a = static_cast<double>(texel) / 14.0;
	}
	else
	{
		// Color the texel normally.
		const uint32_t srcARGB = palette.get()[texel].toARGB();
		const Double4 dstTexel = Double4::fromARGB(srcARGB);
		skyTexel.r = dstTexel.x;
		skyTexel.g = dstTexel.y;
		skyTexel.b = dstTexel.z;
		skyTexel.a = dstTexel.w;
	}

	return skyTexel;
}

SoftwareRenderer::FlatTexture::FlatTexture()
{
	this->width = 0;
	this->height = 0;
}

SoftwareRenderer::SkyTexture::SkyTexture()
{
	this->width = 0;
	this->height = 0;
}

SoftwareRenderer::FlatTextureGroup::StateTypeMapping *SoftwareRenderer::FlatTextureGroup::findMapping(
	EntityAnimationData::StateType stateType)
{
	const auto iter = std::find_if(this->stateTypeMappings.begin(), this->stateTypeMappings.end(),
		[stateType](const StateTypeMapping &mapping)
	{
		return mapping.first == stateType;
	});

	return (iter != this->stateTypeMappings.end()) ? &(*iter) : nullptr;
}

const SoftwareRenderer::FlatTextureGroup::StateTypeMapping *SoftwareRenderer::FlatTextureGroup::findMapping(
	EntityAnimationData::StateType stateType) const
{
	const auto iter = std::find_if(this->stateTypeMappings.begin(), this->stateTypeMappings.end(),
		[stateType](const StateTypeMapping &mapping)
	{
		return mapping.first == stateType;
	});

	return (iter != this->stateTypeMappings.end()) ? &(*iter) : nullptr;
}

int SoftwareRenderer::FlatTextureGroup::anglePercentToIndex(const AngleGroup &angleGroup,
	double anglePercent)
{
	DebugAssert(anglePercent >= 0.0);
	DebugAssert(anglePercent <= 1.0);
	DebugAssert(angleGroup.size() > 0);

	const int groupCount = static_cast<int>(angleGroup.size());
	return std::clamp(static_cast<int>(groupCount * anglePercent), 0, groupCount - 1);
}

SoftwareRenderer::FlatTextureGroup::TextureList *SoftwareRenderer::FlatTextureGroup::findTextureList(
	AngleGroup &angleGroup, int angleID)
{
	const auto iter = std::find_if(angleGroup.begin(), angleGroup.end(),
		[angleID](const auto &pair)
	{
		return pair.first == angleID;
	});

	return (iter != angleGroup.end()) ? &iter->second : nullptr;
}

const SoftwareRenderer::FlatTextureGroup::TextureList *SoftwareRenderer::FlatTextureGroup::getTextureList(
	EntityAnimationData::StateType stateType, double anglePercent) const
{
	const StateTypeMapping *mapping = this->findMapping(stateType);
	if (mapping != nullptr)
	{
		const AngleGroup &angleGroup = mapping->second;
		const int index = FlatTextureGroup::anglePercentToIndex(angleGroup, anglePercent);
		DebugAssertIndex(angleGroup, index);
		return &angleGroup[index].second;
	}
	else
	{
		return nullptr;
	}
}

void SoftwareRenderer::FlatTextureGroup::addTexture(EntityAnimationData::StateType stateType,
	int angleID, bool flipped, const uint8_t *srcTexels, int width, int height,
	const Palette &palette)
{
	DebugAssert(width > 0);
	DebugAssert(height > 0);

	// Add state type mapping if it doesn't exist.
	StateTypeMapping *mapping = this->findMapping(stateType);
	if (mapping == nullptr)
	{
		this->stateTypeMappings.push_back(std::make_pair(stateType, AngleGroup()));
		mapping = &this->stateTypeMappings.back();
	}

	// Add texture list to angle group entry if it doesn't exist.
	AngleGroup &angleGroup = mapping->second;
	TextureList *textureList = this->findTextureList(angleGroup, angleID);
	if (textureList == nullptr)
	{
		angleGroup.push_back(std::make_pair(angleID, TextureList()));
		textureList = &angleGroup.back().second;
	}

	const int texelCount = width * height;

	FlatTexture flatTexture;
	flatTexture.width = width;
	flatTexture.height = height;
	flatTexture.texels = std::vector<FlatTexel>(texelCount);

	// Texel order depends on whether the animation is flipped left or right.
	if (!flipped)
	{
		std::transform(srcTexels, srcTexels + texelCount, flatTexture.texels.begin(),
			[&palette](const uint8_t srcTexel)
		{
			return FlatTexel::makeFrom8Bit(srcTexel, palette);
		});
	}
	else
	{
		for (int y = 0; y < height; y++)
		{
			for (int x = 0; x < width; x++)
			{
				const int srcIndex = x + (y * width);
				const int dstIndex = ((width - 1) - x) + (y * width);
				const uint8_t srcTexel = srcTexels[srcIndex];
				flatTexture.texels[dstIndex] = FlatTexel::makeFrom8Bit(srcTexel, palette);
			}
		}
	}

	textureList->push_back(std::move(flatTexture));
}

SoftwareRenderer::Camera::Camera(const Double3 &eye, const Double3 &direction,
	double fovY, double aspect, double projectionModifier)
	: eye(eye), direction(direction)
{
	// Variations of eye position for certain voxel calculations.
	this->eyeVoxelReal = Double3(
		std::floor(eye.x),
		std::floor(eye.y),
		std::floor(eye.z));
	this->eyeVoxel = Int3(
		static_cast<int>(this->eyeVoxelReal.x),
		static_cast<int>(this->eyeVoxelReal.y),
		static_cast<int>(this->eyeVoxelReal.z));

	// Camera axes. We trick the 2.5D ray caster into thinking the player is always looking
	// straight forward, but we use the Y component of the player's direction to offset 
	// projected coordinates via Y-shearing.
	const Double3 forwardXZ = Double3(direction.x, 0.0, direction.z).normalized();
	const Double3 rightXZ = forwardXZ.cross(Double3::UnitY).normalized();

	// Transformation matrix (model matrix isn't required because it's just the identity).
	this->transform = [&eye, &forwardXZ, &rightXZ, fovY, aspect, projectionModifier]()
	{
		// Global up vector, scaled by the projection modifier (i.e., to account for tall pixels).
		const Double3 up = Double3::UnitY * projectionModifier;

		const Matrix4d view = Matrix4d::view(eye, forwardXZ, rightXZ, up);
		const Matrix4d projection = Matrix4d::perspective(fovY, aspect,
			SoftwareRenderer::NEAR_PLANE, SoftwareRenderer::FAR_PLANE);
		return projection * view;
	}();

	this->forwardX = forwardXZ.x;
	this->forwardZ = forwardXZ.z;
	this->rightX = rightXZ.x;
	this->rightZ = rightXZ.z;

	this->fovY = fovY;

	// Zoom of the camera, based on vertical field of view.
	this->zoom = MathUtils::verticalFovToZoom(fovY);
	this->aspect = aspect;

	// Forward and right modifiers, for interpolating 3D vectors across the screen and
	// so vertical FOV and aspect ratio are taken into consideration.
	this->forwardZoomedX = this->forwardX * this->zoom;
	this->forwardZoomedZ = this->forwardZ * this->zoom;
	this->rightAspectedX = this->rightX * this->aspect;
	this->rightAspectedZ = this->rightZ * this->aspect;

	// Left and right 2D vectors of the view frustum (at left and right edges of the screen).
	const Double2 frustumLeft = Double2(
		this->forwardZoomedX - this->rightAspectedX,
		this->forwardZoomedZ - this->rightAspectedZ).normalized();
	const Double2 frustumRight = Double2(
		this->forwardZoomedX + this->rightAspectedX,
		this->forwardZoomedZ + this->rightAspectedZ).normalized();
	this->frustumLeftX = frustumLeft.x;
	this->frustumLeftZ = frustumLeft.y;
	this->frustumRightX = frustumRight.x;
	this->frustumRightZ = frustumRight.y;

	// Vertical angle of the camera relative to the horizon.
	this->yAngleRadians = direction.getYAngleRadians();

	// Y-shearing is the distance that projected Y coordinates are translated by based on the 
	// player's 3D direction and field of view. First get the player's angle relative to the 
	// horizon, then get the tangent of that angle. The Y component of the player's direction
	// must be clamped less than 1 because 1 would imply they are looking straight up or down, 
	// which is impossible in 2.5D rendering (the vertical line segment of the view frustum 
	// would be infinitely high or low). The camera code should take care of the clamping for us.

	// Get the number of screen heights to translate all projected Y coordinates by, relative to
	// the current zoom. As a reference, this should be some value roughly between -1.0 and 1.0
	// for "acceptable skewing" at a vertical FOV of 90.0. If the camera is not clamped, this
	// could theoretically be between -infinity and infinity, but it would result in far too much
	// skewing.
	this->yShear = std::tan(this->yAngleRadians) * this->zoom;
}

double SoftwareRenderer::Camera::getXZAngleRadians() const
{
	return MathUtils::fullAtan2(this->forwardX, this->forwardZ);
}

int SoftwareRenderer::Camera::getAdjustedEyeVoxelY(double ceilingHeight) const
{
	return static_cast<int>(this->eye.y / ceilingHeight);
}

SoftwareRenderer::Ray::Ray(double dirX, double dirZ)
{
	this->dirX = dirX;
	this->dirZ = dirZ;
}

SoftwareRenderer::DrawRange::DrawRange(double yProjStart, double yProjEnd, int yStart, int yEnd)
{
	this->yProjStart = yProjStart;
	this->yProjEnd = yProjEnd;
	this->yStart = yStart;
	this->yEnd = yEnd;
}

SoftwareRenderer::OcclusionData::OcclusionData(int yMin, int yMax)
{
	this->yMin = yMin;
	this->yMax = yMax;
}

SoftwareRenderer::OcclusionData::OcclusionData()
	: OcclusionData(0, 0) { }

void SoftwareRenderer::OcclusionData::clipRange(int *yStart, int *yEnd) const
{
	const bool occluded = (*yEnd <= this->yMin) || (*yStart >= this->yMax);

	if (occluded)
	{
		// The drawing range is completely hidden.
		*yStart = *yEnd;
	}
	else
	{
		// Clip the drawing range.
		*yStart = std::max(*yStart, this->yMin);
		*yEnd = std::min(*yEnd, this->yMax);
	}
}

void SoftwareRenderer::OcclusionData::update(int yStart, int yEnd)
{
	// Slightly different than clipRange() because values just needs to be adjacent
	// rather than overlap.
	const bool canIncreaseMin = yStart <= this->yMin;
	const bool canDecreaseMax = yEnd >= this->yMax;

	// Determine how to update the occlusion ranges.
	if (canIncreaseMin && canDecreaseMax)
	{
		// The drawing range touches the top and bottom occlusion values, so the 
		// entire column is occluded.
		this->yMin = this->yMax;
	}
	else if (canIncreaseMin)
	{
		// Move the top of the window downward.
		this->yMin = std::max(yEnd, this->yMin);
	}
	else if (canDecreaseMax)
	{
		// Move the bottom of the window upward.
		this->yMax = std::min(yStart, this->yMax);
	}
}

SoftwareRenderer::ShadingInfo::ShadingInfo(const std::vector<Double3> &skyPalette,
	double daytimePercent, double latitude, double ambient, double fogDistance)
{
	this->timeRotation = SoftwareRenderer::getTimeOfDayRotation(daytimePercent);
	this->latitudeRotation = SoftwareRenderer::getLatitudeRotation(latitude);

	// The "sliding window" of sky colors is backwards in the AM (horizon is latest in the palette)
	// and forwards in the PM (horizon is earliest in the palette).
	this->isAM = daytimePercent < 0.50;
	const int slideDirection = this->isAM ? -1 : 1;

	// Get the real index (not the integer index) of the color for the current time as a
	// reference point so each sky color can be interpolated between two samples via 'percent'.
	const double realIndex = static_cast<double>(skyPalette.size()) * daytimePercent;
	const double percent = realIndex - std::floor(realIndex);

	// Lambda for keeping the given index within the palette range.
	auto wrapIndex = [&skyPalette](int index)
	{
		const int paletteCount = static_cast<int>(skyPalette.size());

		while (index >= paletteCount)
		{
			index -= paletteCount;
		}

		while (index < 0)
		{
			index += paletteCount;
		}

		return index;
	};

	// Calculate sky colors based on the time of day.
	for (int i = 0; i < static_cast<int>(this->skyColors.size()); i++)
	{
		const int indexDiff = slideDirection * i;
		const int index = wrapIndex(static_cast<int>(realIndex) + indexDiff);
		const int nextIndex = wrapIndex(index + slideDirection);
		const Double3 &color = skyPalette.at(index);
		const Double3 &nextColor = skyPalette.at(nextIndex);

		this->skyColors[i] = color.lerp(nextColor, this->isAM ? (1.0 - percent) : percent);
	}

	// The sun rises in the west (-Z) and sets in the east (+Z).
	this->sunDirection = [this, latitude]()
	{
		// The sun gets a bonus to latitude. Arena angle units are 0->100.
		const double sunLatitude = -(latitude + (13.0 / 100.0));
		const Matrix4d sunRotation = SoftwareRenderer::getLatitudeRotation(sunLatitude);
		const Double3 baseDir = -Double3::UnitY;
		const Double4 dir = sunRotation * (this->timeRotation * Double4(baseDir, 0.0));
		return Double3(dir.x, dir.y, dir.z).normalized();
	}();
	
	this->sunColor = [this]()
	{
		const Double3 baseSunColor(0.90, 0.875, 0.85);

		// Darken the sun color if it's below the horizon so wall faces aren't lit 
		// as much during the night. This is just an artistic value to compensate
		// for the lack of shadows.
		return (this->sunDirection.y >= 0.0) ? baseSunColor :
			(baseSunColor * (1.0 - (5.0 * std::abs(this->sunDirection.y)))).clamped();
	}();

	this->ambient = ambient;

	// At their darkest, distant objects are ~1/4 of their intensity.
	this->distantAmbient = std::clamp(ambient, 0.25, 1.0);

	this->fogDistance = fogDistance;
}

const Double3 &SoftwareRenderer::ShadingInfo::getFogColor() const
{
	// The fog color is the same as the horizon color.
	return this->skyColors.front();
}

SoftwareRenderer::FrameView::FrameView(uint32_t *colorBuffer, double *depthBuffer, 
	int width, int height)
{
	this->colorBuffer = colorBuffer;
	this->depthBuffer = depthBuffer;
	this->width = width;
	this->height = height;
	this->widthReal = static_cast<double>(width);
	this->heightReal = static_cast<double>(height);
}

template <typename T>
SoftwareRenderer::DistantObject<T>::DistantObject(const T &obj, int textureIndex)
	: obj(obj)
{
	this->textureIndex = textureIndex;
}

const int SoftwareRenderer::DistantObjects::NO_SUN = -1;

SoftwareRenderer::DistantObjects::DistantObjects()
{
	this->sunTextureIndex = DistantObjects::NO_SUN;
}

void SoftwareRenderer::DistantObjects::init(const DistantSky &distantSky,
	std::vector<SkyTexture> &skyTextures, const Palette &palette)
{
	DebugAssert(skyTextures.size() == 0);

	// Creates a render texture from the given surface, adds it to the sky textures list, and
	// returns its index in the sky textures list.
	auto addSkyTexture = [&skyTextures, &palette](const BufferView2D<const uint8_t> &buffer)
	{
		const int width = buffer.getWidth();
		const int height = buffer.getHeight();
		const int texelCount = width * height;

		skyTextures.push_back(SkyTexture());
		SkyTexture &texture = skyTextures.back();
		texture.texels = std::vector<SkyTexel>(texelCount);
		texture.width = width;
		texture.height = height;

		for (int y = 0; y < height; y++)
		{
			for (int x = 0; x < width; x++)
			{
				// Similar to ghosts, some clouds have special palette indices for a simple
				// form of transparency.
				const uint8_t texel = buffer.get(x, y);
				const int index = x + (y * width);
				texture.texels[index] = SkyTexel::makeFrom8Bit(texel, palette);
			}
		}

		return static_cast<int>(skyTextures.size()) - 1;
	};

	// Creates a render texture with a single texel for small stars.
	auto addSmallStarTexture = [&skyTextures](uint32_t color)
	{
		skyTextures.push_back(SkyTexture());
		SkyTexture &texture = skyTextures.back();
		texture.texels = std::vector<SkyTexel>(1);
		texture.width = 1;
		texture.height = 1;

		// Small stars are never transparent in the original game; this is just using the
		// same storage representation as clouds which can have some transparencies.
		const Double4 srcColor = Double4::fromARGB(color);
		SkyTexel &dstTexel = texture.texels.front();
		dstTexel.r = srcColor.x;
		dstTexel.g = srcColor.y;
		dstTexel.b = srcColor.z;
		dstTexel.a = srcColor.w;

		return static_cast<int>(skyTextures.size()) - 1;
	};

	// Reverse iterate through each distant object type in the distant sky, creating associations
	// between the distant sky object and its render texture. Order of insertion matters.
	for (int i = distantSky.getLandObjectCount() - 1; i >= 0; i--)
	{
		const DistantSky::LandObject &landObject = distantSky.getLandObject(i);
		const int entryIndex = landObject.getTextureEntryIndex();
		const int textureIndex = addSkyTexture(distantSky.getTexture(entryIndex));
		this->lands.push_back(DistantObject<DistantSky::LandObject>(landObject, textureIndex));
	}

	for (int i = distantSky.getAnimatedLandObjectCount() - 1; i >= 0; i--)
	{
		const DistantSky::AnimatedLandObject &animLandObject = distantSky.getAnimatedLandObject(i);
		const int setEntryIndex = animLandObject.getTextureSetEntryIndex();
		const int setEntryCount = distantSky.getTextureSetCount(setEntryIndex);
		DebugAssert(setEntryCount > 0);

		// Add first texture to get the start index of the animated textures.
		const int textureIndex = addSkyTexture(distantSky.getTextureSetElement(setEntryIndex, 0));

		for (int j = 1; j < setEntryCount; j++)
		{
			addSkyTexture(distantSky.getTextureSetElement(setEntryIndex, j));
		}

		this->animLands.push_back(DistantObject<DistantSky::AnimatedLandObject>(
			animLandObject, textureIndex));
	}

	for (int i = distantSky.getAirObjectCount() - 1; i >= 0; i--)
	{
		const DistantSky::AirObject &airObject = distantSky.getAirObject(i);
		const int entryIndex = airObject.getTextureEntryIndex();
		const int textureIndex = addSkyTexture(distantSky.getTexture(entryIndex));
		this->airs.push_back(DistantObject<DistantSky::AirObject>(airObject, textureIndex));
	}

	for (int i = distantSky.getMoonObjectCount() - 1; i >= 0; i--)
	{
		const DistantSky::MoonObject &moonObject = distantSky.getMoonObject(i);
		const int entryIndex = moonObject.getTextureEntryIndex();
		const int textureIndex = addSkyTexture(distantSky.getTexture(entryIndex));
		this->moons.push_back(DistantObject<DistantSky::MoonObject>(moonObject, textureIndex));
	}

	for (int i = distantSky.getStarObjectCount() - 1; i >= 0; i--)
	{
		const DistantSky::StarObject &starObject = distantSky.getStarObject(i);
		const int textureIndex = [&distantSky, &addSkyTexture, &addSmallStarTexture, &starObject]()
		{
			if (starObject.getType() == DistantSky::StarObject::Type::Small)
			{
				const DistantSky::StarObject::SmallStar &smallStar = starObject.getSmallStar();
				return addSmallStarTexture(smallStar.color);
			}
			else if (starObject.getType() == DistantSky::StarObject::Type::Large)
			{
				const DistantSky::StarObject::LargeStar &largeStar = starObject.getLargeStar();
				const int entryIndex = largeStar.entryIndex;
				return addSkyTexture(distantSky.getTexture(entryIndex));
			}
			else
			{
				DebugUnhandledReturnMsg(int,
					std::to_string(static_cast<int>(starObject.getType())));
			}
		}();

		this->stars.push_back(DistantObject<DistantSky::StarObject>(
			starObject, textureIndex));
	}

	if (distantSky.hasSun())
	{
		// Add the sun to the sky textures and assign its texture index.
		const int sunEntryIndex = distantSky.getSunEntryIndex();
		this->sunTextureIndex = addSkyTexture(distantSky.getTexture(sunEntryIndex));
	}
}

void SoftwareRenderer::DistantObjects::clear()
{
	this->lands.clear();
	this->animLands.clear();
	this->airs.clear();
	this->moons.clear();
	this->stars.clear();
	this->sunTextureIndex = DistantObjects::NO_SUN;
}

SoftwareRenderer::VisDistantObject::ParallaxData::ParallaxData()
{
	this->xVisAngleStart = 0.0;
	this->xVisAngleEnd = 0.0;
	this->uStart = 0.0;
	this->uEnd = 0.0;
}

SoftwareRenderer::VisDistantObject::ParallaxData::ParallaxData(double xVisAngleStart,
	double xVisAngleEnd, double uStart, double uEnd)
{
	this->xVisAngleStart = xVisAngleStart;
	this->xVisAngleEnd = xVisAngleEnd;
	this->uStart = uStart;
	this->uEnd = uEnd;
}

SoftwareRenderer::VisDistantObject::VisDistantObject(const SkyTexture &texture,
	DrawRange &&drawRange, ParallaxData &&parallax, double xProjStart, double xProjEnd,
	int xStart, int xEnd, bool emissive)
	: drawRange(std::move(drawRange)), parallax(std::move(parallax))
{
	this->texture = &texture;
	this->xProjStart = xProjStart;
	this->xProjEnd = xProjEnd;
	this->xStart = xStart;
	this->xEnd = xEnd;
	this->emissive = emissive;
}

SoftwareRenderer::VisDistantObject::VisDistantObject(const SkyTexture &texture,
	DrawRange &&drawRange, double xProjStart, double xProjEnd, int xStart, int xEnd, bool emissive)
	: VisDistantObject(texture, std::move(drawRange), ParallaxData(), xProjStart, xProjEnd,
		xStart, xEnd, emissive) { }

SoftwareRenderer::VisDistantObjects::VisDistantObjects()
{
	this->landStart = 0;
	this->landEnd = 0;
	this->animLandStart = 0;
	this->animLandEnd = 0;
	this->airStart = 0;
	this->airEnd = 0;
	this->moonStart = 0;
	this->moonEnd = 0;
	this->sunStart = 0;
	this->sunEnd = 0;
	this->starStart = 0;
	this->starEnd = 0;
}

void SoftwareRenderer::VisDistantObjects::clear()
{
	this->objs.clear();
	this->landStart = 0;
	this->landEnd = 0;
	this->animLandStart = 0;
	this->animLandEnd = 0;
	this->airStart = 0;
	this->airEnd = 0;
	this->moonStart = 0;
	this->moonEnd = 0;
	this->sunStart = 0;
	this->sunEnd = 0;
	this->starStart = 0;
	this->starEnd = 0;
}

void SoftwareRenderer::RenderThreadData::SkyGradient::init(double projectedYTop,
	double projectedYBottom, std::vector<Double3> &rowCache)
{
	this->threadsDone = 0;
	this->rowCache = &rowCache;
	this->projectedYTop = projectedYTop;
	this->projectedYBottom = projectedYBottom;
	this->shouldDrawStars = false;
}

void SoftwareRenderer::RenderThreadData::DistantSky::init(bool parallaxSky,
	const VisDistantObjects &visDistantObjs,
	const std::vector<SkyTexture> &skyTextures)
{
	this->threadsDone = 0;
	this->visDistantObjs = &visDistantObjs;
	this->skyTextures = &skyTextures;
	this->parallaxSky = parallaxSky;
	this->doneVisTesting = false;
}

void SoftwareRenderer::RenderThreadData::Voxels::init(double ceilingHeight,
	const std::vector<LevelData::DoorState> &openDoors,
	const std::vector<LevelData::FadeState> &fadingVoxels, const VoxelGrid &voxelGrid,
	const std::vector<VoxelTexture> &voxelTextures, std::vector<OcclusionData> &occlusion)
{
	this->threadsDone = 0;
	this->ceilingHeight = ceilingHeight;
	this->openDoors = &openDoors;
	this->fadingVoxels = &fadingVoxels;
	this->voxelGrid = &voxelGrid;
	this->voxelTextures = &voxelTextures;
	this->occlusion = &occlusion;
}

void SoftwareRenderer::RenderThreadData::Flats::init(const Double3 &flatNormal,
	const std::vector<VisibleFlat> &visibleFlats,
	const std::unordered_map<int, FlatTextureGroup> &flatTextureGroups)
{
	this->threadsDone = 0;
	this->flatNormal = &flatNormal;
	this->visibleFlats = &visibleFlats;
	this->flatTextureGroups = &flatTextureGroups;
	this->doneSorting = false;
}

SoftwareRenderer::RenderThreadData::RenderThreadData()
{
	// Make sure 'go' is initialized to false.
	this->totalThreads = 0;
	this->go = false;
	this->isDestructing = false;
	this->camera = nullptr;
	this->shadingInfo = nullptr;
	this->frame = nullptr;
}

void SoftwareRenderer::RenderThreadData::init(int totalThreads, const Camera &camera,
	const ShadingInfo &shadingInfo, const FrameView &frame)
{
	this->totalThreads = totalThreads;
	this->camera = &camera;
	this->shadingInfo = &shadingInfo;
	this->frame = &frame;
	this->go = false;
	this->isDestructing = false;
}

const double SoftwareRenderer::NEAR_PLANE = 0.0001;
const double SoftwareRenderer::FAR_PLANE = 1000.0;
const int SoftwareRenderer::DEFAULT_VOXEL_TEXTURE_COUNT = 64;
//const int SoftwareRenderer::DEFAULT_FLAT_TEXTURE_COUNT = 256; // Not used with flat texture groups.
const double SoftwareRenderer::DOOR_MIN_VISIBLE = 0.10;
const double SoftwareRenderer::SKY_GRADIENT_ANGLE = 30.0;
const double SoftwareRenderer::DISTANT_CLOUDS_MAX_ANGLE = 25.0;
const double SoftwareRenderer::TALL_PIXEL_RATIO = 1.20;

SoftwareRenderer::SoftwareRenderer()
{
	// Initialize values to empty.
	this->width = 0;
	this->height = 0;
	this->renderThreadsMode = 0;
	this->fogDistance = 0.0;
}

SoftwareRenderer::~SoftwareRenderer()
{
	this->resetRenderThreads();
}

bool SoftwareRenderer::isInited() const
{
	// Frame buffer area must be positive.
	return (this->width > 0) && (this->height > 0);
}

SoftwareRenderer::ProfilerData SoftwareRenderer::getProfilerData() const
{
	// @todo: make this a member of SoftwareRenderer eventually when it is capturing more
	// information in render(), etc..
	ProfilerData data;
	data.width = this->width;
	data.height = this->height;
	return data;
}

const void SoftwareRenderer::getFlatTexel(const Double2& uv, const int& flatIndex, const int& textureId, const double& anglePercent, const EntityAnimationData::StateType& animStateType, double& r, double& g, double& b, double& a) const
{
	// Set the out params to default values
	r = 0;
	g = 0;
	b = 0;
	a = 0;

	// Texture of the flat. It might be flipped horizontally as well, given by
		// the "flat.flipped" value.
	const auto iter = flatTextureGroups.find(flatIndex);
	if (iter == flatTextureGroups.end())
	{
		// No flat texture group available for the flat.
		return;
	}

	const FlatTextureGroup& flatTextureGroup = iter->second;
	const FlatTextureGroup::TextureList* textureList =
		flatTextureGroup.getTextureList(animStateType, anglePercent);

	if (textureList == nullptr)
	{
		// No flat textures allocated for the animation state.
		return;
	}

	const FlatTexture& texture = (*textureList)[textureId];

	// Use the UV to get the FlatTexel
	int x = (int)floor((uv.x * texture.width) + 0.5);
	int y = (int)floor((uv.y * texture.height) + 0.5);
	int coord = y * texture.width + x;

	auto texel = texture.texels[coord];

	r = texel.r;
	g = texel.g;
	b = texel.b;
	a = texel.a;
}

void SoftwareRenderer::init(int width, int height, int renderThreadsMode)
{
	// Initialize 2D frame buffer.
	const int pixelCount = width * height;
	this->depthBuffer = std::vector<double>(pixelCount,
		std::numeric_limits<double>::infinity());

	// Initialize occlusion columns.
	this->occlusion = std::vector<OcclusionData>(width, OcclusionData(0, height));

	// Initialize sky gradient cache.
	this->skyGradientRowCache = std::vector<Double3>(height, Double3::Zero);

	// Initialize texture vectors to default sizes.
	this->voxelTextures = std::vector<VoxelTexture>(SoftwareRenderer::DEFAULT_VOXEL_TEXTURE_COUNT);
	this->flatTextureGroups = std::unordered_map<int, FlatTextureGroup>();

	this->width = width;
	this->height = height;
	this->renderThreadsMode = renderThreadsMode;

	// Fog distance is zero by default.
	this->fogDistance = 0.0;

	// Initialize render threads.
	const int threadCount = SoftwareRenderer::getRenderThreadsFromMode(renderThreadsMode);
	this->initRenderThreads(width, height, threadCount);
}

void SoftwareRenderer::setRenderThreadsMode(int mode)
{
	this->renderThreadsMode = mode;

	// Re-initialize render threads.
	const int threadCount = SoftwareRenderer::getRenderThreadsFromMode(renderThreadsMode);
	this->initRenderThreads(this->width, this->height, threadCount);
}

void SoftwareRenderer::addLight(int id, const Double3 &point, const Double3 &color, 
	double intensity)
{
	DebugNotImplemented();
}

void SoftwareRenderer::setVoxelTexture(int id, const uint8_t *srcTexels, const Palette &palette)
{
	// Clear the selected texture.
	VoxelTexture &texture = this->voxelTextures.at(id);
	std::fill(texture.texels.begin(), texture.texels.end(), VoxelTexel());
	texture.lightTexels.clear();

	for (int y = 0; y < VoxelTexture::HEIGHT; y++)
	{
		for (int x = 0; x < VoxelTexture::WIDTH; x++)
		{
			// @todo: change this calculation for rotated textures. Make sure to have a 
			// source index and destination index.
			// - "dstX" and "dstY" should be calculated, and also used with lightTexels.
			const int index = x + (y * VoxelTexture::WIDTH);
			const uint8_t srcTexel = srcTexels[index];
			VoxelTexel voxelTexel = VoxelTexel::makeFrom8Bit(srcTexel, palette);
			texture.texels[index] = voxelTexel;

			// If it's a white texel, it's used with night lights (i.e., yellow at night).
			const bool isWhite = srcTexel == PALETTE_INDEX_NIGHT_LIGHT;

			if (isWhite)
			{
				texture.lightTexels.push_back(Int2(x, y));
			}
		}
	}
}

void SoftwareRenderer::addFlatTexture(int flatIndex, EntityAnimationData::StateType stateType,
	int angleID, bool flipped, const uint8_t *srcTexels, int width, int height,
	const Palette &palette)
{
	// If the flat mapping doesn't exist, add a new one.
	auto iter = this->flatTextureGroups.find(flatIndex);
	if (iter == this->flatTextureGroups.end())
	{
		iter = this->flatTextureGroups.emplace(
			std::make_pair(flatIndex, FlatTextureGroup())).first;
	}

	FlatTextureGroup &flatTextureGroup = iter->second;
	flatTextureGroup.addTexture(stateType, angleID, flipped, srcTexels, width, height, palette);
}

void SoftwareRenderer::updateLight(int id, const Double3 *point,
	const Double3 *color, const double *intensity)
{
	DebugNotImplemented();
}

void SoftwareRenderer::setFogDistance(double fogDistance)
{
	this->fogDistance = fogDistance;
}

void SoftwareRenderer::setDistantSky(const DistantSky &distantSky, const Palette &palette)
{
	// Clear old distant sky data.
	this->distantObjects.clear();
	this->skyTextures.clear();

	// Create distant objects and set the sky textures.
	this->distantObjects.init(distantSky, this->skyTextures, palette);
}

void SoftwareRenderer::setSkyPalette(const uint32_t *colors, int count)
{
	this->skyPalette = std::vector<Double3>(count);

	for (size_t i = 0; i < this->skyPalette.size(); i++)
	{
		this->skyPalette[i] = Double3::fromRGB(colors[i]);
	}
}

void SoftwareRenderer::setNightLightsActive(bool active)
{
	// @todo: activate lights (don't worry about textures).

	// Change voxel texels based on whether it's night.
	const Double4 texelColor = Double4::fromARGB(
		(active ? Color(255, 166, 0) : Color::Black).toARGB());
	const double texelEmission = active ? 1.0 : 0.0;

	for (auto &voxelTexture : this->voxelTextures)
	{
		auto &texels = voxelTexture.texels;

		for (const auto &lightTexels : voxelTexture.lightTexels)
		{
			const int index = lightTexels.x + (lightTexels.y * VoxelTexture::WIDTH);

			VoxelTexel &texel = texels.at(index);
			texel.r = texelColor.x;
			texel.g = texelColor.y;
			texel.b = texelColor.z;
			texel.transparent = texelColor.w == 0.0;
			texel.emission = texelEmission;
		}
	}
}

void SoftwareRenderer::removeLight(int id)
{
	DebugNotImplemented();
}

void SoftwareRenderer::clearTextures()
{
	for (auto &texture : this->voxelTextures)
	{
		std::fill(texture.texels.begin(), texture.texels.end(), VoxelTexel());
		texture.lightTexels.clear();
	}

	this->flatTextureGroups.clear();

	// Distant sky textures are cleared because the vector size is managed internally.
	this->skyTextures.clear();
	this->distantObjects.sunTextureIndex = SoftwareRenderer::DistantObjects::NO_SUN;
}

void SoftwareRenderer::clearDistantSky()
{
	this->distantObjects.clear();
}

void SoftwareRenderer::resize(int width, int height)
{
	const int pixelCount = width * height;
	this->depthBuffer.resize(pixelCount);
	std::fill(this->depthBuffer.begin(), this->depthBuffer.end(), 
		std::numeric_limits<double>::infinity());

	this->occlusion.resize(width);
	std::fill(this->occlusion.begin(), this->occlusion.end(), OcclusionData(0, height));

	this->skyGradientRowCache.resize(height);
	std::fill(this->skyGradientRowCache.begin(), this->skyGradientRowCache.end(), Double3::Zero);

	this->width = width;
	this->height = height;

	// Restart render threads with new dimensions.
	const int threadCount = SoftwareRenderer::getRenderThreadsFromMode(this->renderThreadsMode);
	this->initRenderThreads(width, height, threadCount);
}

void SoftwareRenderer::initRenderThreads(int width, int height, int threadCount)
{
	// If there are existing threads, reset them.
	if (this->renderThreads.size() > 0)
	{
		this->resetRenderThreads();
	}

	// If more or fewer threads are requested, re-allocate the render thread list.
	if (this->renderThreads.size() != threadCount)
	{
		this->renderThreads.resize(threadCount);
	}

	// Block width and height are the approximate number of columns and rows per thread,
	// respectively.
	const double blockWidth = static_cast<double>(width) / static_cast<double>(threadCount);
	const double blockHeight = static_cast<double>(height) / static_cast<double>(threadCount);

	// Start thread loop for each render thread. Rounding is involved so the start and stop
	// coordinates are correct for all resolutions.
	for (size_t i = 0; i < this->renderThreads.size(); i++)
	{
		const int threadIndex = static_cast<int>(i);
		const int startX = static_cast<int>(std::round(static_cast<double>(i) * blockWidth));
		const int endX = static_cast<int>(std::round(static_cast<double>(i + 1) * blockWidth));
		const int startY = static_cast<int>(std::round(static_cast<double>(i) * blockHeight));
		const int endY = static_cast<int>(std::round(static_cast<double>(i + 1) * blockHeight));

		// Make sure the rounding is correct.
		DebugAssert(startX >= 0);
		DebugAssert(endX <= width);
		DebugAssert(startY >= 0);
		DebugAssert(endY <= height);

		this->renderThreads[i] = std::thread(SoftwareRenderer::renderThreadLoop,
			std::ref(this->threadData), threadIndex, startX, endX, startY, endY);
	}
}

void SoftwareRenderer::resetRenderThreads()
{
	// Tell each render thread it needs to terminate.
	std::unique_lock<std::mutex> lk(this->threadData.mutex);
	this->threadData.go = true;
	this->threadData.isDestructing = true;
	lk.unlock();
	this->threadData.condVar.notify_all();

	for (auto &thread : this->renderThreads)
	{
		if (thread.joinable())
		{
			thread.join();
		}
	}

	// Set signal variables back to defaults, in case the render threads are used again.
	this->threadData.go = false;
	this->threadData.isDestructing = false;
}

void SoftwareRenderer::updateVisibleDistantObjects(bool parallaxSky,
	const ShadingInfo &shadingInfo, const Camera &camera, const FrameView &frame)
{
	this->visDistantObjs.clear();

	// Directions forward and along the edges of the 2D frustum.
	const Double2 forward(camera.forwardX, camera.forwardZ);
	const Double2 frustumLeft(camera.frustumLeftX, camera.frustumLeftZ);
	const Double2 frustumRight(camera.frustumRightX, camera.frustumRightZ);

	// Directions perpendicular to frustum vectors, for determining what points
	// are inside the frustum. Both directions point towards the inside.
	const Double2 frustumLeftPerp = frustumLeft.rightPerp();
	const Double2 frustumRightPerp = frustumRight.leftPerp();

	// Determines the vertical offset of the rendered object's origin on-screen. Most
	// objects have their origin at the bottom, but the sun has its origin at the top so
	// that when it's 6am or 6pm, its top edge will be at the horizon.
	enum class Orientation { Top, Bottom };

	// Lambda for checking if the given object properties make it appear on-screen, and if
	// so, adding it to the visible objects list.
	auto tryAddObject = [this, parallaxSky, &camera, &frame, &forward, &frustumLeftPerp,
		&frustumRightPerp](const SkyTexture &texture, double xAngleRadians, double yAngleRadians,
			bool emissive, Orientation orientation)
	{
		const double objWidth = static_cast<double>(texture.width) / DistantSky::IDENTITY_DIM;
		const double objHeight = static_cast<double>(texture.height) / DistantSky::IDENTITY_DIM;
		const double objHalfWidth = objWidth * 0.50;

		// Y position on-screen is the same regardless of parallax.
		DrawRange drawRange = [yAngleRadians, orientation, objHeight, &camera, &frame]()
		{
			// Project the bottom first then add the object's height above it in screen-space
			// to get the top. This keeps objects from appearing squished the higher they are
			// in the sky. Don't need to worry about cases when the Y angle is at an extreme;
			// the start and end projections will both be off-screen (i.e., +inf or -inf).
			const Double3 objDirBottom = Double3(
				camera.forwardX,
				std::tan(yAngleRadians),
				camera.forwardZ).normalized();

			const Double3 objPointBottom = camera.eye + objDirBottom;

			const double yProjEnd = SoftwareRenderer::getProjectedY(
				objPointBottom, camera.transform, camera.yShear);
			const double yProjStart = yProjEnd - (objHeight * camera.zoom);

			const double yProjBias = (orientation == Orientation::Top) ?
				(yProjEnd - yProjStart) : 0.0;

			const double yProjScreenStart = (yProjStart + yProjBias) * frame.heightReal;
			const double yProjScreenEnd = (yProjEnd + yProjBias) * frame.heightReal;

			const int yStart = SoftwareRenderer::getLowerBoundedPixel(
				yProjScreenStart, frame.height);
			const int yEnd = SoftwareRenderer::getUpperBoundedPixel(
				yProjScreenEnd, frame.height);

			return DrawRange(yProjScreenStart, yProjScreenEnd, yStart, yEnd);
		}();

		// The position of the object's left and right edges depends on whether parallax
		// is enabled.
		if (parallaxSky)
		{
			// Get X angles for left and right edges based on object half width.
			const double xDeltaRadians = objHalfWidth * DistantSky::IDENTITY_ANGLE_RADIANS;
			const double xAngleRadiansLeft = xAngleRadians + xDeltaRadians;
			const double xAngleRadiansRight = xAngleRadians - xDeltaRadians;
			
			// Camera's horizontal field of view.
			const double cameraHFov = MathUtils::verticalFovToHorizontalFov(
				camera.fovY, camera.aspect);
			const double halfCameraHFovRadians = (cameraHFov * 0.50) * Constants::DegToRad;

			// Angles of the camera's forward vector and frustum edges.
			const double cameraAngleRadians = camera.getXZAngleRadians();
			const double cameraAngleLeft = cameraAngleRadians + halfCameraHFovRadians;
			const double cameraAngleRight = cameraAngleRadians - halfCameraHFovRadians;

			// Distant object visible angle range and texture coordinates, set by onScreen.
			double xVisAngleLeft, xVisAngleRight;
			double uStart, uEnd;

			// Determine if the object is at least partially on-screen. The angle range of the
			// object must be at least partially within the angle range of the camera.
			const bool onScreen = [&camera, xAngleRadiansLeft, xAngleRadiansRight, cameraAngleLeft,
				cameraAngleRight, &xVisAngleLeft, &xVisAngleRight, &uStart, &uEnd]()
			{
				// Need to handle special cases where the angle ranges span 0.
				const bool cameraIsGeneralCase = cameraAngleLeft < Constants::TwoPi;
				const bool objectIsGeneralCase = xAngleRadiansLeft < Constants::TwoPi;

				if (cameraIsGeneralCase == objectIsGeneralCase)
				{
					// Both are either general case or special case; no extra behavior necessary.
					xVisAngleLeft = std::min(xAngleRadiansLeft, cameraAngleLeft);
					xVisAngleRight = std::max(xAngleRadiansRight, cameraAngleRight);
				}
				else if (!cameraIsGeneralCase)
				{
					// Camera special case.
					// @todo: cut into two parts?
					xVisAngleLeft = std::min(
						xAngleRadiansLeft, (cameraAngleLeft - Constants::TwoPi));
					xVisAngleRight = std::max(
						xAngleRadiansRight, (cameraAngleRight - Constants::TwoPi));
				}
				else
				{
					// Object special case.
					// @todo: cut into two parts?
					xVisAngleLeft = std::min(
						(xAngleRadiansLeft - Constants::TwoPi), cameraAngleLeft);
					xVisAngleRight = std::max(
						(xAngleRadiansRight - Constants::TwoPi), cameraAngleRight);
				}

				uStart = 1.0 - ((xVisAngleLeft - xAngleRadiansRight) /
					(xAngleRadiansLeft - xAngleRadiansRight));
				uEnd = Constants::JustBelowOne - ((xAngleRadiansRight - xVisAngleRight) /
					(xAngleRadiansRight - xAngleRadiansLeft));

				return (xAngleRadiansLeft >= cameraAngleRight) &&
					(xAngleRadiansRight <= cameraAngleLeft);
			}();

			if (onScreen)
			{
				// Data for parallax texture sampling.
				VisDistantObject::ParallaxData parallax(
					xVisAngleLeft, xVisAngleRight, uStart, uEnd);

				const Double2 objDirLeft2D(
					std::sin(xAngleRadiansLeft),
					std::cos(xAngleRadiansLeft));
				const Double2 objDirRight2D(
					std::sin(xAngleRadiansRight),
					std::cos(xAngleRadiansRight));

				// Project vertical edges.
				const Double3 objDirLeft(objDirLeft2D.x, 0.0, objDirLeft2D.y);
				const Double3 objDirRight(objDirRight2D.x, 0.0, objDirRight2D.y);

				const Double3 objPointLeft = camera.eye + objDirLeft;
				const Double3 objPointRight = camera.eye + objDirRight;

				const Double4 objProjPointLeft = camera.transform * Double4(objPointLeft, 1.0);
				const Double4 objProjPointRight = camera.transform * Double4(objPointRight, 1.0);

				const double xProjStart = 0.50 + ((objProjPointLeft.x / objProjPointLeft.w) * 0.50);
				const double xProjEnd = 0.50 + ((objProjPointRight.x / objProjPointRight.w) * 0.50);

				// Get the start and end X pixel coordinates.
				const int xDrawStart = SoftwareRenderer::getLowerBoundedPixel(
					xProjStart * frame.widthReal, frame.width);
				const int xDrawEnd = SoftwareRenderer::getUpperBoundedPixel(
					xProjEnd * frame.widthReal, frame.width);

				this->visDistantObjs.objs.push_back(VisDistantObject(
					texture, std::move(drawRange), std::move(parallax), xProjStart, xProjEnd,
					xDrawStart, xDrawEnd, emissive));
			}
		}
		else
		{
			// Classic rendering. Render the object based on its midpoint.
			const Double3 objDir(
				std::sin(xAngleRadians),
				0.0,
				std::cos(xAngleRadians));

			// Create a point arbitrarily far away for the object's center in world space.
			const Double3 objPoint = camera.eye + objDir;

			// Project the center point on-screen and get its projected X coordinate.
			const Double4 objProjPoint = camera.transform * Double4(objPoint, 1.0);
			const double xProjCenter = 0.50 + ((objProjPoint.x / objProjPoint.w) * 0.50);

			// Calculate the projected width of the object so we can get the left and right X
			// coordinates on-screen.
			const double objProjWidth = (objWidth * camera.zoom) /
				(camera.aspect * SoftwareRenderer::TALL_PIXEL_RATIO);
			const double objProjHalfWidth = objProjWidth * 0.50;

			// Left and right coordinates of the object in screen space.
			const double xProjStart = xProjCenter - objProjHalfWidth;
			const double xProjEnd = xProjCenter + objProjHalfWidth;

			const Double2 objDir2D(objDir.x, objDir.z);
			const bool onScreen = (objDir2D.dot(forward) > 0.0) &&
				(xProjStart <= 1.0) && (xProjEnd >= 0.0);

			if (onScreen)
			{
				// Get the start and end X pixel coordinates.
				const int xDrawStart = SoftwareRenderer::getLowerBoundedPixel(
					xProjStart * frame.widthReal, frame.width);
				const int xDrawEnd = SoftwareRenderer::getUpperBoundedPixel(
					xProjEnd * frame.widthReal, frame.width);

				this->visDistantObjs.objs.push_back(VisDistantObject(
					texture, std::move(drawRange), xProjStart, xProjEnd, xDrawStart,
					xDrawEnd, emissive));
			}
		}
	};

	// Iterate all distant objects and gather up the visible ones. Set the start
	// and end ranges for each object type to be used during rendering for
	// different types of shading.
	this->visDistantObjs.landStart = 0;

	for (const auto &land : this->distantObjects.lands)
	{
		const SkyTexture &texture = this->skyTextures.at(land.textureIndex);
		const double xAngleRadians = land.obj.getAngleRadians();
		const double yAngleRadians = 0.0;
		const bool emissive = false;
		const Orientation orientation = Orientation::Bottom;

		tryAddObject(texture, xAngleRadians, yAngleRadians, emissive, orientation);
	}

	this->visDistantObjs.landEnd = static_cast<int>(this->visDistantObjs.objs.size());
	this->visDistantObjs.animLandStart = this->visDistantObjs.landEnd;

	for (const auto &animLand : this->distantObjects.animLands)
	{
		const SkyTexture &texture = this->skyTextures.at(
			animLand.textureIndex + animLand.obj.getIndex());
		const double xAngleRadians = animLand.obj.getAngleRadians();
		const double yAngleRadians = 0.0;
		const bool emissive = true;
		const Orientation orientation = Orientation::Bottom;

		tryAddObject(texture, xAngleRadians, yAngleRadians, emissive, orientation);
	}

	this->visDistantObjs.animLandEnd = static_cast<int>(this->visDistantObjs.objs.size());
	this->visDistantObjs.airStart = this->visDistantObjs.animLandEnd;

	for (const auto &air : this->distantObjects.airs)
	{
		const SkyTexture &texture = skyTextures.at(air.textureIndex);
		const double xAngleRadians = air.obj.getAngleRadians();
		const double yAngleRadians = [&air]()
		{
			// 0 is at horizon, 1 is at top of distant cloud height limit.
			const double gradientPercent = air.obj.getHeight();
			return gradientPercent *
				(SoftwareRenderer::DISTANT_CLOUDS_MAX_ANGLE * Constants::DegToRad);
		}();

		const bool emissive = false;
		const Orientation orientation = Orientation::Bottom;

		tryAddObject(texture, xAngleRadians, yAngleRadians, emissive, orientation);
	}

	this->visDistantObjs.airEnd = static_cast<int>(this->visDistantObjs.objs.size());
	this->visDistantObjs.moonStart = this->visDistantObjs.airEnd;

	// Objects in space have their position modified by latitude and time of day.
	// My quaternions are broken or something, so use matrix multiplication instead.
	const Matrix4d &timeRotation = shadingInfo.timeRotation;
	const Matrix4d &latitudeRotation = shadingInfo.latitudeRotation;

	auto getSpaceCorrectedAngles = [&timeRotation, &latitudeRotation](double xAngleRadians,
		double yAngleRadians, double &newXAngleRadians, double &newYAngleRadians)
	{
		// Direction towards the space object.
		const Double3 direction = Double3(
			std::sin(xAngleRadians),
			std::tan(yAngleRadians),
			std::cos(xAngleRadians)).normalized();

		// Rotate the direction based on latitude and time of day.
		const Double4 dir = latitudeRotation * (timeRotation * Double4(direction, 0.0));
		newXAngleRadians = std::atan2(dir.x, dir.z);
		newYAngleRadians = std::asin(dir.y);
	};

	for (const auto &moon : this->distantObjects.moons)
	{
		const SkyTexture &texture = skyTextures.at(moon.textureIndex);

		// These moon directions are roughly correct, based on the original game.
		const Double3 direction = [&moon]()
		{
			const DistantSky::MoonObject::Type type = moon.obj.getType();

			double bonusLatitude;
			const Double3 baseDir = [type, &bonusLatitude]()
			{
				if (type == DistantSky::MoonObject::Type::First)
				{
					bonusLatitude = -15.0 / 100.0;
					return Double3(0.0, -57536.0, 0.0).normalized();
				}
				else if (type == DistantSky::MoonObject::Type::Second)
				{
					bonusLatitude = -30.0 / 100.0;
					return Double3(-3000.0, -53536.0, 0.0).normalized();
				}
				else
				{
					DebugUnhandledReturnMsg(Double3, std::to_string(static_cast<int>(type)));
				}
			}();

			// The moon's position in the sky is modified by its current phase.
			const double phaseModifier = moon.obj.getPhasePercent() + bonusLatitude;
			const Matrix4d moonRotation = SoftwareRenderer::getLatitudeRotation(phaseModifier);
			const Double4 dir = moonRotation * Double4(baseDir, 0.0);
			return Double3(dir.x, dir.y, dir.z).normalized();
		}();

		const double xAngleRadians = MathUtils::fullAtan2(direction.x, direction.z);
		const double yAngleRadians = direction.getYAngleRadians();
		const bool emissive = true;
		const Orientation orientation = Orientation::Top;

		// Modify angle based on latitude and time of day.
		double newXAngleRadians, newYAngleRadians;
		getSpaceCorrectedAngles(xAngleRadians, yAngleRadians, newXAngleRadians, newYAngleRadians);

		tryAddObject(texture, newXAngleRadians, newYAngleRadians, emissive, orientation);
	}

	this->visDistantObjs.moonEnd = static_cast<int>(this->visDistantObjs.objs.size());
	this->visDistantObjs.sunStart = this->visDistantObjs.moonEnd;

	// Try to add the sun to the visible distant objects.
	if (this->distantObjects.sunTextureIndex != SoftwareRenderer::DistantObjects::NO_SUN)
	{
		const SkyTexture &sunTexture = this->skyTextures.at(this->distantObjects.sunTextureIndex);

		// The sun direction is already corrected for latitude and time of day since the same
		// variable is reused with shading.
		const Double3 &sunDirection = shadingInfo.sunDirection;
		const double sunXAngleRadians = MathUtils::fullAtan2(sunDirection.x, sunDirection.z);

		// When the sun is directly above or below, it might cause the X angle to be undefined.
		// We want to filter this out before we try projecting it on-screen.
		if (std::isfinite(sunXAngleRadians))
		{
			const double sunYAngleRadians = sunDirection.getYAngleRadians();
			const bool sunEmissive = true;
			const Orientation sunOrientation = Orientation::Top;

			tryAddObject(sunTexture, sunXAngleRadians, sunYAngleRadians,
				sunEmissive, sunOrientation);
		}
	}

	this->visDistantObjs.sunEnd = static_cast<int>(this->visDistantObjs.objs.size());
	this->visDistantObjs.starStart = this->visDistantObjs.sunEnd;

	for (const auto &star : this->distantObjects.stars)
	{
		const SkyTexture &texture = skyTextures.at(star.textureIndex);

		const Double3 &direction = star.obj.getDirection();
		const double xAngleRadians = MathUtils::fullAtan2(direction.x, direction.z);
		const double yAngleRadians = direction.getYAngleRadians();
		const bool emissive = true;
		const Orientation orientation = Orientation::Bottom;

		// Modify angle based on latitude and time of day.
		double newXAngleRadians, newYAngleRadians;
		getSpaceCorrectedAngles(xAngleRadians, yAngleRadians, newXAngleRadians, newYAngleRadians);

		tryAddObject(texture, newXAngleRadians, newYAngleRadians, emissive, orientation);
	}

	this->visDistantObjs.starEnd = static_cast<int>(this->visDistantObjs.objs.size());
}

void SoftwareRenderer::updateVisibleFlats(const Camera &camera, double ceilingHeight,
	double fogDistance, const VoxelGrid &voxelGrid, const EntityManager &entityManager)
{
	this->visibleFlats.clear();

	// Potentially visible entities buffer. Don't need to clear because it gets
	// overwritten with a set amount of new data each frame.
	this->potentiallyVisibleFlats.resize(entityManager.getTotalCount());
	const int entityCount = entityManager.getTotalEntities(
		this->potentiallyVisibleFlats.data(),
		static_cast<int>(this->potentiallyVisibleFlats.size()));

	// Each flat shares the same axes. The forward direction always faces opposite to 
	// the camera direction.
	const Double3 flatForward = Double3(-camera.forwardX, 0.0, -camera.forwardZ).normalized();
	const Double3 flatUp = Double3::UnitY;
	const Double3 flatRight = flatForward.cross(flatUp).normalized();

	const Double2 eye2D(camera.eye.x, camera.eye.z);
	const Double2 cameraDir(camera.forwardX, camera.forwardZ);

	// Potentially visible flat determination algorithm, given the current camera.
	for (int i = 0; i < entityCount; i++)
	{
		const Entity &entity = *this->potentiallyVisibleFlats[i];

		// Get the entity's data definition and animation.
		const EntityData &entityData = *entityManager.getEntityData(entity.getDataIndex());
		const EntityAnimationData &entityAnimData = entityData.getAnimationData();

		// Get active state.
		const EntityAnimationData::Instance &animInstance = entity.getAnimation();
		const std::vector<EntityAnimationData::State> &stateList = animInstance.getStateList(entityAnimData);
		const int stateCount = static_cast<int>(stateList.size()); // 1 if it's the same for all angles.

		// Calculate state index based on entity direction relative to camera.
		const double animAngle = [&eye2D, &cameraDir, &entity, stateCount]()
		{
			if (entity.getEntityType() == EntityType::Static)
			{
				// Static entities always face the camera.
				return 0.0;
			}
			else if (entity.getEntityType() == EntityType::Dynamic)
			{
				// Dynamic entities are angle-dependent.
				const DynamicEntity &dynamicEntity = static_cast<const DynamicEntity&>(entity);
				const Double2 &entityDir = dynamicEntity.getDirection();
				const Double2 diffDir = (eye2D - entity.getPosition()).normalized();

				const double entityAngle = MathUtils::fullAtan2(entityDir.y, entityDir.x);
				const double diffAngle = MathUtils::fullAtan2(diffDir.y, diffDir.x);

				// Use the difference of the two vectors as the angle vector.
				const Double2 resultDir = entityDir - diffDir;
				const double resultAngle = Constants::Pi + MathUtils::fullAtan2(resultDir.y, resultDir.x);

				// Angle bias so the final direction is centered within its angle range.
				const double angleBias = (Constants::TwoPi / static_cast<double>(stateCount)) * 0.50;

				return std::fmod(resultAngle + angleBias, Constants::TwoPi);
			}
			else
			{
				DebugUnhandledReturnMsg(double,
					std::to_string(static_cast<int>(entity.getEntityType())));
			}
		}();

		const double anglePercent = std::clamp(animAngle / Constants::TwoPi, 0.0, Constants::JustBelowOne);
		const int stateIndex = [stateCount, anglePercent]()
		{
			const int index = static_cast<int>(static_cast<double>(stateCount) * anglePercent);
			return std::clamp(index, 0, stateCount - 1);
		}();

		DebugAssertIndex(stateList, stateIndex);
		const EntityAnimationData::State &animState = stateList[stateIndex];
		
		// Get the entity's current animation frame (dimensions, texture, etc.).
		const EntityAnimationData::Keyframe &keyframe = [&entity, &entityAnimData, &animInstance,
			stateIndex, &animState]() -> const EntityAnimationData::Keyframe&
		{
			const int keyframeIndex = animInstance.getKeyframeIndex(stateIndex, entityAnimData);
			const BufferView<const EntityAnimationData::Keyframe> keyframes = animState.getKeyframes();
			return keyframes.get(keyframeIndex);
		}();

		const double flatWidth = keyframe.getWidth();
		const double flatHeight = keyframe.getHeight();
		const double flatHalfWidth = flatWidth * 0.50;

		const Double2 &entityPos = entity.getPosition();
		const double entityPosX = entityPos.x;
		const double entityPosZ = entityPos.y;

		const double flatYOffset =
			static_cast<double>(-entityData.getYOffset()) / MIFFile::ARENA_UNITS;

		// If the entity is in a raised platform voxel, they are set on top of it.
		const double raisedPlatformYOffset = [ceilingHeight, &voxelGrid, &entityPos]()
		{
			const Int2 entityVoxelPos(
				static_cast<int>(entityPos.x),
				static_cast<int>(entityPos.y));
			const uint16_t voxelID = voxelGrid.getVoxel(entityVoxelPos.x, 1, entityVoxelPos.y);
			const VoxelData &voxelData = voxelGrid.getVoxelData(voxelID);

			if (voxelData.dataType == VoxelDataType::Raised)
			{
				const VoxelData::RaisedData &raised = voxelData.raised;
				return (raised.yOffset + raised.ySize) * ceilingHeight;
			}
			else
			{
				// No raised platform offset.
				return 0.0;
			}
		}();

		// Bottom center of flat.
		const Double3 flatPosition(
			entityPosX,
			ceilingHeight + flatYOffset + raisedPlatformYOffset,
			entityPosZ);
		const Double2 flatPosition2D(
			flatPosition.x,
			flatPosition.z);

		// Check if the flat is somewhere in front of the camera.
		const Double2 flatEyeDiff = flatPosition2D - eye2D;
		const double flatEyeDiffLen = flatEyeDiff.length();
		const Double2 flatEyeDir = flatEyeDiff / flatEyeDiffLen;
		const bool inFrontOfCamera = cameraDir.dot(flatEyeDir) > 0.0;

		// Check if the flat is within the fog distance. Treat the flat as a cylinder and
		// see if it's inside the fog distance circle centered on the player. Can't use
		// distance squared here because a^2 - b^2 does not equal (a - b)^2.
		const double flatRadius = flatHalfWidth;
		const double flatEyeCylinderDist = flatEyeDiffLen - flatRadius;
		const bool inFogDistance = flatEyeCylinderDist < fogDistance;

		if (inFrontOfCamera && inFogDistance)
		{
			// Scaled axes based on flat dimensions.
			const Double3 flatRightScaled = flatRight * flatHalfWidth;
			const Double3 flatUpScaled = flatUp * flatHeight;

			// Determine if the flat is potentially visible to the camera.
			VisibleFlat visFlat;
			visFlat.flatIndex = entityData.getFlatIndex();
			visFlat.animStateType = animState.getType();

			// Calculate each corner of the flat in world space.
			visFlat.bottomLeft = flatPosition + flatRightScaled;
			visFlat.bottomRight = flatPosition - flatRightScaled;
			visFlat.topLeft = visFlat.bottomLeft + flatUpScaled;
			visFlat.topRight = visFlat.bottomRight + flatUpScaled;

			// Now project two of the flat's opposing corner points into camera space.
			// The Z value is used with flat sorting (not rendering), and the X and Y values 
			// are used to find where the flat is on-screen.
			Double4 projStart = camera.transform * Double4(visFlat.topLeft, 1.0);
			Double4 projEnd = camera.transform * Double4(visFlat.bottomRight, 1.0);

			// Normalize coordinates.
			projStart = projStart / projStart.w;
			projEnd = projEnd / projEnd.w;

			// Assign each screen value to the flat frame data.
			visFlat.startX = 0.50 + (projStart.x * 0.50);
			visFlat.endX = 0.50 + (projEnd.x * 0.50);
			visFlat.startY = (0.50 + camera.yShear) - (projStart.y * 0.50);
			visFlat.endY = (0.50 + camera.yShear) - (projEnd.y * 0.50);
			visFlat.z = projStart.z;

			// Check that the projected values are within view and are inside the near
			// and far clip planes.
			const bool inScreenX = (visFlat.startX < 1.0) && (visFlat.endX > 0.0);
			const bool inScreenY = (visFlat.startY < 1.0) && (visFlat.endY > 0.0);
			const bool inPlanes = (visFlat.z >= SoftwareRenderer::NEAR_PLANE) &&
				(visFlat.z <= SoftwareRenderer::FAR_PLANE);

			if (inScreenX && inScreenY && inPlanes)
			{
				// Finish initializing the visible flat.
				visFlat.textureID = keyframe.getTextureID();
				visFlat.anglePercent = anglePercent;

				// Add the flat data to the draw list.
				this->visibleFlats.push_back(std::move(visFlat));
			}
		}
	}

	// Sort the visible flats farthest to nearest (relevant for transparencies).
	std::sort(this->visibleFlats.begin(), this->visibleFlats.end(),
		[](const VisibleFlat &a, const VisibleFlat &b) { return a.z > b.z; });
}

/*Double3 SoftwareRenderer::castRay(const Double3 &direction,
	const VoxelGrid &voxelGrid) const
{
	// This is an extension of Lode Vandevenne's DDA algorithm from 2D to 3D.
	// Technically, it could be considered a "3D-DDA" algorithm. It will eventually 
	// have some additional features so all of Arena's geometry can be represented.

	// @todo:
	// - Figure out proper DDA lengths for variable-height voxels, and why using
	//   voxelHeight squared instead of 1.0 in deltaDist.y looks weird (sideDist.y?).
	// - Cuboids within voxels (bridges, beds, shelves) with variable Y offset and size.
	// - Sprites (SpriteGrid? Sprite data, and list of sprite IDs per voxel).
	// - Transparent textures (check texel alpha in DDA loop).
	// - Sky (if hitID == 0).
	// - Shading (shadows from the sun, point lights).

	// Some floating point behavior assumptions:
	// -> (value / 0.0) == infinity
	// -> (value / infinity) == 0.0
	// -> (int)(-0.8) == 0
	// -> (int)floor(-0.8) == -1
	// -> (int)ceil(-0.8) == 0

	const Double3 dirSquared(
		direction.x * direction.x,
		direction.y * direction.y,
		direction.z * direction.z);

	// Height (Y size) of each voxel in the voxel grid. Some levels in Arena have
	// "tall" voxels, so the voxel height must be a variable.
	const double voxelHeight = voxelGrid.getVoxelHeight();

	// A custom variable that represents the Y "floor" of the current voxel.
	const double eyeYRelativeFloor = std::floor(this->eye.y / voxelHeight) * voxelHeight;

	// Calculate delta distances along each axis. These determine how far
	// the ray has to go until the next X, Y, or Z side is hit, respectively.
	const Double3 deltaDist(
		std::sqrt(1.0 + (dirSquared.y / dirSquared.x) + (dirSquared.z / dirSquared.x)),
		std::sqrt(1.0 + (dirSquared.x / dirSquared.y) + (dirSquared.z / dirSquared.y)),
		std::sqrt(1.0 + (dirSquared.x / dirSquared.z) + (dirSquared.y / dirSquared.z)));

	// Booleans for whether a ray component is non-negative. Used with step directions 
	// and texture coordinates.
	const bool nonNegativeDirX = direction.x >= 0.0;
	const bool nonNegativeDirY = direction.y >= 0.0;
	const bool nonNegativeDirZ = direction.z >= 0.0;

	// Calculate step directions and initial side distances.
	Int3 step;
	Double3 sideDist;
	if (nonNegativeDirX)
	{
		step.x = 1;
		sideDist.x = (this->startCellReal.x + 1.0 - this->eye.x) * deltaDist.x;
	}
	else
	{
		step.x = -1;
		sideDist.x = (this->eye.x - this->startCellReal.x) * deltaDist.x;
	}

	if (nonNegativeDirY)
	{
		step.y = 1;
		sideDist.y = (eyeYRelativeFloor + voxelHeight - this->eye.y) * deltaDist.y;
	}
	else
	{
		step.y = -1;
		sideDist.y = (this->eye.y - eyeYRelativeFloor) * deltaDist.y;
	}

	if (nonNegativeDirZ)
	{
		step.z = 1;
		sideDist.z = (this->startCellReal.z + 1.0 - this->eye.z) * deltaDist.z;
	}
	else
	{
		step.z = -1;
		sideDist.z = (this->eye.z - this->startCellReal.z) * deltaDist.z;
	}

	// Make a copy of the initial side distances. They are used for the special case 
	// of the ray ending in the same voxel it started in.
	const Double3 initialSideDist = sideDist;

	// Make a copy of the step magnitudes, converted to doubles.
	const Double3 stepReal(
		static_cast<double>(step.x),
		static_cast<double>(step.y),
		static_cast<double>(step.z));

	// Get initial voxel coordinates.
	Int3 cell = this->startCell;

	// ID of a hit voxel. Zero (air) by default.
	char hitID = 0;

	// Axis of a hit voxel's side. X by default.
	enum class Axis { X, Y, Z };
	Axis axis = Axis::X;

	// Distance squared (in voxels) that the ray has stepped. Square roots are
	// too slow to use in the DDA loop, so this is used instead.
	// - When using variable-sized voxels, this may be calculated differently.
	double cellDistSquared = 0.0;

	// Offset values for which corner of a voxel to compare the distance 
	// squared against. The correct corner to use is important when culling
	// shapes at max view distance.
	const Double3 startCellWithOffset(
		this->startCellReal.x + ((1.0 + stepReal.x) / 2.0),
		eyeYRelativeFloor + (((1.0 + stepReal.y) / 2.0) * voxelHeight),
		this->startCellReal.z + ((1.0 + stepReal.z) / 2.0));
	const Double3 cellOffset(
		(1.0 - stepReal.x) / 2.0,
		((1.0 - stepReal.y) / 2.0) * voxelHeight,
		(1.0 - stepReal.z) / 2.0);

	// Get dimensions of the voxel grid.
	const int gridWidth = voxelGrid.getWidth();
	const int gridHeight = voxelGrid.getHeight();
	const int gridDepth = voxelGrid.getDepth();

	// Check world bounds on the start voxel. Bounds are partially recalculated 
	// for axes that the DDA loop is stepping through.
	bool voxelIsValid = (cell.x >= 0) && (cell.y >= 0) && (cell.z >= 0) &&
		(cell.x < gridWidth) && (cell.y < gridHeight) && (cell.z < gridDepth);

	// Step through the voxel grid while the current coordinate is valid and
	// the total voxel distance stepped is less than the view distance.
	// (Note that the "voxel distance" is not the same as "actual" distance.)
	const char *voxels = voxelGrid.getVoxels();
	while (voxelIsValid && (cellDistSquared < this->viewDistSquared))
	{
		// Get the index of the current voxel in the voxel grid.
		const int gridIndex = cell.x + (cell.y * gridWidth) +
			(cell.z * gridWidth * gridHeight);

		// Check if the current voxel is solid.
		const char voxelID = voxels[gridIndex];

		if (voxelID > 0)
		{
			hitID = voxelID;
			break;
		}

		if ((sideDist.x < sideDist.y) && (sideDist.x < sideDist.z))
		{
			sideDist.x += deltaDist.x;
			cell.x += step.x;
			axis = Axis::X;
			voxelIsValid &= (cell.x >= 0) && (cell.x < gridWidth);
		}
		else if (sideDist.y < sideDist.z)
		{
			sideDist.y += deltaDist.y;
			cell.y += step.y;
			axis = Axis::Y;
			voxelIsValid &= (cell.y >= 0) && (cell.y < gridHeight);
		}
		else
		{
			sideDist.z += deltaDist.z;
			cell.z += step.z;
			axis = Axis::Z;
			voxelIsValid &= (cell.z >= 0) && (cell.z < gridDepth);
		}

		// Refresh how far the current cell is from the start cell, squared.
		// The "offsets" move each point to the correct corner for each voxel
		// so that the stepping stops correctly at max view distance.
		const Double3 cellDiff(
			(static_cast<double>(cell.x) + cellOffset.x) - startCellWithOffset.x,
			(static_cast<double>(cell.y) + cellOffset.y) - startCellWithOffset.y,
			(static_cast<double>(cell.z) + cellOffset.z) - startCellWithOffset.z);
		cellDistSquared = (cellDiff.x * cellDiff.x) + (cellDiff.y * cellDiff.y) +
			(cellDiff.z * cellDiff.z);
	}

	// Boolean for whether the ray ended in the same voxel it started in.
	const bool stoppedInFirstVoxel = cell == this->startCell;

	// Get the distance from the camera to the hit point. It is a special case
	// if the ray stopped in the first voxel.
	double distance;
	if (stoppedInFirstVoxel)
	{
		if ((initialSideDist.x < initialSideDist.y) &&
			(initialSideDist.x < initialSideDist.z))
		{
			distance = initialSideDist.x;
			axis = Axis::X;
		}
		else if (initialSideDist.y < initialSideDist.z)
		{
			distance = initialSideDist.y;
			axis = Axis::Y;
		}
		else
		{
			distance = initialSideDist.z;
			axis = Axis::Z;
		}
	}
	else
	{
		const size_t axisIndex = static_cast<size_t>(axis);

		// Assign to distance based on which axis was hit.
		if (axis == Axis::X)
		{
			distance = (static_cast<double>(cell.x) - this->eye.x +
				((1.0 - stepReal.x) / 2.0)) / direction.x;
		}
		else if (axis == Axis::Y)
		{
			distance = ((static_cast<double>(cell.y) * voxelHeight) - this->eye.y +
				(((1.0 - stepReal.y) / 2.0) * voxelHeight)) / direction.y;
		}
		else
		{
			distance = (static_cast<double>(cell.z) - this->eye.z +
				((1.0 - stepReal.z) / 2.0)) / direction.z;
		}
	}

	// If there was a hit, get the shaded color.
	if (hitID > 0)
	{
		// Intersection point on the voxel.
		const Double3 hitPoint = this->eye + (direction * distance);

		// Boolean for whether the hit point is on the back of a voxel face.
		const bool backFace = stoppedInFirstVoxel;

		// Texture coordinates. U and V are affected by which side is hit (near, far),
		// and whether the hit point is on the front or back of the voxel face.
		// - Note, for edge cases where {u,v}Val == 1.0, the texture coordinate is
		//   out of bounds by one pixel, so instead of 1.0, something like 0.9999999
		//   should be used instead. std::nextafter(1.0, -INFINITY)?
		double u, v;
		if (axis == Axis::X)
		{
			const double uVal = hitPoint.z - std::floor(hitPoint.z);

			u = (nonNegativeDirX ^ backFace) ? uVal : (1.0 - uVal);
			//v = 1.0 - (hitPoint.y - std::floor(hitPoint.y));
			v = 1.0 - (std::fmod(hitPoint.y, voxelHeight) / voxelHeight);
		}
		else if (axis == Axis::Y)
		{
			const double vVal = hitPoint.x - std::floor(hitPoint.x);

			u = hitPoint.z - std::floor(hitPoint.z);
			v = (nonNegativeDirY ^ backFace) ? vVal : (1.0 - vVal);
		}
		else
		{
			const double uVal = hitPoint.x - std::floor(hitPoint.x);

			u = (nonNegativeDirZ ^ backFace) ? (1.0 - uVal) : uVal;
			//v = 1.0 - (hitPoint.y - std::floor(hitPoint.y));
			v = 1.0 - (std::fmod(hitPoint.y, voxelHeight) / voxelHeight);
		}

		// -- temp --
		// Display bad texture coordinates as magenta. If any of these is true, it
		// means something above is wrong.
		if ((u < 0.0) || (u >= 1.0) || (v < 0.0) || (v >= 1.0))
		{
			return Double3(1.0, 0.0, 1.0);
		}
		// -- end temp --

		// Get the voxel data associated with the ID. Subtract 1 because the first
		// entry is at index 0 but the lowest hitID is 1.
		const VoxelData &voxelData = voxelGrid.getVoxelData(hitID - 1);

		// Get the texture depending on which face was hit.
		const TextureData &texture = (axis == Axis::Y) ?
			this->textures[voxelData.floorAndCeilingID] :
			this->textures[voxelData.sideID];

		// Calculate position in texture.
		const int textureX = static_cast<int>(u * texture.width);
		const int textureY = static_cast<int>(v * texture.height);

		// Get the texel color at the hit point.
		// - Later, the alpha component can be used for transparency and ignoring
		//   intersections (in the DDA loop).
		const Double4 &texel = texture.pixels[textureX + (textureY * texture.width)];

		// Convert the texel to a 3-component color.
		const Double3 color(texel.x, texel.y, texel.z);

		// Linearly interpolate with some depth.
		const double depth = std::min(distance, this->viewDistance) / this->viewDistance;
		return color.lerp(this->fogColor, depth);
	}
	else
	{
		// No intersection. Return sky color.
		return this->fogColor;
	}
}*/

int SoftwareRenderer::getRenderThreadsFromMode(int mode)
{
	if (mode == 0)
	{
		// Very low.
		return 1;
	}
	else if (mode == 1)
	{
		// Low.
		return std::max(Platform::getThreadCount() / 4, 1);
	}
	else if (mode == 2)
	{
		// Medium.
		return std::max(Platform::getThreadCount() / 2, 1);
	}
	else if (mode == 3)
	{
		// High.
		return std::max((3 * Platform::getThreadCount()) / 4, 1);
	}
	else if (mode == 4)
	{
		// Very high.
		return std::max(Platform::getThreadCount() - 1, 1);
	}
	else if (mode == 5)
	{
		// Max.
		return Platform::getThreadCount();
	}
	else
	{
		DebugUnhandledReturnMsg(int, std::to_string(mode));
	}
}

VoxelData::Facing SoftwareRenderer::getInitialChasmFarFacing(int voxelX, int voxelZ,
	const Double2 &eye, const Ray &ray)
{
	// Angle of the ray from the camera eye.
	const double angle = MathUtils::fullAtan2(ray.dirX, ray.dirZ);

	// Corners in world space.
	const Double2 bottomLeftCorner(
		static_cast<double>(voxelX),
		static_cast<double>(voxelZ));
	const Double2 topLeftCorner(
		bottomLeftCorner.x + 1.0,
		bottomLeftCorner.y);
	const Double2 bottomRightCorner(
		bottomLeftCorner.x,
		bottomLeftCorner.y + 1.0);
	const Double2 topRightCorner(
		topLeftCorner.x,
		bottomRightCorner.y);

	const Double2 upLeft = (topLeftCorner - eye).normalized();
	const Double2 upRight = (topRightCorner - eye).normalized();
	const Double2 downLeft = (bottomLeftCorner - eye).normalized();
	const Double2 downRight = (bottomRightCorner - eye).normalized();
	const double upLeftAngle = MathUtils::fullAtan2(upLeft.x, upLeft.y);
	const double upRightAngle = MathUtils::fullAtan2(upRight.x, upRight.y);
	const double downLeftAngle = MathUtils::fullAtan2(downLeft.x, downLeft.y);
	const double downRightAngle = MathUtils::fullAtan2(downRight.x, downRight.y);

	// Find which range the ray's angle lies within.
	if ((angle < upRightAngle) || (angle > downRightAngle))
	{
		return VoxelData::Facing::PositiveZ;
	}
	else if (angle < upLeftAngle)
	{
		return VoxelData::Facing::PositiveX;
	}
	else if (angle < downLeftAngle)
	{
		return VoxelData::Facing::NegativeZ;
	}
	else
	{
		return VoxelData::Facing::NegativeX;
	}
}

VoxelData::Facing SoftwareRenderer::getChasmFarFacing(int voxelX, int voxelZ, 
	VoxelData::Facing nearFacing, const Camera &camera, const Ray &ray)
{
	const Double2 eye2D(camera.eye.x, camera.eye.z);
	
	// Angle of the ray from the camera eye.
	const double angle = MathUtils::fullAtan2(ray.dirX, ray.dirZ);

	// Corners in world space.
	const Double2 bottomLeftCorner(
		static_cast<double>(voxelX),
		static_cast<double>(voxelZ));
	const Double2 topLeftCorner(
		bottomLeftCorner.x + 1.0,
		bottomLeftCorner.y);
	const Double2 bottomRightCorner(
		bottomLeftCorner.x,
		bottomLeftCorner.y + 1.0);
	const Double2 topRightCorner(
		topLeftCorner.x,
		bottomRightCorner.y);

	const Double2 upLeft = (topLeftCorner - eye2D).normalized();
	const Double2 upRight = (topRightCorner - eye2D).normalized();
	const Double2 downLeft = (bottomLeftCorner - eye2D).normalized();
	const Double2 downRight = (bottomRightCorner - eye2D).normalized();
	const double upLeftAngle = MathUtils::fullAtan2(upLeft.x, upLeft.y);
	const double upRightAngle = MathUtils::fullAtan2(upRight.x, upRight.y);
	const double downLeftAngle = MathUtils::fullAtan2(downLeft.x, downLeft.y);
	const double downRightAngle = MathUtils::fullAtan2(downRight.x, downRight.y);

	// Find which side it starts on, then do some checks against line angles.
	// When the ray origin is at a diagonal to the voxel, ignore the corner
	// closest to that origin.
	if (nearFacing == VoxelData::Facing::PositiveX)
	{
		// Starts on (1.0, z).
		const bool onRight = camera.eyeVoxel.z > voxelZ;
		const bool onLeft = camera.eyeVoxel.z < voxelZ;
		
		if (onRight)
		{
			// Ignore top-right corner.
			if (angle < downLeftAngle)
			{
				return VoxelData::Facing::NegativeZ;
			}
			else
			{
				return VoxelData::Facing::NegativeX;
			}
		}
		else if (onLeft)
		{
			// Ignore top-left corner.
			if ((angle > downLeftAngle) && (angle < downRightAngle))
			{
				return VoxelData::Facing::NegativeX;
			}
			else
			{
				return VoxelData::Facing::PositiveZ;
			}
		}
		else
		{
			if (angle > downRightAngle)
			{
				return VoxelData::Facing::PositiveZ;
			}
			else if (angle > downLeftAngle)
			{
				return VoxelData::Facing::NegativeX;
			}
			else
			{
				return VoxelData::Facing::NegativeZ;
			}
		}
	}
	else if (nearFacing == VoxelData::Facing::NegativeX)
	{
		// Starts on (0.0, z).
		const bool onRight = camera.eyeVoxel.z > voxelZ;
		const bool onLeft = camera.eyeVoxel.z < voxelZ;

		if (onRight)
		{
			// Ignore bottom-right corner.
			if (angle < upLeftAngle)
			{
				return VoxelData::Facing::PositiveX;
			}
			else
			{
				return VoxelData::Facing::NegativeZ;
			}
		}
		else if (onLeft)
		{
			// Ignore bottom-left corner.
			if (angle < upRightAngle)
			{
				return VoxelData::Facing::PositiveZ;
			}
			else
			{
				return VoxelData::Facing::PositiveX;
			}
		}
		else
		{
			if (angle < upRightAngle)
			{
				return VoxelData::Facing::PositiveZ;
			}
			else if (angle < upLeftAngle)
			{
				return VoxelData::Facing::PositiveX;
			}
			else
			{
				return VoxelData::Facing::NegativeZ;
			}
		}
	}				
	else if (nearFacing == VoxelData::Facing::PositiveZ)
	{
		// Starts on (x, 1.0).
		const bool onTop = camera.eyeVoxel.x > voxelX;
		const bool onBottom = camera.eyeVoxel.x < voxelX;

		if (onTop)
		{
			// Ignore top-right corner.
			if (angle < downLeftAngle)
			{
				return VoxelData::Facing::NegativeZ;
			}
			else
			{
				return VoxelData::Facing::NegativeX;
			}
		}
		else if (onBottom)
		{
			// Ignore bottom-right corner.
			if (angle < upLeftAngle)
			{
				return VoxelData::Facing::PositiveX;
			}
			else
			{
				return VoxelData::Facing::NegativeZ;
			}
		}
		else
		{
			if (angle < upLeftAngle)
			{
				return VoxelData::Facing::PositiveX;
			}
			else if (angle < downLeftAngle)
			{
				return VoxelData::Facing::NegativeZ;
			}
			else
			{
				return VoxelData::Facing::NegativeX;
			}
		}
	}
	else
	{
		// Starts on (x, 0.0). This one splits the origin, so it needs some 
		// special cases.
		const bool onTop = camera.eyeVoxel.x > voxelX;
		const bool onBottom = camera.eyeVoxel.x < voxelX;

		if (onTop)
		{
			// Ignore top-left corner.
			if ((angle > downLeftAngle) && (angle < downRightAngle))
			{
				return VoxelData::Facing::NegativeX;
			}
			else
			{
				return VoxelData::Facing::PositiveZ;
			}
		}
		else if (onBottom)
		{
			// Ignore bottom-left corner.
			if ((angle > upRightAngle) && (angle < upLeftAngle))
			{
				return VoxelData::Facing::PositiveX;
			}
			else
			{
				return VoxelData::Facing::PositiveZ;
			}
		}
		else
		{
			if ((angle < upRightAngle) || (angle > downRightAngle))
			{
				return VoxelData::Facing::PositiveZ;
			}
			else if (angle > downLeftAngle)
			{
				return VoxelData::Facing::NegativeX;
			}
			else
			{
				return VoxelData::Facing::PositiveX;
			}
		}
	}
}

double SoftwareRenderer::getDoorPercentOpen(int voxelX, int voxelZ,
	const std::vector<LevelData::DoorState> &openDoors)
{
	const Int2 voxel(voxelX, voxelZ);
	const auto iter = std::find_if(openDoors.begin(), openDoors.end(),
		[&voxel](const LevelData::DoorState &openDoor)
	{
		return openDoor.getVoxel() == voxel;
	});

	return (iter != openDoors.end()) ? iter->getPercentOpen() : 0.0;
}

double SoftwareRenderer::getFadingVoxelPercent(int voxelX, int voxelY, int voxelZ,
	const std::vector<LevelData::FadeState> &fadingVoxels)
{
	const Int3 voxel(voxelX, voxelY, voxelZ);
	const auto iter = std::find_if(fadingVoxels.begin(), fadingVoxels.end(),
		[&voxel](const LevelData::FadeState &fadeState)
	{
		return fadeState.getVoxel() == voxel;
	});

	return (iter != fadingVoxels.end()) ?
		std::clamp(1.0 - iter->getPercentDone(), 0.0, 1.0) : 1.0;
}

double SoftwareRenderer::getProjectedY(const Double3 &point, 
	const Matrix4d &transform, double yShear)
{	
	// Just do 3D projection for the Y and W coordinates instead of a whole
	// matrix * vector4 multiplication to keep from doing some unnecessary work.
	double projectedY, projectedW;
	transform.ywMultiply(point, projectedY, projectedW);

	// Convert the projected Y to normalized coordinates.
	projectedY /= projectedW;

	// Calculate the Y position relative to the center row of the screen, and offset it by 
	// the Y-shear. Multiply by 0.5 for the correct aspect ratio.
	return (0.50 + yShear) - (projectedY * 0.50);
}

int SoftwareRenderer::getLowerBoundedPixel(double projected, int frameDim)
{
	return std::clamp(static_cast<int>(std::ceil(projected - 0.50)), 0, frameDim);
}

int SoftwareRenderer::getUpperBoundedPixel(double projected, int frameDim)
{
	return std::clamp(static_cast<int>(std::floor(projected + 0.50)), 0, frameDim);
}

SoftwareRenderer::DrawRange SoftwareRenderer::makeDrawRange(const Double3 &startPoint,
	const Double3 &endPoint, const Camera &camera, const FrameView &frame)
{
	const double yProjStart = SoftwareRenderer::getProjectedY(
		startPoint, camera.transform, camera.yShear) * frame.heightReal;
	const double yProjEnd = SoftwareRenderer::getProjectedY(
		endPoint, camera.transform, camera.yShear) * frame.heightReal;
	const int yStart = SoftwareRenderer::getLowerBoundedPixel(yProjStart, frame.height);
	const int yEnd = SoftwareRenderer::getUpperBoundedPixel(yProjEnd, frame.height);

	return DrawRange(yProjStart, yProjEnd, yStart, yEnd);
}

std::array<SoftwareRenderer::DrawRange, 2> SoftwareRenderer::makeDrawRangeTwoPart(
	const Double3 &startPoint, const Double3 &midPoint, const Double3 &endPoint,
	const Camera &camera, const FrameView &frame)
{
	const double startYProjStart = SoftwareRenderer::getProjectedY(
		startPoint, camera.transform, camera.yShear) * frame.heightReal;
	const double startYProjEnd = SoftwareRenderer::getProjectedY(
		midPoint, camera.transform, camera.yShear) * frame.heightReal;
	const double endYProjEnd = SoftwareRenderer::getProjectedY(
		endPoint, camera.transform, camera.yShear) * frame.heightReal;

	const int startYStart = SoftwareRenderer::getLowerBoundedPixel(startYProjStart, frame.height);
	const int startYEnd = SoftwareRenderer::getUpperBoundedPixel(startYProjEnd, frame.height);
	const int endYStart = startYEnd;
	const int endYEnd = SoftwareRenderer::getUpperBoundedPixel(endYProjEnd, frame.height);

	return std::array<DrawRange, 2>
	{
		DrawRange(startYProjStart, startYProjEnd, startYStart, startYEnd),
		DrawRange(startYProjEnd, endYProjEnd, endYStart, endYEnd)
	};
}

std::array<SoftwareRenderer::DrawRange, 3> SoftwareRenderer::makeDrawRangeThreePart(
	const Double3 &startPoint, const Double3 &midPoint1, const Double3 &midPoint2,
	const Double3 &endPoint, const Camera &camera, const FrameView &frame)
{
	const double startYProjStart = SoftwareRenderer::getProjectedY(
		startPoint, camera.transform, camera.yShear) * frame.heightReal;
	const double startYProjEnd = SoftwareRenderer::getProjectedY(
		midPoint1, camera.transform, camera.yShear) * frame.heightReal;
	const double mid1YProjEnd = SoftwareRenderer::getProjectedY(
		midPoint2, camera.transform, camera.yShear) * frame.heightReal;
	const double mid2YProjEnd = SoftwareRenderer::getProjectedY(
		endPoint, camera.transform, camera.yShear) * frame.heightReal;

	const int startYStart = SoftwareRenderer::getLowerBoundedPixel(startYProjStart, frame.height);
	const int startYEnd = SoftwareRenderer::getUpperBoundedPixel(startYProjEnd, frame.height);
	const int mid1YStart = startYEnd;
	const int mid1YEnd = SoftwareRenderer::getUpperBoundedPixel(mid1YProjEnd, frame.height);
	const int mid2YStart = mid1YEnd;
	const int mid2YEnd = SoftwareRenderer::getUpperBoundedPixel(mid2YProjEnd, frame.height);

	return std::array<DrawRange, 3>
	{
		DrawRange(startYProjStart, startYProjEnd, startYStart, startYEnd),
		DrawRange(startYProjEnd, mid1YProjEnd, mid1YStart, mid1YEnd),
		DrawRange(mid1YProjEnd, mid2YProjEnd, mid2YStart, mid2YEnd)
	};
}

Matrix4d SoftwareRenderer::getLatitudeRotation(double latitude)
{
	return Matrix4d::zRotation(latitude * (Constants::Pi / 8.0));
}

Matrix4d SoftwareRenderer::getTimeOfDayRotation(double daytimePercent)
{
	return Matrix4d::xRotation(daytimePercent * Constants::TwoPi);
}

void SoftwareRenderer::getSkyGradientProjectedYRange(const Camera &camera, double &projectedYTop,
	double &projectedYBottom)
{
	// Get two points some arbitrary distance away from the camera to use as the top
	// and bottom reference points of the sky gradient.
	const Double3 forward = Double3(camera.forwardX, 0.0, camera.forwardZ).normalized();

	// Determine the sky gradient's position on-screen by getting the projected Y percentages for
	// the start and end. If these values are less than 0 or greater than 1, they are off-screen.
	projectedYTop = [&camera, &forward]()
	{
		const Double3 gradientTopPoint = [&camera, &forward]()
		{
			// Top of the sky gradient is some angle above the horizon.
			const double gradientAngleRadians =
				SoftwareRenderer::SKY_GRADIENT_ANGLE * Constants::DegToRad;

			// Height of the gradient's triangle with width of 1 and angle of 30 degrees.
			const double upPercent = std::tan(gradientAngleRadians);
			const Double3 up = Double3::UnitY;

			// Direction from camera eye to the top of the sky gradient.
			const Double3 gradientTopDir = (forward + (up * upPercent)).normalized();

			return camera.eye + gradientTopDir;
		}();

		return SoftwareRenderer::getProjectedY(
			gradientTopPoint, camera.transform, camera.yShear);
	}();

	projectedYBottom = [&camera, &forward]()
	{
		const Double3 gradientBottomPoint = camera.eye + forward;
		return SoftwareRenderer::getProjectedY(
			gradientBottomPoint, camera.transform, camera.yShear);
	}();
}

double SoftwareRenderer::getSkyGradientPercent(double projectedY, double projectedYTop,
	double projectedYBottom)
{
	// The sky gradient percent is 0 at the horizon and just below 1 at the top (for sky texture
	// coordinates).
	return Constants::JustBelowOne - std::clamp(
		(projectedY - projectedYTop) / (projectedYBottom - projectedYTop),
		0.0, Constants::JustBelowOne);
}

Double3 SoftwareRenderer::getSkyGradientRowColor(double gradientPercent, const ShadingInfo &shadingInfo)
{
	// Determine which sky color index the percent falls into, and how much of that
	// color to interpolate with the next one.
	const auto &skyColors = shadingInfo.skyColors;
	const int skyColorCount = static_cast<int>(skyColors.size());
	const double realIndex = gradientPercent * static_cast<double>(skyColorCount);
	const double percent = realIndex - std::floor(realIndex);
	const int index = static_cast<int>(realIndex);
	const int nextIndex = std::clamp(index + 1, 0, skyColorCount - 1);
	const Double3 &color = skyColors.at(index);
	const Double3 &nextColor = skyColors.at(nextIndex);
	return color.lerp(nextColor, percent);
}

bool SoftwareRenderer::findDiag1Intersection(int voxelX, int voxelZ, const Double2 &nearPoint,
	const Double2 &farPoint, RayHit &hit)
{
	// Start, middle, and end points of the diagonal line segment relative to the grid.
	const Double2 diagStart(
		static_cast<double>(voxelX),
		static_cast<double>(voxelZ));
	const Double2 diagMiddle(
		static_cast<double>(voxelX) + 0.50,
		static_cast<double>(voxelZ) + 0.50);
	const Double2 diagEnd(
		static_cast<double>(voxelX) + Constants::JustBelowOne,
		static_cast<double>(voxelZ) + Constants::JustBelowOne);

	// Normals for the left and right faces of the wall, facing up-left and down-right
	// respectively (magic number is sqrt(2) / 2).
	const Double3 leftNormal(0.7071068, 0.0, -0.7071068);
	const Double3 rightNormal(-0.7071068, 0.0, 0.7071068);
	
	// An intersection occurs if the near point and far point are on different sides 
	// of the diagonal line, or if the near point lies on the diagonal line. No need
	// to normalize the (localPoint - diagMiddle) vector because it's just checking
	// if it's greater than zero.
	const Double2 leftNormal2D(leftNormal.x, leftNormal.z);
	const bool nearOnLeft = leftNormal2D.dot(nearPoint - diagMiddle) >= 0.0;
	const bool farOnLeft = leftNormal2D.dot(farPoint - diagMiddle) >= 0.0;
	const bool intersectionOccurred = (nearOnLeft && !farOnLeft) || (!nearOnLeft && farOnLeft);

	// Only set the output data if an intersection occurred.
	if (intersectionOccurred)
	{
		// Change in X and change in Z of the incoming ray across the voxel.
		const double dx = farPoint.x - nearPoint.x;
		const double dz = farPoint.y - nearPoint.y;

		// The hit coordinate is a 0->1 value representing where the diagonal was hit.
		const double hitCoordinate = [&nearPoint, &diagStart, dx, dz]()
		{
			// Special cases: when the slope is horizontal or vertical. This method treats
			// the X axis as the vertical axis and the Z axis as the horizontal axis.
			const double isHorizontal = std::abs(dx) < Constants::Epsilon;
			const double isVertical = std::abs(dz) < Constants::Epsilon;

			if (isHorizontal)
			{
				// The X axis intercept is the intersection coordinate.
				return nearPoint.x - diagStart.x;
			}
			else if (isVertical)
			{
				// The Z axis intercept is the intersection coordinate.
				return nearPoint.y - diagStart.y;
			}
			else
			{
				// Slope of the diagonal line (trivial, x = z).
				const double diagSlope = 1.0;

				// Vertical axis intercept of the diagonal line.
				const double diagXIntercept = diagStart.x - diagStart.y;

				// Slope of the incoming ray.
				const double raySlope = dx / dz;

				// Get the vertical axis intercept of the incoming ray.
				const double rayXIntercept = nearPoint.x - (raySlope * nearPoint.y);

				// General line intersection calculation.
				return ((rayXIntercept - diagXIntercept) / 
					(diagSlope - raySlope)) - diagStart.y;
			}
		}();

		// Set the hit data.
		hit.u = std::clamp(hitCoordinate, 0.0, Constants::JustBelowOne);
		hit.point = Double2(
			static_cast<double>(voxelX) + hit.u,
			static_cast<double>(voxelZ) + hit.u);
		hit.innerZ = (hit.point - nearPoint).length();
		hit.normal = nearOnLeft ? leftNormal : rightNormal;

		return true;
	}
	else
	{
		// No intersection.
		return false;
	}
}

bool SoftwareRenderer::findDiag2Intersection(int voxelX, int voxelZ, const Double2 &nearPoint,
	const Double2 &farPoint, RayHit &hit)
{
	// Mostly a copy of findDiag1Intersection(), though with a couple different values
	// for the diagonal (end points, slope, etc.).

	// Start, middle, and end points of the diagonal line segment relative to the grid.
	const Double2 diagStart(
		static_cast<double>(voxelX) + Constants::JustBelowOne,
		static_cast<double>(voxelZ));
	const Double2 diagMiddle(
		static_cast<double>(voxelX) + 0.50,
		static_cast<double>(voxelZ) + 0.50);
	const Double2 diagEnd(
		static_cast<double>(voxelX),
		static_cast<double>(voxelZ) + Constants::JustBelowOne);

	// Normals for the left and right faces of the wall, facing up-right and down-left
	// respectively (magic number is sqrt(2) / 2).
	const Double3 leftNormal(0.7071068, 0.0, 0.7071068);
	const Double3 rightNormal(-0.7071068, 0.0, -0.7071068);

	// An intersection occurs if the near point and far point are on different sides 
	// of the diagonal line, or if the near point lies on the diagonal line. No need
	// to normalize the (localPoint - diagMiddle) vector because it's just checking
	// if it's greater than zero.
	const Double2 leftNormal2D(leftNormal.x, leftNormal.z);
	const bool nearOnLeft = leftNormal2D.dot(nearPoint - diagMiddle) >= 0.0;
	const bool farOnLeft = leftNormal2D.dot(farPoint - diagMiddle) >= 0.0;
	const bool intersectionOccurred = (nearOnLeft && !farOnLeft) || (!nearOnLeft && farOnLeft);

	// Only set the output data if an intersection occurred.
	if (intersectionOccurred)
	{
		// Change in X and change in Z of the incoming ray across the voxel.
		const double dx = farPoint.x - nearPoint.x;
		const double dz = farPoint.y - nearPoint.y;

		// The hit coordinate is a 0->1 value representing where the diagonal was hit.
		const double hitCoordinate = [&nearPoint, &diagStart, dx, dz]()
		{
			// Special cases: when the slope is horizontal or vertical. This method treats
			// the X axis as the vertical axis and the Z axis as the horizontal axis.
			const double isHorizontal = std::abs(dx) < Constants::Epsilon;
			const double isVertical = std::abs(dz) < Constants::Epsilon;

			if (isHorizontal)
			{
				// The X axis intercept is the compliment of the intersection coordinate.
				return Constants::JustBelowOne - (nearPoint.x - diagStart.x);
			}
			else if (isVertical)
			{
				// The Z axis intercept is the compliment of the intersection coordinate.
				return Constants::JustBelowOne - (nearPoint.y - diagStart.y);
			}
			else
			{
				// Slope of the diagonal line (trivial, x = -z).
				const double diagSlope = -1.0;

				// Vertical axis intercept of the diagonal line.
				const double diagXIntercept = diagStart.x + diagStart.y;

				// Slope of the incoming ray.
				const double raySlope = dx / dz;

				// Get the vertical axis intercept of the incoming ray.
				const double rayXIntercept = nearPoint.x - (raySlope * nearPoint.y);

				// General line intersection calculation.
				return ((rayXIntercept - diagXIntercept) /
					(diagSlope - raySlope)) - diagStart.y;
			}
		}();

		// Set the hit data.
		hit.u = std::clamp(hitCoordinate, 0.0, Constants::JustBelowOne);
		hit.point = Double2(
			static_cast<double>(voxelX) + (Constants::JustBelowOne - hit.u),
			static_cast<double>(voxelZ) + hit.u);
		hit.innerZ = (hit.point - nearPoint).length();
		hit.normal = nearOnLeft ? leftNormal : rightNormal;

		return true;
	}
	else
	{
		// No intersection.
		return false;
	}
}

bool SoftwareRenderer::findInitialEdgeIntersection(int voxelX, int voxelZ, 
	VoxelData::Facing edgeFacing, bool flipped, const Double2 &nearPoint, const Double2 &farPoint,
	const Camera &camera, const Ray &ray, RayHit &hit)
{
	// Reuse the chasm facing code to find which face is intersected.
	const VoxelData::Facing farFacing = SoftwareRenderer::getInitialChasmFarFacing(
		voxelX, voxelZ, Double2(camera.eye.x, camera.eye.z), ray);

	// If the edge facing and far facing match, there's an intersection.
	if (edgeFacing == farFacing)
	{
		hit.innerZ = (farPoint - nearPoint).length();
		hit.u = [flipped, &farPoint, farFacing]()
		{
			const double uVal = [&farPoint, farFacing]()
			{
				if (farFacing == VoxelData::Facing::PositiveX)
				{
					return Constants::JustBelowOne - (farPoint.y - std::floor(farPoint.y));
				}
				else if (farFacing == VoxelData::Facing::NegativeX)
				{
					return farPoint.y - std::floor(farPoint.y);
				}
				else if (farFacing == VoxelData::Facing::PositiveZ)
				{
					return farPoint.x - std::floor(farPoint.x);
				}
				else
				{
					return Constants::JustBelowOne - (farPoint.x - std::floor(farPoint.x));
				}
			}();

			// Account for the possibility of the texture being flipped horizontally.
			return std::clamp(!flipped ? uVal : (Constants::JustBelowOne - uVal),
				0.0, Constants::JustBelowOne);
		}();

		hit.point = farPoint;
		hit.normal = -VoxelData::getNormal(farFacing);
		return true;
	}
	else
	{
		// No intersection.
		return false;
	}
}

bool SoftwareRenderer::findEdgeIntersection(int voxelX, int voxelZ, VoxelData::Facing edgeFacing,
	bool flipped, VoxelData::Facing nearFacing, const Double2 &nearPoint, const Double2 &farPoint,
	double nearU, const Camera &camera, const Ray &ray, RayHit &hit)
{
	// If the edge facing and near facing match, the intersection is trivial.
	if (edgeFacing == nearFacing)
	{
		hit.innerZ = 0.0;
		hit.u = !flipped ? nearU : std::clamp(
			Constants::JustBelowOne - nearU, 0.0, Constants::JustBelowOne);
		hit.point = nearPoint;
		hit.normal = VoxelData::getNormal(nearFacing);
		return true;
	}
	else
	{
		// A search is needed to see whether an intersection occurred. Reuse the chasm
		// facing code to find what the far facing is.
		const VoxelData::Facing farFacing = SoftwareRenderer::getChasmFarFacing(
			voxelX, voxelZ, nearFacing, camera, ray);

		// If the edge facing and far facing match, there's an intersection.
		if (edgeFacing == farFacing)
		{
			hit.innerZ = (farPoint - nearPoint).length();
			hit.u = [flipped, &farPoint, farFacing]()
			{
				const double uVal = [&farPoint, farFacing]()
				{
					if (farFacing == VoxelData::Facing::PositiveX)
					{
						return Constants::JustBelowOne - (farPoint.y - std::floor(farPoint.y));
					}
					else if (farFacing == VoxelData::Facing::NegativeX)
					{
						return farPoint.y - std::floor(farPoint.y);
					}
					else if (farFacing == VoxelData::Facing::PositiveZ)
					{
						return farPoint.x - std::floor(farPoint.x);
					}
					else
					{
						return Constants::JustBelowOne - (farPoint.x - std::floor(farPoint.x));
					}
				}();

				// Account for the possibility of the texture being flipped horizontally.
				return std::clamp(!flipped ? uVal : (Constants::JustBelowOne - uVal),
					0.0, Constants::JustBelowOne);
			}();

			hit.point = farPoint;
			hit.normal = -VoxelData::getNormal(farFacing);
			return true;
		}
		else
		{
			// No intersection.
			return false;
		}
	}
}

bool SoftwareRenderer::findInitialSwingingDoorIntersection(int voxelX, int voxelZ,
	double percentOpen, const Double2 &nearPoint, const Double2 &farPoint, bool xAxis,
	const Camera &camera, const Ray &ray, RayHit &hit)
{
	// Decide which corner the door's hinge will be in, and create the line segment
	// that will be rotated based on percent open.
	Double2 interpStart;
	const Double2 pivot = [voxelX, voxelZ, xAxis, &interpStart]()
	{
		const Int2 corner = [voxelX, voxelZ, xAxis, &interpStart]()
		{
			if (xAxis)
			{
				interpStart = -Double2::UnitX;
				return Int2(voxelX + 1, voxelZ + 1);
			}
			else
			{
				interpStart = -Double2::UnitY;
				return Int2(voxelX, voxelZ + 1);
			}
		}();

		const Double2 cornerReal(
			static_cast<double>(corner.x),
			static_cast<double>(corner.y));

		// Bias the pivot towards the voxel center slightly to avoid Z-fighting with
		// adjacent walls.
		const Double2 voxelCenter(
			static_cast<double>(voxelX) + 0.50,
			static_cast<double>(voxelZ) + 0.50);
		const Double2 bias = (voxelCenter - cornerReal) * Constants::Epsilon;
		return cornerReal + bias;
	}();

	// Use the left perpendicular vector of the door's closed position as the 
	// fully open position.
	const Double2 interpEnd = interpStart.leftPerp();

	// Actual position of the door in its rotation, represented as a vector.
	const Double2 doorVec = interpStart.lerp(interpEnd, 1.0 - percentOpen).normalized();

	// Use back-face culling with swinging doors so it's not obstructing the player's
	// view as much when it's opening.
	const Double2 eye2D(camera.eye.x, camera.eye.z);
	const bool isFrontFace = (eye2D - pivot).normalized().dot(doorVec.leftPerp()) > 0.0;

	if (isFrontFace)
	{
		// Vector cross product in 2D, returns a scalar.
		auto cross = [](const Double2 &a, const Double2 &b)
		{
			return (a.x * b.y) - (b.x * a.y);
		};

		// Solve line segment intersection between the incoming ray and the door.
		const Double2 p1 = pivot;
		const Double2 v1 = doorVec;
		const Double2 p2 = nearPoint;
		const Double2 v2 = farPoint - nearPoint;

		// Percent from p1 to (p1 + v1).
		const double t = cross(p2 - p1, v2) / cross(v1, v2);

		// See if the two line segments intersect.
		if ((t >= 0.0) && (t < 1.0))
		{
			// Hit.
			hit.point = p1 + (v1 * t);
			hit.innerZ = (hit.point - nearPoint).length();
			hit.u = t;
			hit.normal = [&v1]()
			{
				const Double2 norm2D = v1.rightPerp();
				return Double3(norm2D.x, 0.0, norm2D.y);
			}();

			return true;
		}
		else
		{
			// No hit.
			return false;
		}
	}
	else
	{
		// Cull back face.
		return false;
	}
}

bool SoftwareRenderer::findInitialDoorIntersection(int voxelX, int voxelZ,
	VoxelData::DoorData::Type doorType, double percentOpen, const Double2 &nearPoint,
	const Double2 &farPoint, const Camera &camera, const Ray &ray,
	const VoxelGrid &voxelGrid, RayHit &hit)
{
	// Determine which axis the door should open/close for (either X or Z).
	const bool xAxis = [voxelX, voxelZ, &voxelGrid]()
	{
		// Check adjacent voxels on the X axis for air.
		auto voxelIsAir = [&voxelGrid](int x, int z)
		{
			const bool insideGrid = (x >= 0) && (x < voxelGrid.getWidth()) &&
				(z >= 0) && (z < voxelGrid.getDepth());

			if (insideGrid)
			{
				const uint16_t voxelID = voxelGrid.getVoxel(x, 1, z);
				const VoxelData &voxelData = voxelGrid.getVoxelData(voxelID);
				return voxelData.dataType == VoxelDataType::None;
			}
			else
			{
				// Anything past the map edge is considered air.
				return true;
			}
		};

		// If voxels (x - 1, z) and (x + 1, z) are empty, return true.
		return voxelIsAir(voxelX - 1, voxelZ) && voxelIsAir(voxelX + 1, voxelZ);
	}();

	// If the current intersection surface is along one of the voxel's edges, treat the door
	// like a wall by basing intersection calculations on the far facing.
	const bool useFarFacing = [doorType, percentOpen]()
	{
		const bool isClosed = percentOpen == 0.0;
		return isClosed ||
			(doorType == VoxelData::DoorData::Type::Sliding) ||
			(doorType == VoxelData::DoorData::Type::Raising) ||
			(doorType == VoxelData::DoorData::Type::Splitting);
	}();

	if (useFarFacing)
	{
		// Treat the door like a wall. Reuse the chasm facing code to find which face is
		// intersected.
		const VoxelData::Facing farFacing = SoftwareRenderer::getInitialChasmFarFacing(
			voxelX, voxelZ, Double2(camera.eye.x, camera.eye.z), ray);
		const VoxelData::Facing doorFacing = xAxis ?
			VoxelData::Facing::PositiveX : VoxelData::Facing::PositiveZ;

		if (doorFacing == farFacing)
		{
			// The ray intersected the target facing. See if the door itself was intersected
			// and write out hit data based on the door type.
			const double farU = [&farPoint, xAxis]()
			{
				const double uVal = [&farPoint, xAxis]()
				{
					if (xAxis)
					{
						return Constants::JustBelowOne - (farPoint.y - std::floor(farPoint.y));
					}
					else
					{
						return farPoint.x - std::floor(farPoint.x);
					}
				}();

				return std::clamp(uVal, 0.0, Constants::JustBelowOne);
			}();

			if (doorType == VoxelData::DoorData::Type::Swinging)
			{
				// Treat like a wall.
				hit.innerZ = (farPoint - nearPoint).length();
				hit.u = farU;
				hit.point = farPoint;
				hit.normal = -VoxelData::getNormal(farFacing);
				return true;
			}
			else if (doorType == VoxelData::DoorData::Type::Sliding)
			{
				// If far U coordinate is within percent closed, it's a hit. At 100% open,
				// a sliding door is still partially visible.
				const double minVisible = SoftwareRenderer::DOOR_MIN_VISIBLE;
				const double visibleAmount = 1.0 - ((1.0 - minVisible) * percentOpen);
				if (visibleAmount > farU)
				{
					hit.innerZ = (farPoint - nearPoint).length();
					hit.u = std::clamp(
						farU + (1.0 - visibleAmount), 0.0, Constants::JustBelowOne);
					hit.point = farPoint;
					hit.normal = -VoxelData::getNormal(farFacing);
					return true;
				}
				else
				{
					// No hit.
					return false;
				}
			}
			else if (doorType == VoxelData::DoorData::Type::Raising)
			{
				// Raising doors are always hit.
				hit.innerZ = (farPoint - nearPoint).length();
				hit.u = farU;
				hit.point = farPoint;
				hit.normal = -VoxelData::getNormal(farFacing);
				return true;
			}
			else if (doorType == VoxelData::DoorData::Type::Splitting)
			{
				// If far U coordinate is within percent closed on left or right half, it's a hit.
				// At 100% open, a splitting door is still partially visible.
				const double minVisible = SoftwareRenderer::DOOR_MIN_VISIBLE;
				const bool leftHalf = farU < 0.50;
				const bool rightHalf = farU > 0.50;
				double leftVisAmount, rightVisAmount;
				const bool success = [percentOpen, farU, minVisible, leftHalf, rightHalf,
					&leftVisAmount, &rightVisAmount]()
				{
					if (leftHalf)
					{
						// Left half.
						leftVisAmount = 0.50 - ((0.50 - minVisible) * percentOpen);
						return farU <= leftVisAmount;
					}
					else if (rightHalf)
					{
						// Right half.
						rightVisAmount = 0.50 + ((0.50 - minVisible) * percentOpen);
						return farU >= rightVisAmount;
					}
					else
					{
						// Midpoint (only when door is completely closed).
						return percentOpen == 0.0;
					}
				}();

				if (success)
				{
					// Hit.
					hit.innerZ = (farPoint - nearPoint).length();
					hit.u = [farU, leftHalf, rightHalf, leftVisAmount, rightVisAmount]()
					{
						const double u = [farU, leftHalf, rightHalf, leftVisAmount, rightVisAmount]()
						{
							if (leftHalf)
							{
								return (farU + 0.50) - leftVisAmount;
							}
							else if (rightHalf)
							{
								return (farU + 0.50) - rightVisAmount;
							}
							else
							{
								// Midpoint.
								return 0.50;
							}
						}();

						return std::clamp(u, 0.0, Constants::JustBelowOne);
					}();

					hit.point = farPoint;
					hit.normal = -VoxelData::getNormal(farFacing);

					return true;
				}
				else
				{
					// No hit.
					return false;
				}
			}
			else
			{
				// Invalid door type.
				return false;
			}
		}
		else
		{
			// No hit.
			return false;
		}
	}
	else if (doorType == VoxelData::DoorData::Type::Swinging)
	{
		return SoftwareRenderer::findInitialSwingingDoorIntersection(voxelX, voxelZ, percentOpen,
			nearPoint, farPoint, xAxis, camera, ray, hit);
	}
	else
	{
		// Invalid door type.
		return false;
	}
}

bool SoftwareRenderer::findSwingingDoorIntersection(int voxelX, int voxelZ,
	double percentOpen, VoxelData::Facing nearFacing, const Double2 &nearPoint,
	const Double2 &farPoint, double nearU, RayHit &hit)
{
	// Decide which corner the door's hinge will be in, and create the line segment
	// that will be rotated based on percent open.
	Double2 interpStart;
	const Double2 pivot = [voxelX, voxelZ, nearFacing, &interpStart]()
	{
		const Int2 corner = [voxelX, voxelZ, nearFacing, &interpStart]()
		{
			if (nearFacing == VoxelData::Facing::PositiveX)
			{
				interpStart = -Double2::UnitX;
				return Int2(voxelX + 1, voxelZ + 1);
			}
			else if (nearFacing == VoxelData::Facing::NegativeX)
			{
				interpStart = Double2::UnitX;
				return Int2(voxelX, voxelZ);
			}
			else if (nearFacing == VoxelData::Facing::PositiveZ)
			{
				interpStart = -Double2::UnitY;
				return Int2(voxelX, voxelZ + 1);
			}
			else if (nearFacing == VoxelData::Facing::NegativeZ)
			{
				interpStart = Double2::UnitY;
				return Int2(voxelX + 1, voxelZ);
			}
			else
			{
				DebugUnhandledReturnMsg(Int2, std::to_string(static_cast<int>(nearFacing)));
			}
		}();

		const Double2 cornerReal(
			static_cast<double>(corner.x),
			static_cast<double>(corner.y));

		// Bias the pivot towards the voxel center slightly to avoid Z-fighting with
		// adjacent walls.
		const Double2 voxelCenter(
			static_cast<double>(voxelX) + 0.50,
			static_cast<double>(voxelZ) + 0.50);
		const Double2 bias = (voxelCenter - cornerReal) * Constants::Epsilon;
		return cornerReal + bias;
	}();

	// Use the left perpendicular vector of the door's closed position as the 
	// fully open position.
	const Double2 interpEnd = interpStart.leftPerp();

	// Actual position of the door in its rotation, represented as a vector.
	const Double2 doorVec = interpStart.lerp(interpEnd, 1.0 - percentOpen).normalized();

	// Vector cross product in 2D, returns a scalar.
	auto cross = [](const Double2 &a, const Double2 &b)
	{
		return (a.x * b.y) - (b.x * a.y);
	};

	// Solve line segment intersection between the incoming ray and the door.
	const Double2 p1 = pivot;
	const Double2 v1 = doorVec;
	const Double2 p2 = nearPoint;
	const Double2 v2 = farPoint - nearPoint;

	// Percent from p1 to (p1 + v1).
	const double t = cross(p2 - p1, v2) / cross(v1, v2);

	// See if the two line segments intersect.
	if ((t >= 0.0) && (t < 1.0))
	{
		// Hit.
		hit.point = p1 + (v1 * t);
		hit.innerZ = (hit.point - nearPoint).length();
		hit.u = t;
		hit.normal = [&v1]()
		{
			const Double2 norm2D = v1.rightPerp();
			return Double3(norm2D.x, 0.0, norm2D.y);
		}();

		return true;
	}
	else
	{
		// No hit.
		return false;
	}
}

bool SoftwareRenderer::findDoorIntersection(int voxelX, int voxelZ, 
	VoxelData::DoorData::Type doorType, double percentOpen, VoxelData::Facing nearFacing,
	const Double2 &nearPoint, const Double2 &farPoint, double nearU, RayHit &hit)
{
	// Check trivial case first: whether the door is closed.
	const bool isClosed = percentOpen == 0.0;

	if (isClosed)
	{
		// Treat like a wall.
		hit.innerZ = 0.0;
		hit.u = nearU;
		hit.point = nearPoint;
		hit.normal = VoxelData::getNormal(nearFacing);
		return true;
	}
	else if (doorType == VoxelData::DoorData::Type::Swinging)
	{
		return SoftwareRenderer::findSwingingDoorIntersection(voxelX, voxelZ, percentOpen,
			nearFacing, nearPoint, farPoint, nearU, hit);
	}
	else if (doorType == VoxelData::DoorData::Type::Sliding)
	{
		// If near U coordinate is within percent closed, it's a hit. At 100% open,
		// a sliding door is still partially visible.
		const double minVisible = SoftwareRenderer::DOOR_MIN_VISIBLE;
		const double visibleAmount = 1.0 - ((1.0 - minVisible) * percentOpen);
		if (visibleAmount > nearU)
		{
			hit.innerZ = 0.0;
			hit.u = std::clamp(
				nearU + (1.0 - visibleAmount), 0.0, Constants::JustBelowOne);
			hit.point = nearPoint;
			hit.normal = VoxelData::getNormal(nearFacing);
			return true;
		}
		else
		{
			// No hit.
			return false;
		}
	}
	else if (doorType == VoxelData::DoorData::Type::Raising)
	{
		// Raising doors are always hit.
		hit.innerZ = 0.0;
		hit.u = nearU;
		hit.point = nearPoint;
		hit.normal = VoxelData::getNormal(nearFacing);
		return true;
	}
	else if (doorType == VoxelData::DoorData::Type::Splitting)
	{
		// If near U coordinate is within percent closed on left or right half, it's a hit.
		// At 100% open, a splitting door is still partially visible.
		const double minVisible = SoftwareRenderer::DOOR_MIN_VISIBLE;
		const bool leftHalf = nearU < 0.50;
		const bool rightHalf = nearU > 0.50;
		double leftVisAmount, rightVisAmount;
		const bool success = [percentOpen, nearU, minVisible, leftHalf, rightHalf,
			&leftVisAmount, &rightVisAmount]()
		{
			if (leftHalf)
			{
				// Left half.
				leftVisAmount = 0.50 - ((0.50 - minVisible) * percentOpen);
				return nearU <= leftVisAmount;
			}
			else if (rightHalf)
			{
				// Right half.
				rightVisAmount = 0.50 + ((0.50 - minVisible) * percentOpen);
				return nearU >= rightVisAmount;
			}
			else
			{
				// Midpoint (only when door is completely closed).
				return percentOpen == 0.0;
			}
		}();
		
		if (success)
		{
			// Hit.
			hit.innerZ = 0.0;
			hit.u = [nearU, leftHalf, rightHalf, leftVisAmount, rightVisAmount]()
			{
				const double u = [nearU, leftHalf, rightHalf, leftVisAmount, rightVisAmount]()
				{
					if (leftHalf)
					{
						return (nearU + 0.50) - leftVisAmount;
					}
					else if (rightHalf)
					{
						return (nearU + 0.50) - rightVisAmount;
					}
					else
					{
						// Midpoint.
						return 0.50;
					}
				}();
				
				return std::clamp(u, 0.0, Constants::JustBelowOne);
			}();

			hit.point = nearPoint;
			hit.normal = VoxelData::getNormal(nearFacing);

			return true;
		}
		else
		{
			// No hit.
			return false;
		}
	}
	else
	{
		// Invalid door type.
		return false;
	}
}

// @todo: might be better as a macro so there's no chance of a function call in the pixel loop.
template <int FilterMode, bool Transparency>
void SoftwareRenderer::sampleVoxelTexture(const VoxelTexture &texture, double u, double v,
	double *r, double *g, double *b, double *emission, bool *transparent)
{
	constexpr double textureWidthReal = static_cast<double>(VoxelTexture::WIDTH);
	constexpr double textureHeightReal = static_cast<double>(VoxelTexture::HEIGHT);

	if constexpr (FilterMode == 0)
	{
		// Nearest.
		const int textureX = static_cast<int>(u * textureWidthReal);
		const int textureY = static_cast<int>(v * textureHeightReal);
		const int textureIndex = textureX + (textureY * VoxelTexture::WIDTH);

		const VoxelTexel &texel = texture.texels[textureIndex];
		*r = texel.r;
		*g = texel.g;
		*b = texel.b;
		*emission = texel.emission;
		
		if constexpr (Transparency)
		{
			*transparent = texel.transparent;
		}
	}
	else if constexpr (FilterMode == 1)
	{
		// Linear.
		constexpr double texelWidth = 1.0 / textureWidthReal;
		constexpr double texelHeight = 1.0 / textureHeightReal;
		constexpr double halfTexelWidth = texelWidth / 2.0;
		constexpr double halfTexelHeight = texelHeight / 2.0;
		const double uL = std::max(u - halfTexelWidth, 0.0); // Change to wrapping for better texture edges
		const double uR = std::min(u + halfTexelWidth, Constants::JustBelowOne);
		const double vT = std::max(v - halfTexelHeight, 0.0);
		const double vB = std::min(v + halfTexelHeight, Constants::JustBelowOne);
		const double uLWidth = uL * textureWidthReal;
		const double vTHeight = vT * textureHeightReal;
		const double uLPercent = 1.0 - (uLWidth - std::floor(uLWidth));
		const double uRPercent = 1.0 - uLPercent;
		const double vTPercent = 1.0 - (vTHeight - std::floor(vTHeight));
		const double vBPercent = 1.0 - vTPercent;
		const double tlPercent = uLPercent * vTPercent;
		const double trPercent = uRPercent * vTPercent;
		const double blPercent = uLPercent * vBPercent;
		const double brPercent = uRPercent * vBPercent;
		const int textureXL = static_cast<int>(uL * textureWidthReal);
		const int textureXR = static_cast<int>(uR * textureWidthReal);
		const int textureYT = static_cast<int>(vT * textureHeightReal);
		const int textureYB = static_cast<int>(vB * textureHeightReal);
		const int textureIndexTL = textureXL + (textureYT * VoxelTexture::WIDTH);
		const int textureIndexTR = textureXR + (textureYT * VoxelTexture::WIDTH);
		const int textureIndexBL = textureXL + (textureYB * VoxelTexture::WIDTH);
		const int textureIndexBR = textureXR + (textureYB * VoxelTexture::WIDTH);

		const VoxelTexel &texelTL = texture.texels[textureIndexTL];
		const VoxelTexel &texelTR = texture.texels[textureIndexTR];
		const VoxelTexel &texelBL = texture.texels[textureIndexBL];
		const VoxelTexel &texelBR = texture.texels[textureIndexBR];
		*r = (texelTL.r * tlPercent) + (texelTR.r * trPercent) + (texelBL.r * blPercent) + (texelBR.r * brPercent);
		*g = (texelTL.g * tlPercent) + (texelTR.g * trPercent) + (texelBL.g * blPercent) + (texelBR.g * brPercent);
		*b = (texelTL.b * tlPercent) + (texelTR.b * trPercent) + (texelBL.b * blPercent) + (texelBR.b * brPercent);
		*emission = (texelTL.emission * tlPercent) + (texelTR.emission * trPercent) +
			(texelBL.emission * blPercent) + (texelBR.emission * brPercent);

		if constexpr (Transparency)
		{
			*transparent = texelTL.transparent && texelTR.transparent &&
				texelBL.transparent && texelBR.transparent;
		}
	}
	else
	{
		// Silently fail; don't want error reporting in a pixel shader.
		// (apparently can't static assert false here on GCC/Clang).
	}
}

template <bool Fading>
void SoftwareRenderer::drawPixelsShader(int x, const DrawRange &drawRange, double depth,
	double u, double vStart, double vEnd, const Double3 &normal, const VoxelTexture &texture,
	double fadePercent, const ShadingInfo &shadingInfo, OcclusionData &occlusion,
	const FrameView &frame)
{
	// Draw range values.
	const double yProjStart = drawRange.yProjStart;
	const double yProjEnd = drawRange.yProjEnd;
	int yStart = drawRange.yStart;
	int yEnd = drawRange.yEnd;

	// Horizontal offset in texture.
	// - Taken care of in texture sampling function (redundant calculation, though).
	//const int textureX = static_cast<int>(u * static_cast<double>(VoxelTexture::WIDTH));

	// Linearly interpolated fog.
	const Double3 &fogColor = shadingInfo.getFogColor();
	const double fogPercent = std::min(depth / shadingInfo.fogDistance, 1.0);

	// Contribution from the sun.
	const double lightNormalDot = std::max(0.0, shadingInfo.sunDirection.dot(normal));
	const Double3 sunComponent = (shadingInfo.sunColor * lightNormalDot).clamped(
		0.0, 1.0 - shadingInfo.ambient);

	// Shading on the texture.
	// - @todo: contribution from lights.
	const Double3 shading(
		shadingInfo.ambient + sunComponent.x,
		shadingInfo.ambient + sunComponent.y,
		shadingInfo.ambient + sunComponent.z);

	// Clip the Y start and end coordinates as needed, and refresh the occlusion buffer.
	occlusion.clipRange(&yStart, &yEnd);
	occlusion.update(yStart, yEnd);

	// Draw the column to the output buffer.
	for (int y = yStart; y < yEnd; y++)
	{
		const int index = x + (y * frame.width);

		// Check depth of the pixel before rendering.
		// - @todo: implement occlusion culling and back-to-front transparent rendering so
		//   this depth check isn't needed.
		if (depth <= (frame.depthBuffer[index] - Constants::Epsilon))
		{
			// Percent stepped from beginning to end on the column.
			const double yPercent =
				((static_cast<double>(y) + 0.50) - yProjStart) / (yProjEnd - yProjStart);

			// Vertical texture coordinate.
			const double v = vStart + ((vEnd - vStart) * yPercent);

			// Texture color. Alpha is ignored in this loop, so transparent texels will appear black.
			constexpr bool TextureTransparency = false;
			double colorR, colorG, colorB, colorEmission;
			SoftwareRenderer::sampleVoxelTexture<TextureFilterMode, TextureTransparency>(
				texture, u, v, &colorR, &colorG, &colorB, &colorEmission, nullptr);

			// Shading from light.
			constexpr double shadingMax = 1.0;
			colorR *= std::min(shading.x + colorEmission, shadingMax);
			colorG *= std::min(shading.y + colorEmission, shadingMax);
			colorB *= std::min(shading.z + colorEmission, shadingMax);

			if constexpr (Fading)
			{
				// Apply voxel fade percent.
				colorR *= fadePercent;
				colorG *= fadePercent;
				colorB *= fadePercent;
			}

			// Linearly interpolate with fog.
			colorR += (fogColor.x - colorR) * fogPercent;
			colorG += (fogColor.y - colorG) * fogPercent;
			colorB += (fogColor.z - colorB) * fogPercent;

			// Clamp maximum (don't worry about negative values).
			constexpr double high = 1.0;
			colorR = (colorR > high) ? high : colorR;
			colorG = (colorG > high) ? high : colorG;
			colorB = (colorB > high) ? high : colorB;

			// Convert floats to integers.
			const uint32_t colorRGB = static_cast<uint32_t>(
				((static_cast<uint8_t>(colorR * 255.0)) << 16) |
				((static_cast<uint8_t>(colorG * 255.0)) << 8) |
				((static_cast<uint8_t>(colorB * 255.0))));

			frame.colorBuffer[index] = colorRGB;
			frame.depthBuffer[index] = depth;
		}
	}
}

void SoftwareRenderer::drawPixels(int x, const DrawRange &drawRange, double depth, double u,
	double vStart, double vEnd, const Double3 &normal, const VoxelTexture &texture,
	double fadePercent, const ShadingInfo &shadingInfo, OcclusionData &occlusion,
	const FrameView &frame)
{
	if (fadePercent == 1.0)
	{
		constexpr bool fading = false;
		SoftwareRenderer::drawPixelsShader<fading>(x, drawRange, depth, u, vStart, vEnd, normal, texture,
			fadePercent, shadingInfo, occlusion, frame);
	}
	else
	{
		constexpr bool fading = true;
		SoftwareRenderer::drawPixelsShader<fading>(x, drawRange, depth, u, vStart, vEnd, normal, texture,
			fadePercent, shadingInfo, occlusion, frame);
	}
}

template <bool Fading>
void SoftwareRenderer::drawPerspectivePixelsShader(int x, const DrawRange &drawRange,
	const Double2 &startPoint, const Double2 &endPoint, double depthStart, double depthEnd,
	const Double3 &normal, const VoxelTexture &texture, double fadePercent,
	const ShadingInfo &shadingInfo, OcclusionData &occlusion, const FrameView &frame)
{
	// Draw range values.
	const double yProjStart = drawRange.yProjStart;
	const double yProjEnd = drawRange.yProjEnd;
	int yStart = drawRange.yStart;
	int yEnd = drawRange.yEnd;

	// Fog color to interpolate with.
	const Double3 &fogColor = shadingInfo.getFogColor();

	// Contribution from the sun.
	const double lightNormalDot = std::max(0.0, shadingInfo.sunDirection.dot(normal));
	const Double3 sunComponent = (shadingInfo.sunColor * lightNormalDot).clamped(
		0.0, 1.0 - shadingInfo.ambient);

	// Shading on the texture.
	// - @todo: contribution from lights.
	const Double3 shading(
		shadingInfo.ambient + sunComponent.x,
		shadingInfo.ambient + sunComponent.y,
		shadingInfo.ambient + sunComponent.z);

	// Values for perspective-correct interpolation.
	const double depthStartRecip = 1.0 / depthStart;
	const double depthEndRecip = 1.0 / depthEnd;
	const Double2 startPointDiv = startPoint * depthStartRecip;
	const Double2 endPointDiv = endPoint * depthEndRecip;
	const Double2 pointDivDiff = endPointDiv - startPointDiv;

	// Clip the Y start and end coordinates as needed, and refresh the occlusion buffer.
	occlusion.clipRange(&yStart, &yEnd);
	occlusion.update(yStart, yEnd);

	// Draw the column to the output buffer.
	for (int y = yStart; y < yEnd; y++)
	{
		const int index = x + (y * frame.width);

		// Percent stepped from beginning to end on the column.
		const double yPercent =
			((static_cast<double>(y) + 0.50) - yProjStart) / (yProjEnd - yProjStart);

		// Interpolate between the near and far depth.
		const double depth = 1.0 /
			(depthStartRecip + ((depthEndRecip - depthStartRecip) * yPercent));

		// Check depth of the pixel before rendering.
		// - @todo: implement occlusion culling and back-to-front transparent rendering so
		//   this depth check isn't needed.
		if (depth <= frame.depthBuffer[index])
		{
			// Linearly interpolated fog.
			const double fogPercent = std::min(depth / shadingInfo.fogDistance, 1.0);

			// Interpolate between start and end points.
			const double currentPointX = (startPointDiv.x + (pointDivDiff.x * yPercent)) * depth;
			const double currentPointY = (startPointDiv.y + (pointDivDiff.y * yPercent)) * depth;

			// Texture coordinates.
			const double u = std::clamp(
				Constants::JustBelowOne - (currentPointX - std::floor(currentPointX)),
				0.0, Constants::JustBelowOne);
			const double v = std::clamp(
				Constants::JustBelowOne - (currentPointY - std::floor(currentPointY)),
				0.0, Constants::JustBelowOne);

			// Texture color. Alpha is ignored in this loop, so transparent texels will appear black.
			constexpr bool TextureTransparency = false;
			double colorR, colorG, colorB, colorEmission;
			SoftwareRenderer::sampleVoxelTexture<TextureFilterMode, TextureTransparency>(
				texture, u, v, &colorR, &colorG, &colorB, &colorEmission, nullptr);

			// Shading from light.
			constexpr double shadingMax = 1.0;
			colorR *= std::min(shading.x + colorEmission, shadingMax);
			colorG *= std::min(shading.y + colorEmission, shadingMax);
			colorB *= std::min(shading.z + colorEmission, shadingMax);

			if constexpr (Fading)
			{
				// Apply voxel fade percent.
				colorR *= fadePercent;
				colorG *= fadePercent;
				colorB *= fadePercent;
			}

			// Linearly interpolate with fog.
			colorR += (fogColor.x - colorR) * fogPercent;
			colorG += (fogColor.y - colorG) * fogPercent;
			colorB += (fogColor.z - colorB) * fogPercent;

			// Clamp maximum (don't worry about negative values).
			const double high = 1.0;
			colorR = (colorR > high) ? high : colorR;
			colorG = (colorG > high) ? high : colorG;
			colorB = (colorB > high) ? high : colorB;

			// Convert floats to integers.
			const uint32_t colorRGB = static_cast<uint32_t>(
				((static_cast<uint8_t>(colorR * 255.0)) << 16) |
				((static_cast<uint8_t>(colorG * 255.0)) << 8) |
				((static_cast<uint8_t>(colorB * 255.0))));

			frame.colorBuffer[index] = colorRGB;
			frame.depthBuffer[index] = depth;
		}
	}
}

void SoftwareRenderer::drawPerspectivePixels(int x, const DrawRange &drawRange,
	const Double2 &startPoint, const Double2 &endPoint, double depthStart, double depthEnd,
	const Double3 &normal, const VoxelTexture &texture, double fadePercent,
	const ShadingInfo &shadingInfo, OcclusionData &occlusion, const FrameView &frame)
{
	if (fadePercent == 1.0)
	{
		constexpr bool fading = false;
		SoftwareRenderer::drawPerspectivePixelsShader<fading>(x, drawRange, startPoint, endPoint,
			depthStart, depthEnd, normal, texture, fadePercent, shadingInfo, occlusion, frame);
	}
	else
	{
		constexpr bool fading = true;
		SoftwareRenderer::drawPerspectivePixelsShader<fading>(x, drawRange, startPoint, endPoint,
			depthStart, depthEnd, normal, texture, fadePercent, shadingInfo, occlusion, frame);
	}
}

void SoftwareRenderer::drawTransparentPixels(int x, const DrawRange &drawRange, double depth,
	double u, double vStart, double vEnd, const Double3 &normal, const VoxelTexture &texture,
	const ShadingInfo &shadingInfo, const OcclusionData &occlusion, const FrameView &frame)
{
	// Draw range values.
	const double yProjStart = drawRange.yProjStart;
	const double yProjEnd = drawRange.yProjEnd;
	int yStart = drawRange.yStart;
	int yEnd = drawRange.yEnd;

	// Horizontal offset in texture.
	// - Taken care of in texture sampling function (redundant calculation, though).
	//const int textureX = static_cast<int>(u * static_cast<double>(VoxelTexture::WIDTH));

	// Linearly interpolated fog.
	const Double3 &fogColor = shadingInfo.getFogColor();
	const double fogPercent = std::min(depth / shadingInfo.fogDistance, 1.0);

	// Contribution from the sun.
	const double lightNormalDot = std::max(0.0, shadingInfo.sunDirection.dot(normal));
	const Double3 sunComponent = (shadingInfo.sunColor * lightNormalDot).clamped(
		0.0, 1.0 - shadingInfo.ambient);

	// Shading on the texture.
	// - @todo: contribution from lights.
	const Double3 shading(
		shadingInfo.ambient + sunComponent.x,
		shadingInfo.ambient + sunComponent.y,
		shadingInfo.ambient + sunComponent.z);

	// Clip the Y start and end coordinates as needed, but do not refresh the occlusion buffer,
	// because transparent ranges do not occlude as simply as opaque ranges.
	occlusion.clipRange(&yStart, &yEnd);

	// Draw the column to the output buffer.
	for (int y = yStart; y < yEnd; y++)
	{
		const int index = x + (y * frame.width);

		// Check depth of the pixel before rendering.
		if (depth <= (frame.depthBuffer[index] - Constants::Epsilon))
		{
			// Percent stepped from beginning to end on the column.
			const double yPercent =
				((static_cast<double>(y) + 0.50) - yProjStart) / (yProjEnd - yProjStart);

			// Vertical texture coordinate.
			const double v = vStart + ((vEnd - vStart) * yPercent);

			// Texture color. Alpha is checked in this loop, and transparent texels are not drawn.
			constexpr bool TextureTransparency = true;
			double colorR, colorG, colorB, colorEmission;
			bool colorTransparent;
			SoftwareRenderer::sampleVoxelTexture<TextureFilterMode, TextureTransparency>(
				texture, u, v, &colorR, &colorG, &colorB, &colorEmission, &colorTransparent);
			
			if (!colorTransparent)
			{
				// Shading from light.
				constexpr double shadingMax = 1.0;
				colorR *= std::min(shading.x + colorEmission, shadingMax);
				colorG *= std::min(shading.y + colorEmission, shadingMax);
				colorB *= std::min(shading.z + colorEmission, shadingMax);

				// Linearly interpolate with fog.
				colorR += (fogColor.x - colorR) * fogPercent;
				colorG += (fogColor.y - colorG) * fogPercent;
				colorB += (fogColor.z - colorB) * fogPercent;
				
				// Clamp maximum (don't worry about negative values).
				const double high = 1.0;
				colorR = (colorR > high) ? high : colorR;
				colorG = (colorG > high) ? high : colorG;
				colorB = (colorB > high) ? high : colorB;

				// Convert floats to integers.
				const uint32_t colorRGB = static_cast<uint32_t>(
					((static_cast<uint8_t>(colorR * 255.0)) << 16) |
					((static_cast<uint8_t>(colorG * 255.0)) << 8) |
					((static_cast<uint8_t>(colorB * 255.0))));

				frame.colorBuffer[index] = colorRGB;
				frame.depthBuffer[index] = depth;
			}
		}
	}
}

void SoftwareRenderer::drawDistantPixels(int x, const DrawRange &drawRange, double u,
	double vStart, double vEnd, const SkyTexture &texture, bool emissive,
	const ShadingInfo &shadingInfo, const FrameView &frame)
{
	// Draw range values.
	const double yProjStart = drawRange.yProjStart;
	const double yProjEnd = drawRange.yProjEnd;
	const int yStart = drawRange.yStart;
	const int yEnd = drawRange.yEnd;

	// Horizontal offset in texture.
	const int textureX = static_cast<int>(u * static_cast<double>(texture.width));
	
	// Shading on the texture. Some distant objects are completely bright.
	const double shading = emissive ? 1.0 : shadingInfo.distantAmbient;

	// Draw the column to the output buffer.
	for (int y = yStart; y < yEnd; y++)
	{
		const int index = x + (y * frame.width);

		// Percent stepped from beginning to end on the column.
		const double yPercent =
			((static_cast<double>(y) + 0.50) - yProjStart) / (yProjEnd - yProjStart);

		// Vertical texture coordinate.
		const double v = vStart + ((vEnd - vStart) * yPercent);

		// Y position in texture.
		const int textureY = static_cast<int>(v * static_cast<double>(texture.height));

		// Alpha is checked in this loop, and transparent texels are not drawn.
		const int textureIndex = textureX + (textureY * texture.width);
		const SkyTexel &texel = texture.texels[textureIndex];

		if (texel.a != 0.0)
		{
			// Special case (for true color): if texel alpha is between 0 and 1,
			// the previously rendered pixel is diminished by some amount. This is mostly
			// only pertinent to the edges of some clouds (with respect to distant sky).
			double colorR, colorG, colorB;
			if (texel.a < 1.0)
			{
				// Diminish the previous color in the frame buffer.
				const Double3 prevColor = Double3::fromRGB(frame.colorBuffer[index]);
				const double visPercent = std::clamp(1.0 - texel.a, 0.0, 1.0);
				colorR = prevColor.x * visPercent;
				colorG = prevColor.y * visPercent;
				colorB = prevColor.z * visPercent;
			}
			else
			{
				// Texture color with shading.
				colorR = texel.r * shading;
				colorG = texel.g * shading;
				colorB = texel.b * shading;
			}

			// Clamp maximum (don't worry about negative values).
			const double high = 1.0;
			colorR = (colorR > high) ? high : colorR;
			colorG = (colorG > high) ? high : colorG;
			colorB = (colorB > high) ? high : colorB;

			// Convert floats to integers.
			const uint32_t colorRGB = static_cast<uint32_t>(
				((static_cast<uint8_t>(colorR * 255.0)) << 16) |
				((static_cast<uint8_t>(colorG * 255.0)) << 8) |
				((static_cast<uint8_t>(colorB * 255.0))));

			frame.colorBuffer[index] = colorRGB;
		}
	}
}

void SoftwareRenderer::drawDistantPixelsSSE(int x, const DrawRange &drawRange, double u,
	double vStart, double vEnd, const SkyTexture &texture, bool emissive,
	const ShadingInfo &shadingInfo, const FrameView &frame)
{
	// @todo: fix. this is now out of sync with the non-SSE version.

	// Draw range values.
	const int yStart = drawRange.yStart;
	const int yEnd = drawRange.yEnd;
	const __m128d yProjStarts = _mm_set1_pd(drawRange.yProjStart);
	const __m128d yProjEnds = _mm_set1_pd(drawRange.yProjEnd);
	const __m128d yStarts = _mm_set1_pd(drawRange.yStart);
	const __m128d yEnds = _mm_set1_pd(drawRange.yEnd);

	// Horizontal offset in texture.
	const __m128i textureXs = [u, &texture]()
	{
		const __m128d us = _mm_set1_pd(std::min(u, Constants::JustBelowOne));
		const __m128d textureWidths = _mm_cvtepi32_pd(_mm_set1_epi32(texture.width));
		const __m128d mults = _mm_mul_pd(us, textureWidths);
		return _mm_cvttpd_epi32(mults);
	}();

	// Shading on the texture. Some distant objects are completely bright.
	const __m128d shadings = _mm_set1_pd(emissive ? 1.0 : shadingInfo.distantAmbient);

	// Some pre-calculated values.
	const __m128i zeroes = _mm_set1_epi32(0);
	const __m128d halfReals = _mm_set1_pd(0.50);
	const __m128d oneReals = _mm_set1_pd(1.0);
	const __m128d twoFiftyFiveReals = _mm_set1_pd(255.0);
	const __m128i xs = _mm_set1_epi32(x);
	const __m128i frameWidths = _mm_set1_epi32(frame.width);
	const __m128i frameHeights = _mm_set1_epi32(frame.height);
	const __m128d yProjDiffs = _mm_sub_pd(yProjStarts, yProjEnds);
	const __m128d vStarts = _mm_set1_pd(std::max(vStart, 0.0));
	const __m128d vEnds = _mm_set1_pd(std::min(vEnd, Constants::JustBelowOne));
	const __m128d vDiffs = _mm_sub_pd(vStarts, vEnds);
	const __m128i textureWidths = _mm_set1_epi32(texture.width);
	const __m128i textureHeights = _mm_set1_epi32(texture.height);
	const __m128d textureWidthReals = _mm_cvtepi32_pd(textureWidths);
	const __m128d textureHeightReals = _mm_cvtepi32_pd(textureHeights);

	// SIMD stride size.
	constexpr int stride = sizeof(__m128d) / sizeof(double);
	static_assert(stride == 2);

	// @todo: need special case loop afterwards to catch missed rows.
	for (int y = yStart; y < (yEnd - (stride - 1)); y += stride)
	{
		// Row and frame buffer index.
		const __m128i ys = _mm_setr_epi32(y, y + 1, 0, 0);
		const __m128i yRowOffsets = _mm_mullo_epi32(ys, frameWidths);

		// Percents stepped from beginning to end on the column.
		const __m128d yReals = _mm_cvtepi32_pd(ys);
		const __m128d yMidpoints = _mm_add_pd(yReals, halfReals);
		const __m128d yMidpointDiffs = _mm_sub_pd(yMidpoints, yProjStarts);
		const __m128d yPercents = _mm_div_pd(yMidpointDiffs, yProjDiffs);

		// Vertical texture coordinate.
		const __m128d vDiffYPercents = _mm_mul_pd(vDiffs, yPercents);
		const __m128d vs = _mm_add_pd(vStarts, vDiffYPercents);

		// Y position in texture.
		const __m128d vTextureHeights = _mm_mul_pd(vs, textureHeightReals);
		const __m128i textureYs = _mm_cvttpd_epi32(vTextureHeights);

		// Alpha is checked in this loop, and transparent texels are not drawn.
		const __m128i textureYWidths = _mm_mullo_epi32(textureYs, textureWidths);
		const __m128i textureIndices = _mm_add_epi32(textureXs, textureYWidths);

		// Load alpha component of texel to see if it's even visible.
		// @todo: proper SIMD texel format.
		// - For now, assume we can load texels really fast?! No gather instruction for SSE.
		// - This seems like a big bandwidth win once each color channel is in its own array.
		const SkyTexel &texel0 = texture.texels[_mm_extract_epi32(textureIndices, 0)];
		const SkyTexel &texel1 = texture.texels[_mm_extract_epi32(textureIndices, 1)];
		const __m128i texelAs = _mm_setr_epi32(
			static_cast<int>(texel0.a == 0.0),
			static_cast<int>(texel1.a == 0.0),
			static_cast<int>(false),
			static_cast<int>(false));

		// Check if the texel is opaque.
		const __m128i opaques = _mm_cmpeq_epi32(texelAs, zeroes);
		const bool opaque0 = _mm_extract_epi32(opaques, 0) != 0;
		const bool opaque1 = _mm_extract_epi32(opaques, 0) != 0;
		const bool anyOpaque = opaque0 || opaque1;
		if (anyOpaque)
		{
			// @todo: missing transparency branch of non-SSE version.
			// Texel colors.
			const __m128d texelRs = _mm_setr_pd(texel0.r, texel1.r);
			const __m128d texelGs = _mm_setr_pd(texel0.g, texel1.g);
			const __m128d texelBs = _mm_setr_pd(texel0.b, texel1.b);

			// Texture color with shading.
			__m128d colorRs = _mm_mul_pd(texelRs, shadings);
			__m128d colorGs = _mm_mul_pd(texelGs, shadings);
			__m128d colorBs = _mm_mul_pd(texelBs, shadings);

			// Clamp maximum (don't worry about negative values).
			const __m128d highs = oneReals;
			__m128d colorRCmps = _mm_cmpgt_pd(colorRs, highs);
			__m128d colorGCmps = _mm_cmpgt_pd(colorGs, highs);
			__m128d colorBCmps = _mm_cmpgt_pd(colorBs, highs);
			colorRs = _mm_blendv_pd(colorRs, highs, colorRCmps);
			colorGs = _mm_blendv_pd(colorGs, highs, colorGCmps);
			colorBs = _mm_blendv_pd(colorBs, highs, colorBCmps);

			// Convert floats to integers.
			const __m128d mul255s = twoFiftyFiveReals;
			const __m128d colorRs255 = _mm_mul_pd(colorRs, mul255s);
			const __m128d colorGs255 = _mm_mul_pd(colorGs, mul255s);
			const __m128d colorBs255 = _mm_mul_pd(colorBs, mul255s);
			const __m128i colorRsU32 = _mm_cvttpd_epi32(colorRs255);
			const __m128i colorGsU32 = _mm_cvttpd_epi32(colorGs255);
			const __m128i colorBsU32 = _mm_cvttpd_epi32(colorBs255);
			const __m128i colorRsShifted = _mm_slli_epi32(colorRsU32, 16);
			const __m128i colorGsShifted = _mm_slli_epi32(colorGsU32, 8);
			const __m128i colorBsShifted = colorBsU32;
			const __m128i colors = _mm_or_si128(
				_mm_or_si128(colorRsShifted, colorGsShifted), colorBsShifted);

			// Frame buffer index.
			const __m128i indices = _mm_add_epi32(xs, yRowOffsets);

			// (SIMD only) Conditionally write pixels based on texel opacity.
			if (opaque0)
			{
				const int index0 = _mm_extract_epi32(indices, 0);
				const uint32_t color0 = _mm_extract_epi32(colors, 0);
				frame.colorBuffer[index0] = color0;
			}

			if (opaque1)
			{
				const int index1 = _mm_extract_epi32(indices, 1);
				const uint32_t color1 = _mm_extract_epi32(colors, 1);
				frame.colorBuffer[index1] = color1;
			}
		}
	}
}

/*void SoftwareRenderer::drawDistantPixelsAVX(int x, const DrawRange &drawRange, double u,
	double vStart, double vEnd, const SkyTexture &texture, bool emissive,
	const ShadingInfo &shadingInfo, const FrameView &frame)
{
	// Draw range values.
	const int yStart = drawRange.yStart;
	const int yEnd = drawRange.yEnd;
	const __m256d yProjStarts = _mm256_set1_pd(drawRange.yProjStart);
	const __m256d yProjEnds = _mm256_set1_pd(drawRange.yProjEnd);
	const __m256d yStarts = _mm256_set1_pd(drawRange.yStart);
	const __m256d yEnds = _mm256_set1_pd(drawRange.yEnd);

	// Horizontal offset in texture.
	const __m128i textureXs = [u, &texture]()
	{
		const __m256d us = _mm256_set1_pd(std::min(u, Constants::JustBelowOne));
		const __m256d textureWidths = _mm256_cvtepi32_pd(_mm_set1_epi32(texture.width));
		const __m256d mults = _mm256_mul_pd(us, textureWidths);
		return _mm256_cvttpd_epi32(mults);
	}();

	// Shading on the texture. Some distant objects are completely bright.
	const __m256d shadings = _mm256_set1_pd(emissive ? 1.0 : shadingInfo.distantAmbient);

	// Some pre-calculated values.
	const __m128i zeroes = _mm_set1_epi32(0);
	const __m256d halfReals = _mm256_set1_pd(0.50);
	const __m256d oneReals = _mm256_set1_pd(1.0);
	const __m256d twoFiftyFiveReals = _mm256_set1_pd(255.0);
	const __m128i xs = _mm_set1_epi32(x);
	const __m128i frameWidths = _mm_set1_epi32(frame.width);
	const __m128i frameHeights = _mm_set1_epi32(frame.height);
	const __m256d yProjDiffs = _mm256_sub_pd(yProjStarts, yProjEnds);
	const __m256d vStarts = _mm256_set1_pd(std::max(vStart, 0.0));
	const __m256d vEnds = _mm256_set1_pd(std::min(vEnd, Constants::JustBelowOne));
	const __m256d vDiffs = _mm256_sub_pd(vStarts, vEnds);
	const __m128i textureWidths = _mm_set1_epi32(texture.width);
	const __m128i textureHeights = _mm_set1_epi32(texture.height);
	const __m256d textureWidthReals = _mm256_cvtepi32_pd(textureWidths);
	const __m256d textureHeightReals = _mm256_cvtepi32_pd(textureHeights);

	// SIMD stride size.
	constexpr int stride = sizeof(__m256d) / sizeof(double);
	static_assert(stride == 4);

	// @todo: need special case loop afterwards to catch missed rows.
	for (int y = yStart; y < (yEnd - (stride - 1)); y += stride)
	{
		// Row and frame buffer index.
		const __m128i ys = _mm_setr_epi32(y, y + 1, y + 2, y + 3);
		const __m128i yRowOffsets = _mm_mullo_epi32(ys, frameWidths);

		// Percents stepped from beginning to end on the column.
		const __m256d yReals = _mm256_cvtepi32_pd(ys);
		const __m256d yMidpoints = _mm256_add_pd(yReals, halfReals);
		const __m256d yMidpointDiffs = _mm256_sub_pd(yMidpoints, yProjStarts);
		const __m256d yPercents = _mm256_div_pd(yMidpointDiffs, yProjDiffs);

		// Vertical texture coordinate.
		const __m256d vDiffYPercents = _mm256_mul_pd(vDiffs, yPercents);
		const __m256d vs = _mm256_add_pd(vStarts, vDiffYPercents);

		// Y position in texture.
		const __m256d vTextureHeights = _mm256_mul_pd(vs, textureHeightReals);
		const __m128i textureYs = _mm256_cvttpd_epi32(vTextureHeights);

		// Alpha is checked in this loop, and transparent texels are not drawn.
		const __m128i textureYWidths = _mm_mullo_epi32(textureYs, textureWidths);
		const __m128i textureIndices = _mm_add_epi32(textureXs, textureYWidths);

		// Load alpha component of texel to see if it's even visible.
		// @todo: proper SIMD texel format.
		// - For now, assume we can load texels really fast?! No gather instruction for SSE.
		// - This seems like a big bandwidth win once each color channel is in its own array.
		const SkyTexel &texel0 = texture.texels[_mm_extract_epi32(textureIndices, 0)];
		const SkyTexel &texel1 = texture.texels[_mm_extract_epi32(textureIndices, 1)];
		const SkyTexel &texel2 = texture.texels[_mm_extract_epi32(textureIndices, 2)];
		const SkyTexel &texel3 = texture.texels[_mm_extract_epi32(textureIndices, 3)];
		const __m128i texelAs = _mm_setr_epi32(
			static_cast<int>(texel0.transparent),
			static_cast<int>(texel1.transparent),
			static_cast<int>(texel2.transparent),
			static_cast<int>(texel3.transparent));

		// Check if the texel is opaque.
		const __m128i opaques = _mm_cmpeq_epi32(texelAs, zeroes);
		const bool opaque0 = _mm_extract_epi32(opaques, 0) != 0;
		const bool opaque1 = _mm_extract_epi32(opaques, 1) != 0;
		const bool opaque2 = _mm_extract_epi32(opaques, 2) != 0;
		const bool opaque3 = _mm_extract_epi32(opaques, 3) != 0;
		const bool anyOpaque = opaque0 || opaque1 || opaque2 || opaque3;
		if (anyOpaque)
		{
			// Texel colors.
			const __m256d texelRs = _mm256_setr_pd(texel0.r, texel1.r, texel2.r, texel3.r);
			const __m256d texelGs = _mm256_setr_pd(texel0.g, texel1.g, texel2.g, texel3.g);
			const __m256d texelBs = _mm256_setr_pd(texel0.b, texel1.b, texel2.b, texel3.b);

			// Texture color with shading.
			__m256d colorRs = _mm256_mul_pd(texelRs, shadings);
			__m256d colorGs = _mm256_mul_pd(texelGs, shadings);
			__m256d colorBs = _mm256_mul_pd(texelBs, shadings);

			// Clamp maximum (don't worry about negative values).
			const __m256d highs = oneReals;
			__m256d colorRCmps = _mm256_cmp_pd(colorRs, highs, _CMP_GT_OS);
			__m256d colorGCmps = _mm256_cmp_pd(colorGs, highs, _CMP_GT_OS);
			__m256d colorBCmps = _mm256_cmp_pd(colorBs, highs, _CMP_GT_OS);
			colorRs = _mm256_blendv_pd(colorRs, highs, colorRCmps);
			colorGs = _mm256_blendv_pd(colorGs, highs, colorGCmps);
			colorBs = _mm256_blendv_pd(colorBs, highs, colorBCmps);

			// Convert floats to integers.
			const __m256d mul255s = twoFiftyFiveReals;
			const __m256d colorRs255 = _mm256_mul_pd(colorRs, mul255s);
			const __m256d colorGs255 = _mm256_mul_pd(colorGs, mul255s);
			const __m256d colorBs255 = _mm256_mul_pd(colorBs, mul255s);
			const __m128i colorRsU32 = _mm256_cvttpd_epi32(colorRs255);
			const __m128i colorGsU32 = _mm256_cvttpd_epi32(colorGs255);
			const __m128i colorBsU32 = _mm256_cvttpd_epi32(colorBs255);
			const __m128i colorRsShifted = _mm_slli_epi32(colorRsU32, 16);
			const __m128i colorGsShifted = _mm_slli_epi32(colorGsU32, 8);
			const __m128i colorBsShifted = colorBsU32;
			const __m128i colors = _mm_or_si128(
				_mm_or_si128(colorRsShifted, colorGsShifted), colorBsShifted);

			// Frame buffer index.
			const __m128i indices = _mm_add_epi32(xs, yRowOffsets);

			// (SIMD only) Conditionally write pixels based on texel opacity.
			if (opaque0)
			{
				const int index0 = _mm_extract_epi32(indices, 0);
				const uint32_t color0 = _mm_extract_epi32(colors, 0);
				frame.colorBuffer[index0] = color0;
			}

			if (opaque1)
			{
				const int index1 = _mm_extract_epi32(indices, 1);
				const uint32_t color1 = _mm_extract_epi32(colors, 1);
				frame.colorBuffer[index1] = color1;
			}

			if (opaque2)
			{
				const int index2 = _mm_extract_epi32(indices, 2);
				const uint32_t color2 = _mm_extract_epi32(colors, 2);
				frame.colorBuffer[index2] = color2;
			}

			if (opaque3)
			{
				const int index3 = _mm_extract_epi32(indices, 3);
				const uint32_t color3 = _mm_extract_epi32(colors, 3);
				frame.colorBuffer[index3] = color3;
			}
		}
	}
}*/

void SoftwareRenderer::drawMoonPixels(int x, const DrawRange &drawRange, double u, double vStart,
	double vEnd, const SkyTexture &texture, const ShadingInfo &shadingInfo, const FrameView &frame)
{
	// Draw range values.
	const double yProjStart = drawRange.yProjStart;
	const double yProjEnd = drawRange.yProjEnd;
	const int yStart = drawRange.yStart;
	const int yEnd = drawRange.yEnd;

	// Horizontal offset in texture.
	const int textureX = static_cast<int>(u * static_cast<double>(texture.width));

	// The gradient color is used for "unlit" texels on the moon's texture.
	constexpr double gradientPercent = 0.80;
	const Double3 gradientColor = SoftwareRenderer::getSkyGradientRowColor(
		gradientPercent, shadingInfo);

	// The 'signal' color used in the original game to denote moon texels that should
	// use the gradient color behind the moon instead.
	const Double3 unlitColor(170.0 / 255.0, 0.0, 0.0);

	// Draw the column to the output buffer.
	for (int y = yStart; y < yEnd; y++)
	{
		const int index = x + (y * frame.width);

		// Percent stepped from beginning to end on the column.
		const double yPercent =
			((static_cast<double>(y) + 0.50) - yProjStart) / (yProjEnd - yProjStart);

		// Vertical texture coordinate.
		const double v = vStart + ((vEnd - vStart) * yPercent);

		// Y position in texture.
		const int textureY = static_cast<int>(v * static_cast<double>(texture.height));

		// Alpha is checked in this loop, and transparent texels are not drawn.
		const int textureIndex = textureX + (textureY * texture.width);
		const SkyTexel &texel = texture.texels[textureIndex];

		if (texel.a != 0.0)
		{
			// Determine how the pixel should be shaded based on the moon texel. Should be
			// safe to do floating-point comparisons here with no error.
			const bool texelIsLit = (texel.r != unlitColor.x) && (texel.g != unlitColor.y) &&
				(texel.b != unlitColor.z);

			double colorR;
			double colorG;
			double colorB;

			if (texelIsLit)
			{
				// Use the moon texel.
				colorR = texel.r;
				colorG = texel.g;
				colorB = texel.b;
			}
			else
			{
				// Use the gradient color.
				colorR = gradientColor.x;
				colorG = gradientColor.y;
				colorB = gradientColor.z;
			}

			// Clamp maximum (don't worry about negative values).
			const double high = 1.0;
			colorR = (colorR > high) ? high : colorR;
			colorG = (colorG > high) ? high : colorG;
			colorB = (colorB > high) ? high : colorB;

			// Convert floats to integers.
			const uint32_t colorRGB = static_cast<uint32_t>(
				((static_cast<uint8_t>(colorR * 255.0)) << 16) |
				((static_cast<uint8_t>(colorG * 255.0)) << 8) |
				((static_cast<uint8_t>(colorB * 255.0))));

			frame.colorBuffer[index] = colorRGB;
		}
	}
}

void SoftwareRenderer::drawStarPixels(int x, const DrawRange &drawRange, double u, double vStart,
	double vEnd, const SkyTexture &texture, const std::vector<Double3> &skyGradientRowCache,
	const ShadingInfo &shadingInfo, const FrameView &frame)
{
	// Draw range values.
	const double yProjStart = drawRange.yProjStart;
	const double yProjEnd = drawRange.yProjEnd;
	const int yStart = drawRange.yStart;
	const int yEnd = drawRange.yEnd;

	// Horizontal offset in texture.
	const int textureX = static_cast<int>(u * static_cast<double>(texture.width));

	// Draw the column to the output buffer.
	for (int y = yStart; y < yEnd; y++)
	{
		const int index = x + (y * frame.width);

		// Percent stepped from beginning to end on the column.
		const double yPercent =
			((static_cast<double>(y) + 0.50) - yProjStart) / (yProjEnd - yProjStart);

		// Vertical texture coordinate.
		const double v = vStart + ((vEnd - vStart) * yPercent);

		// Y position in texture.
		const int textureY = static_cast<int>(v * static_cast<double>(texture.height));

		// Alpha is checked in this loop, and transparent texels are not drawn.
		const int textureIndex = textureX + (textureY * texture.width);
		const SkyTexel &texel = texture.texels[textureIndex];

		if (texel.a != 0.0)
		{
			// Get gradient color from sky gradient row cache.
			const Double3 &gradientColor = skyGradientRowCache[y];

			// If the gradient color behind the star is dark enough, then draw. Interpolate with a
			// range of intensities so stars don't immediately blink on/off when the gradient is a
			// certain color. Stars are generally small so I think it's okay to do more expensive
			// per-pixel operations here.
			constexpr double visThreshold = ShadingInfo::STAR_VIS_THRESHOLD; // Stars are becoming visible.
			constexpr double brightestThreshold = 32.0 / 255.0; // Stars are brightest.

			const double brightestComponent = std::max(
				std::max(gradientColor.x, gradientColor.y), gradientColor.z);
			const bool isDarkEnough = brightestComponent <= visThreshold;

			if (isDarkEnough)
			{
				const double gradientVisPercent = std::clamp(
					(brightestComponent - brightestThreshold) / (visThreshold - brightestThreshold),
					0.0, 1.0);

				// Texture color with shading.
				double colorR = texel.r;
				double colorG = texel.g;
				double colorB = texel.b;

				// Lerp with sky gradient for smoother transition between day and night.
				colorR += (gradientColor.x - colorR) * gradientVisPercent;
				colorG += (gradientColor.y - colorG) * gradientVisPercent;
				colorB += (gradientColor.z - colorB) * gradientVisPercent;

				// Clamp maximum (don't worry about negative values).
				const double high = 1.0;
				colorR = (colorR > high) ? high : colorR;
				colorG = (colorG > high) ? high : colorG;
				colorB = (colorB > high) ? high : colorB;

				// Convert floats to integers.
				const uint32_t colorRGB = static_cast<uint32_t>(
					((static_cast<uint8_t>(colorR * 255.0)) << 16) |
					((static_cast<uint8_t>(colorG * 255.0)) << 8) |
					((static_cast<uint8_t>(colorB * 255.0))));

				frame.colorBuffer[index] = colorRGB;
			}
		}
	}
}

void SoftwareRenderer::drawInitialVoxelColumn(int x, int voxelX, int voxelZ, const Camera &camera,
	const Ray &ray, VoxelData::Facing facing, const Double2 &nearPoint, const Double2 &farPoint,
	double nearZ, double farZ, const ShadingInfo &shadingInfo, double ceilingHeight,
	const std::vector<LevelData::DoorState> &openDoors,
	const std::vector<LevelData::FadeState> &fadingVoxels, const VoxelGrid &voxelGrid,
	const std::vector<VoxelTexture> &textures, OcclusionData &occlusion, const FrameView &frame)
{
	// This method handles some special cases such as drawing the back-faces of wall sides.

	// When clamping Y values for drawing ranges, subtract 0.5 from starts and add 0.5 to 
	// ends before converting to integers because the drawing methods sample at the center 
	// of pixels. The clamping function depends on which side of the range is being clamped; 
	// either way, the drawing range should be contained within the projected range at the 
	// sub-pixel level. This ensures that the vertical texture coordinate is always within 0->1.

	const double wallU = [&farPoint, facing]()
	{
		const double uVal = [&farPoint, facing]()
		{
			if (facing == VoxelData::Facing::PositiveX)
			{
				return farPoint.y - std::floor(farPoint.y);
			}
			else if (facing == VoxelData::Facing::NegativeX)
			{
				return Constants::JustBelowOne - (farPoint.y - std::floor(farPoint.y));
			}
			else if (facing == VoxelData::Facing::PositiveZ)
			{
				return Constants::JustBelowOne - (farPoint.x - std::floor(farPoint.x));
			}
			else
			{
				return farPoint.x - std::floor(farPoint.x);
			}
		}();

		return std::clamp(uVal, 0.0, Constants::JustBelowOne);
	}();

	// Normal of the wall for the incoming ray, potentially shared between multiple voxels in
	// this voxel column.
	const Double3 wallNormal = -VoxelData::getNormal(facing);

	auto drawInitialVoxel = [x, voxelX, voxelZ, &camera, &ray, &wallNormal, &nearPoint,
		&farPoint, nearZ, farZ, wallU, &shadingInfo, ceilingHeight, &openDoors, &fadingVoxels,
		&voxelGrid, &textures, &occlusion, &frame](int voxelY)
	{
		const uint16_t voxelID = voxelGrid.getVoxel(voxelX, voxelY, voxelZ);
		const VoxelData &voxelData = voxelGrid.getVoxelData(voxelID);
		const double voxelHeight = ceilingHeight;
		const double voxelYReal = static_cast<double>(voxelY) * voxelHeight;

		if (voxelData.dataType == VoxelDataType::Wall)
		{
			// Draw inner ceiling, wall, and floor.
			const VoxelData::WallData &wallData = voxelData.wall;

			const Double3 farCeilingPoint(
				farPoint.x,
				voxelYReal + voxelHeight,
				farPoint.y);
			const Double3 nearCeilingPoint(
				nearPoint.x,
				farCeilingPoint.y,
				nearPoint.y);
			const Double3 farFloorPoint(
				farPoint.x,
				voxelYReal,
				farPoint.y);
			const Double3 nearFloorPoint(
				nearPoint.x,
				farFloorPoint.y,
				nearPoint.y);

			const auto drawRanges = SoftwareRenderer::makeDrawRangeThreePart(
				nearCeilingPoint, farCeilingPoint, farFloorPoint, nearFloorPoint, camera, frame);
			const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
				voxelX, voxelY, voxelZ, fadingVoxels);

			// Ceiling.
			SoftwareRenderer::drawPerspectivePixels(x, drawRanges.at(0), nearPoint, farPoint,
				nearZ, farZ, -Double3::UnitY, textures.at(wallData.ceilingID), fadePercent,
				shadingInfo, occlusion, frame);

			// Wall.
			SoftwareRenderer::drawPixels(x, drawRanges.at(1), farZ, wallU, 0.0,
				Constants::JustBelowOne, wallNormal, textures.at(wallData.sideID), fadePercent,
				shadingInfo, occlusion, frame);

			// Floor.
			SoftwareRenderer::drawPerspectivePixels(x, drawRanges.at(2), farPoint, nearPoint,
				farZ, nearZ, Double3::UnitY, textures.at(wallData.floorID), fadePercent,
				shadingInfo, occlusion, frame);
		}
		else if (voxelData.dataType == VoxelDataType::Floor)
		{
			// Do nothing. Floors can only be seen from above.
		}
		else if (voxelData.dataType == VoxelDataType::Ceiling)
		{
			// Draw bottom of ceiling voxel if the camera is below it.
			if (camera.eye.y < voxelYReal)
			{
				const VoxelData::CeilingData &ceilingData = voxelData.ceiling;

				const Double3 nearFloorPoint(
					nearPoint.x,
					voxelYReal,
					nearPoint.y);
				const Double3 farFloorPoint(
					farPoint.x,
					nearFloorPoint.y,
					farPoint.y);

				const auto drawRange = SoftwareRenderer::makeDrawRange(
					nearFloorPoint, farFloorPoint, camera, frame);
				const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
					voxelX, voxelY, voxelZ, fadingVoxels);

				SoftwareRenderer::drawPerspectivePixels(x, drawRange, nearPoint, farPoint,
					nearZ, farZ, -Double3::UnitY, textures.at(ceilingData.id), fadePercent,
					shadingInfo, occlusion, frame);
			}
		}
		else if (voxelData.dataType == VoxelDataType::Raised)
		{
			const VoxelData::RaisedData &raisedData = voxelData.raised;

			const Double3 nearCeilingPoint(
				nearPoint.x,
				voxelYReal + ((raisedData.yOffset + raisedData.ySize) * voxelHeight),
				nearPoint.y);
			const Double3 nearFloorPoint(
				nearPoint.x,
				voxelYReal + (raisedData.yOffset * voxelHeight),
				nearPoint.y);

			// Draw order depends on the player's Y position relative to the platform.
			if (camera.eye.y > nearCeilingPoint.y)
			{
				// Above platform.
				const Double3 farCeilingPoint(
					farPoint.x,
					nearCeilingPoint.y,
					farPoint.y);

				const auto drawRange = SoftwareRenderer::makeDrawRange(
					farCeilingPoint, nearCeilingPoint, camera, frame);
				const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
					voxelX, voxelY, voxelZ, fadingVoxels);

				// Ceiling.
				SoftwareRenderer::drawPerspectivePixels(x, drawRange, farPoint, nearPoint, farZ,
					nearZ, Double3::UnitY, textures.at(raisedData.ceilingID), fadePercent,
					shadingInfo, occlusion, frame);
			}
			else if (camera.eye.y < nearFloorPoint.y)
			{
				// Below platform.
				const Double3 farFloorPoint(
					farPoint.x,
					nearFloorPoint.y,
					farPoint.y);

				const auto drawRange = SoftwareRenderer::makeDrawRange(
					nearFloorPoint, farFloorPoint, camera, frame);
				const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
					voxelX, voxelY, voxelZ, fadingVoxels);

				// Floor.
				SoftwareRenderer::drawPerspectivePixels(x, drawRange, nearPoint, farPoint, nearZ,
					farZ, -Double3::UnitY, textures.at(raisedData.floorID), fadePercent,
					shadingInfo, occlusion, frame);
			}
			else
			{
				// Between top and bottom.
				const Double3 farCeilingPoint(
					farPoint.x,
					nearCeilingPoint.y,
					farPoint.y);
				const Double3 farFloorPoint(
					farPoint.x,
					nearFloorPoint.y,
					farPoint.y);

				const auto drawRanges = SoftwareRenderer::makeDrawRangeThreePart(
					nearCeilingPoint, farCeilingPoint, farFloorPoint, nearFloorPoint,
					camera, frame);
				const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
					voxelX, voxelY, voxelZ, fadingVoxels);

				// Ceiling.
				SoftwareRenderer::drawPerspectivePixels(x, drawRanges.at(0), nearPoint, farPoint,
					nearZ, farZ, -Double3::UnitY, textures.at(raisedData.ceilingID), fadePercent,
					shadingInfo, occlusion, frame);

				// Wall.
				SoftwareRenderer::drawTransparentPixels(x, drawRanges.at(1), farZ, wallU,
					raisedData.vTop, raisedData.vBottom, wallNormal,
					textures.at(raisedData.sideID), shadingInfo, occlusion, frame);

				// Floor.
				SoftwareRenderer::drawPerspectivePixels(x, drawRanges.at(2), farPoint, nearPoint,
					farZ, nearZ, Double3::UnitY, textures.at(raisedData.floorID), fadePercent,
					shadingInfo, occlusion, frame);
			}
		}
		else if (voxelData.dataType == VoxelDataType::Diagonal)
		{
			const VoxelData::DiagonalData &diagData = voxelData.diagonal;

			// Find intersection.
			RayHit hit;
			const bool success = diagData.type1 ?
				SoftwareRenderer::findDiag1Intersection(voxelX, voxelZ, nearPoint, farPoint, hit) :
				SoftwareRenderer::findDiag2Intersection(voxelX, voxelZ, nearPoint, farPoint, hit);

			if (success)
			{
				const Double3 diagTopPoint(
					hit.point.x,
					voxelYReal + voxelHeight,
					hit.point.y);
				const Double3 diagBottomPoint(
					diagTopPoint.x,
					voxelYReal,
					diagTopPoint.z);

				const auto drawRange = SoftwareRenderer::makeDrawRange(
					diagTopPoint, diagBottomPoint, camera, frame);
				const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
					voxelX, voxelY, voxelZ, fadingVoxels);

				SoftwareRenderer::drawPixels(x, drawRange, nearZ + hit.innerZ, hit.u, 0.0,
					Constants::JustBelowOne, hit.normal, textures.at(diagData.id), fadePercent,
					shadingInfo, occlusion, frame);
			}
		}
		else if (voxelData.dataType == VoxelDataType::TransparentWall)
		{
			// Do nothing. Transparent walls have no back-faces.
		}
		else if (voxelData.dataType == VoxelDataType::Edge)
		{
			const VoxelData::EdgeData &edgeData = voxelData.edge;

			// Find intersection.
			RayHit hit;
			const bool success = SoftwareRenderer::findInitialEdgeIntersection(
				voxelX, voxelZ, edgeData.facing, edgeData.flipped, nearPoint, farPoint,
				camera, ray, hit);

			if (success)
			{
				const Double3 edgeTopPoint(
					hit.point.x,
					voxelYReal + voxelHeight + edgeData.yOffset,
					hit.point.y);
				const Double3 edgeBottomPoint(
					edgeTopPoint.x,
					voxelYReal + edgeData.yOffset,
					edgeTopPoint.z);

				const auto drawRange = SoftwareRenderer::makeDrawRange(
					edgeTopPoint, edgeBottomPoint, camera, frame);

				SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ + hit.innerZ, hit.u,
					0.0, Constants::JustBelowOne, hit.normal, textures.at(edgeData.id),
					shadingInfo, occlusion, frame);
			}
		}
		else if (voxelData.dataType == VoxelDataType::Chasm)
		{
			// Render back-face.
			const VoxelData::ChasmData &chasmData = voxelData.chasm;

			// Find which far face on the chasm was intersected.
			const VoxelData::Facing farFacing = SoftwareRenderer::getInitialChasmFarFacing(
				voxelX, voxelZ, Double2(camera.eye.x, camera.eye.z), ray);

			// Far.
			if (chasmData.faceIsVisible(farFacing))
			{
				const double farU = [&farPoint, farFacing]()
				{
					const double uVal = [&farPoint, farFacing]()
					{
						if (farFacing == VoxelData::Facing::PositiveX)
						{
							return farPoint.y - std::floor(farPoint.y);
						}
						else if (farFacing == VoxelData::Facing::NegativeX)
						{
							return Constants::JustBelowOne - (farPoint.y - std::floor(farPoint.y));
						}
						else if (farFacing == VoxelData::Facing::PositiveZ)
						{
							return Constants::JustBelowOne - (farPoint.x - std::floor(farPoint.x));
						}
						else
						{
							return farPoint.x - std::floor(farPoint.x);
						}
					}();

					return std::clamp(uVal, 0.0, Constants::JustBelowOne);
				}();

				const Double3 farNormal = -VoxelData::getNormal(farFacing);

				// Wet chasms and lava chasms are unaffected by ceiling height.
				const double chasmDepth = (chasmData.type == VoxelData::ChasmData::Type::Dry) ?
					voxelHeight : VoxelData::ChasmData::WET_LAVA_DEPTH;

				const Double3 farCeilingPoint(
					farPoint.x,
					voxelYReal + voxelHeight,
					farPoint.y);
				const Double3 farFloorPoint(
					farPoint.x,
					farCeilingPoint.y - chasmDepth,
					farPoint.y);

				const auto drawRange = SoftwareRenderer::makeDrawRange(
					farCeilingPoint, farFloorPoint, camera, frame);

				SoftwareRenderer::drawTransparentPixels(x, drawRange, farZ, farU, 0.0,
					Constants::JustBelowOne, farNormal, textures.at(chasmData.id), shadingInfo,
					occlusion, frame);
			}
		}
		else if (voxelData.dataType == VoxelDataType::Door)
		{
			const VoxelData::DoorData &doorData = voxelData.door;
			const double percentOpen = SoftwareRenderer::getDoorPercentOpen(
				voxelX, voxelZ, openDoors);

			RayHit hit;
			const bool success = SoftwareRenderer::findInitialDoorIntersection(voxelX, voxelZ,
				doorData.type, percentOpen, nearPoint, farPoint, camera, ray, voxelGrid, hit);

			if (success)
			{
				if (doorData.type == VoxelData::DoorData::Type::Swinging)
				{
					const Double3 doorTopPoint(
						hit.point.x,
						voxelYReal + voxelHeight,
						hit.point.y);
					const Double3 doorBottomPoint(
						doorTopPoint.x,
						voxelYReal,
						doorTopPoint.z);

					const auto drawRange = SoftwareRenderer::makeDrawRange(
						doorTopPoint, doorBottomPoint, camera, frame);

					SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ + hit.innerZ,
						hit.u, 0.0, Constants::JustBelowOne, hit.normal, textures.at(doorData.id),
						shadingInfo, occlusion, frame);
				}
				else if (doorData.type == VoxelData::DoorData::Type::Sliding)
				{
					const Double3 doorTopPoint(
						hit.point.x,
						voxelYReal + voxelHeight,
						hit.point.y);
					const Double3 doorBottomPoint(
						doorTopPoint.x,
						voxelYReal,
						doorTopPoint.z);

					const auto drawRange = SoftwareRenderer::makeDrawRange(
						doorTopPoint, doorBottomPoint, camera, frame);

					SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ + hit.innerZ,
						hit.u, 0.0, Constants::JustBelowOne, hit.normal, textures.at(doorData.id),
						shadingInfo, occlusion, frame);
				}
				else if (doorData.type == VoxelData::DoorData::Type::Raising)
				{
					// Top point is fixed, bottom point depends on percent open.
					const double minVisible = SoftwareRenderer::DOOR_MIN_VISIBLE;
					const double raisedAmount = (voxelHeight * (1.0 - minVisible)) * percentOpen;

					const Double3 doorTopPoint(
						hit.point.x,
						voxelYReal + voxelHeight,
						hit.point.y);
					const Double3 doorBottomPoint(
						doorTopPoint.x,
						voxelYReal + raisedAmount,
						doorTopPoint.z);

					const auto drawRange = SoftwareRenderer::makeDrawRange(
						doorTopPoint, doorBottomPoint, camera, frame);

					// The start of the vertical texture coordinate depends on the percent open.
					const double vStart = raisedAmount / voxelHeight;

					SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ + hit.innerZ,
						hit.u, vStart, Constants::JustBelowOne, hit.normal,
						textures.at(doorData.id), shadingInfo, occlusion, frame);
				}
				else if (doorData.type == VoxelData::DoorData::Type::Splitting)
				{
					const Double3 doorTopPoint(
						hit.point.x,
						voxelYReal + voxelHeight,
						hit.point.y);
					const Double3 doorBottomPoint(
						doorTopPoint.x,
						voxelYReal,
						doorTopPoint.z);

					const auto drawRange = SoftwareRenderer::makeDrawRange(
						doorTopPoint, doorBottomPoint, camera, frame);

					SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ + hit.innerZ,
						hit.u, 0.0, Constants::JustBelowOne, hit.normal, textures.at(doorData.id),
						shadingInfo, occlusion, frame);
				}
			}
		}
	};

	auto drawInitialVoxelBelow = [x, voxelX, voxelZ, &camera, &ray, &wallNormal, &nearPoint,
		&farPoint, nearZ, farZ, wallU, &shadingInfo, ceilingHeight, &openDoors, &fadingVoxels,
		&voxelGrid, &textures, &occlusion, &frame](int voxelY)
	{
		const uint16_t voxelID = voxelGrid.getVoxel(voxelX, voxelY, voxelZ);
		const VoxelData &voxelData = voxelGrid.getVoxelData(voxelID);
		const double voxelHeight = ceilingHeight;
		const double voxelYReal = static_cast<double>(voxelY) * voxelHeight;

		if (voxelData.dataType == VoxelDataType::Wall)
		{
			const VoxelData::WallData &wallData = voxelData.wall;

			const Double3 farCeilingPoint(
				farPoint.x,
				voxelYReal + voxelHeight,
				farPoint.y);
			const Double3 nearCeilingPoint(
				nearPoint.x,
				farCeilingPoint.y,
				nearPoint.y);

			const auto drawRange = SoftwareRenderer::makeDrawRange(
				farCeilingPoint, nearCeilingPoint, camera, frame);
			const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
				voxelX, voxelY, voxelZ, fadingVoxels);

			// Ceiling.
			SoftwareRenderer::drawPerspectivePixels(x, drawRange, farPoint, nearPoint, farZ,
				nearZ, Double3::UnitY, textures.at(wallData.ceilingID), fadePercent,
				shadingInfo, occlusion, frame);
		}
		else if (voxelData.dataType == VoxelDataType::Floor)
		{
			// Draw top of floor voxel.
			const VoxelData::FloorData &floorData = voxelData.floor;

			const Double3 farCeilingPoint(
				farPoint.x,
				voxelYReal + voxelHeight,
				farPoint.y);
			const Double3 nearCeilingPoint(
				nearPoint.x,
				farCeilingPoint.y,
				nearPoint.y);

			const auto drawRange = SoftwareRenderer::makeDrawRange(
				farCeilingPoint, nearCeilingPoint, camera, frame);
			const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
				voxelX, voxelY, voxelZ, fadingVoxels);

			// Ceiling.
			SoftwareRenderer::drawPerspectivePixels(x, drawRange, farPoint, nearPoint, farZ,
				nearZ, Double3::UnitY, textures.at(floorData.id), fadePercent, shadingInfo,
				occlusion, frame);
		}
		else if (voxelData.dataType == VoxelDataType::Ceiling)
		{
			// Do nothing. Ceilings can only be seen from below.
		}
		else if (voxelData.dataType == VoxelDataType::Raised)
		{
			const VoxelData::RaisedData &raisedData = voxelData.raised;

			const Double3 nearCeilingPoint(
				nearPoint.x,
				voxelYReal + ((raisedData.yOffset + raisedData.ySize) * voxelHeight),
				nearPoint.y);
			const Double3 nearFloorPoint(
				nearPoint.x,
				voxelYReal + (raisedData.yOffset * voxelHeight),
				nearPoint.y);

			// Draw order depends on the player's Y position relative to the platform.
			if (camera.eye.y > nearCeilingPoint.y)
			{
				// Above platform.
				const Double3 farCeilingPoint(
					farPoint.x,
					nearCeilingPoint.y,
					farPoint.y);

				const auto drawRange = SoftwareRenderer::makeDrawRange(
					farCeilingPoint, nearCeilingPoint, camera, frame);
				const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
					voxelX, voxelY, voxelZ, fadingVoxels);

				// Ceiling.
				SoftwareRenderer::drawPerspectivePixels(x, drawRange, farPoint, nearPoint, farZ,
					nearZ, Double3::UnitY, textures.at(raisedData.ceilingID), fadePercent,
					shadingInfo, occlusion, frame);
			}
			else if (camera.eye.y < nearFloorPoint.y)
			{
				// Below platform.
				const Double3 farFloorPoint(
					farPoint.x,
					nearFloorPoint.y,
					farPoint.y);

				const auto drawRange = SoftwareRenderer::makeDrawRange(
					nearFloorPoint, farFloorPoint, camera, frame);
				const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
					voxelX, voxelY, voxelZ, fadingVoxels);

				// Floor.
				SoftwareRenderer::drawPerspectivePixels(x, drawRange, nearPoint, farPoint, nearZ,
					farZ, -Double3::UnitY, textures.at(raisedData.floorID), fadePercent,
					shadingInfo, occlusion, frame);
			}
			else
			{
				// Between top and bottom.
				const Double3 farCeilingPoint(
					farPoint.x,
					nearCeilingPoint.y,
					farPoint.y);
				const Double3 farFloorPoint(
					farPoint.x,
					nearFloorPoint.y,
					farPoint.y);

				const auto drawRanges = SoftwareRenderer::makeDrawRangeThreePart(
					nearCeilingPoint, farCeilingPoint, farFloorPoint, nearFloorPoint,
					camera, frame);
				const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
					voxelX, voxelY, voxelZ, fadingVoxels);

				// Ceiling.
				SoftwareRenderer::drawPerspectivePixels(x, drawRanges.at(0), nearPoint, farPoint,
					nearZ, farZ, -Double3::UnitY, textures.at(raisedData.ceilingID), fadePercent,
					shadingInfo, occlusion, frame);

				// Wall.
				SoftwareRenderer::drawTransparentPixels(x, drawRanges.at(1), farZ, wallU,
					raisedData.vTop, raisedData.vBottom, wallNormal,
					textures.at(raisedData.sideID), shadingInfo, occlusion, frame);

				// Floor.
				SoftwareRenderer::drawPerspectivePixels(x, drawRanges.at(2), farPoint, nearPoint,
					farZ, nearZ, Double3::UnitY, textures.at(raisedData.floorID), fadePercent,
					shadingInfo, occlusion, frame);
			}
		}
		else if (voxelData.dataType == VoxelDataType::Diagonal)
		{
			const VoxelData::DiagonalData &diagData = voxelData.diagonal;

			// Find intersection.
			RayHit hit;
			const bool success = diagData.type1 ?
				SoftwareRenderer::findDiag1Intersection(voxelX, voxelZ, nearPoint, farPoint, hit) :
				SoftwareRenderer::findDiag2Intersection(voxelX, voxelZ, nearPoint, farPoint, hit);

			if (success)
			{
				const Double3 diagTopPoint(
					hit.point.x,
					voxelYReal + voxelHeight,
					hit.point.y);
				const Double3 diagBottomPoint(
					diagTopPoint.x,
					voxelYReal,
					diagTopPoint.z);

				const auto drawRange = SoftwareRenderer::makeDrawRange(
					diagTopPoint, diagBottomPoint, camera, frame);
				const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
					voxelX, voxelY, voxelZ, fadingVoxels);

				SoftwareRenderer::drawPixels(x, drawRange, nearZ + hit.innerZ, hit.u, 0.0,
					Constants::JustBelowOne, hit.normal, textures.at(diagData.id), fadePercent,
					shadingInfo, occlusion, frame);
			}
		}
		else if (voxelData.dataType == VoxelDataType::TransparentWall)
		{
			// Do nothing. Transparent walls have no back-faces.
		}
		else if (voxelData.dataType == VoxelDataType::Edge)
		{
			const VoxelData::EdgeData &edgeData = voxelData.edge;

			// Find intersection.
			RayHit hit;
			const bool success = SoftwareRenderer::findInitialEdgeIntersection(
				voxelX, voxelZ, edgeData.facing, edgeData.flipped, nearPoint, farPoint,
				camera, ray, hit);

			if (success)
			{
				const Double3 edgeTopPoint(
					hit.point.x,
					voxelYReal + voxelHeight + edgeData.yOffset,
					hit.point.y);
				const Double3 edgeBottomPoint(
					hit.point.x,
					voxelYReal + edgeData.yOffset,
					hit.point.y);

				const auto drawRange = SoftwareRenderer::makeDrawRange(
					edgeTopPoint, edgeBottomPoint, camera, frame);

				SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ + hit.innerZ, hit.u,
					0.0, Constants::JustBelowOne, hit.normal, textures.at(edgeData.id),
					shadingInfo, occlusion, frame);
			}
		}
		else if (voxelData.dataType == VoxelDataType::Chasm)
		{
			// Render back-face.
			const VoxelData::ChasmData &chasmData = voxelData.chasm;

			// Find which far face on the chasm was intersected.
			const VoxelData::Facing farFacing = SoftwareRenderer::getInitialChasmFarFacing(
				voxelX, voxelZ, Double2(camera.eye.x, camera.eye.z), ray);

			// Far.
			if (chasmData.faceIsVisible(farFacing))
			{
				const double farU = [&farPoint, farFacing]()
				{
					const double uVal = [&farPoint, farFacing]()
					{
						if (farFacing == VoxelData::Facing::PositiveX)
						{
							return farPoint.y - std::floor(farPoint.y);
						}
						else if (farFacing == VoxelData::Facing::NegativeX)
						{
							return Constants::JustBelowOne - (farPoint.y - std::floor(farPoint.y));
						}
						else if (farFacing == VoxelData::Facing::PositiveZ)
						{
							return Constants::JustBelowOne - (farPoint.x - std::floor(farPoint.x));
						}
						else
						{
							return farPoint.x - std::floor(farPoint.x);
						}
					}();

					return std::clamp(uVal, 0.0, Constants::JustBelowOne);
				}();

				const Double3 farNormal = -VoxelData::getNormal(farFacing);

				// Wet chasms and lava chasms are unaffected by ceiling height.
				const double chasmDepth = (chasmData.type == VoxelData::ChasmData::Type::Dry) ?
					voxelHeight : VoxelData::ChasmData::WET_LAVA_DEPTH;

				const Double3 farCeilingPoint(
					farPoint.x,
					voxelYReal + voxelHeight,
					farPoint.y);
				const Double3 farFloorPoint(
					farPoint.x,
					farCeilingPoint.y - chasmDepth,
					farPoint.y);

				const auto drawRange = SoftwareRenderer::makeDrawRange(
					farCeilingPoint, farFloorPoint, camera, frame);

				SoftwareRenderer::drawTransparentPixels(x, drawRange, farZ, farU, 0.0,
					Constants::JustBelowOne, farNormal, textures.at(chasmData.id), shadingInfo,
					occlusion, frame);
			}
		}
		else if (voxelData.dataType == VoxelDataType::Door)
		{
			const VoxelData::DoorData &doorData = voxelData.door;
			const double percentOpen = SoftwareRenderer::getDoorPercentOpen(
				voxelX, voxelZ, openDoors);

			RayHit hit;
			const bool success = SoftwareRenderer::findInitialDoorIntersection(voxelX, voxelZ,
				doorData.type, percentOpen, nearPoint, farPoint, camera, ray, voxelGrid, hit);

			if (success)
			{
				if (doorData.type == VoxelData::DoorData::Type::Swinging)
				{
					const Double3 doorTopPoint(
						hit.point.x,
						voxelYReal + voxelHeight,
						hit.point.y);
					const Double3 doorBottomPoint(
						doorTopPoint.x,
						voxelYReal,
						doorTopPoint.z);

					const auto drawRange = SoftwareRenderer::makeDrawRange(
						doorTopPoint, doorBottomPoint, camera, frame);

					SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ + hit.innerZ,
						hit.u, 0.0, Constants::JustBelowOne, hit.normal, textures.at(doorData.id),
						shadingInfo, occlusion, frame);
				}
				else if (doorData.type == VoxelData::DoorData::Type::Sliding)
				{
					const Double3 doorTopPoint(
						hit.point.x,
						voxelYReal + voxelHeight,
						hit.point.y);
					const Double3 doorBottomPoint(
						doorTopPoint.x,
						voxelYReal,
						doorTopPoint.z);

					const auto drawRange = SoftwareRenderer::makeDrawRange(
						doorTopPoint, doorBottomPoint, camera, frame);

					SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ + hit.innerZ,
						hit.u, 0.0, Constants::JustBelowOne, hit.normal, textures.at(doorData.id),
						shadingInfo, occlusion, frame);
				}
				else if (doorData.type == VoxelData::DoorData::Type::Raising)
				{
					// Top point is fixed, bottom point depends on percent open.
					const double minVisible = SoftwareRenderer::DOOR_MIN_VISIBLE;
					const double raisedAmount = (voxelHeight * (1.0 - minVisible)) * percentOpen;

					const Double3 doorTopPoint(
						hit.point.x,
						voxelYReal + voxelHeight,
						hit.point.y);
					const Double3 doorBottomPoint(
						doorTopPoint.x,
						voxelYReal + raisedAmount,
						doorTopPoint.z);

					const auto drawRange = SoftwareRenderer::makeDrawRange(
						doorTopPoint, doorBottomPoint, camera, frame);

					// The start of the vertical texture coordinate depends on the percent open.
					const double vStart = raisedAmount / voxelHeight;

					SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ + hit.innerZ,
						hit.u, vStart, Constants::JustBelowOne, hit.normal,
						textures.at(doorData.id), shadingInfo, occlusion, frame);
				}
				else if (doorData.type == VoxelData::DoorData::Type::Splitting)
				{
					const Double3 doorTopPoint(
						hit.point.x,
						voxelYReal + voxelHeight,
						hit.point.y);
					const Double3 doorBottomPoint(
						doorTopPoint.x,
						voxelYReal,
						doorTopPoint.z);

					const auto drawRange = SoftwareRenderer::makeDrawRange(
						doorTopPoint, doorBottomPoint, camera, frame);

					SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ + hit.innerZ,
						hit.u, 0.0, Constants::JustBelowOne, hit.normal, textures.at(doorData.id),
						shadingInfo, occlusion, frame);
				}
			}
		}
	};

	auto drawInitialVoxelAbove = [x, voxelX, voxelZ, &camera, &ray, &wallNormal, &nearPoint,
		&farPoint, nearZ, farZ, wallU, &shadingInfo, ceilingHeight, &openDoors, &fadingVoxels,
		&voxelGrid, &textures, &occlusion, &frame](int voxelY)
	{
		const uint16_t voxelID = voxelGrid.getVoxel(voxelX, voxelY, voxelZ);
		const VoxelData &voxelData = voxelGrid.getVoxelData(voxelID);
		const double voxelHeight = ceilingHeight;
		const double voxelYReal = static_cast<double>(voxelY) * voxelHeight;

		if (voxelData.dataType == VoxelDataType::Wall)
		{
			const VoxelData::WallData &wallData = voxelData.wall;

			const Double3 nearFloorPoint(
				nearPoint.x,
				voxelYReal,
				nearPoint.y);
			const Double3 farFloorPoint(
				farPoint.x,
				nearFloorPoint.y,
				farPoint.y);

			const auto drawRange = SoftwareRenderer::makeDrawRange(
				nearFloorPoint, farFloorPoint, camera, frame);
			const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
				voxelX, voxelY, voxelZ, fadingVoxels);

			// Floor.
			SoftwareRenderer::drawPerspectivePixels(x, drawRange, nearPoint, farPoint, nearZ,
				farZ, -Double3::UnitY, textures.at(wallData.floorID), fadePercent,
				shadingInfo, occlusion, frame);
		}
		else if (voxelData.dataType == VoxelDataType::Floor)
		{
			// Do nothing. Floors can only be seen from above.
		}
		else if (voxelData.dataType == VoxelDataType::Ceiling)
		{
			// Draw bottom of ceiling voxel.
			const VoxelData::CeilingData &ceilingData = voxelData.ceiling;

			const Double3 nearFloorPoint(
				nearPoint.x,
				voxelYReal,
				nearPoint.y);
			const Double3 farFloorPoint(
				farPoint.x,
				nearFloorPoint.y,
				farPoint.y);

			const auto drawRange = SoftwareRenderer::makeDrawRange(
				nearFloorPoint, farFloorPoint, camera, frame);
			const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
				voxelX, voxelY, voxelZ, fadingVoxels);

			SoftwareRenderer::drawPerspectivePixels(x, drawRange, nearPoint, farPoint, nearZ,
				farZ, -Double3::UnitY, textures.at(ceilingData.id), fadePercent, shadingInfo,
				occlusion, frame);
		}
		else if (voxelData.dataType == VoxelDataType::Raised)
		{
			const VoxelData::RaisedData &raisedData = voxelData.raised;

			const Double3 nearCeilingPoint(
				nearPoint.x,
				voxelYReal + ((raisedData.yOffset + raisedData.ySize) * voxelHeight),
				nearPoint.y);
			const Double3 nearFloorPoint(
				nearPoint.x,
				voxelYReal + (raisedData.yOffset * voxelHeight),
				nearPoint.y);

			// Draw order depends on the player's Y position relative to the platform.
			if (camera.eye.y > nearCeilingPoint.y)
			{
				// Above platform.
				const Double3 farCeilingPoint(
					farPoint.x,
					nearCeilingPoint.y,
					farPoint.y);

				const auto drawRange = SoftwareRenderer::makeDrawRange(
					farCeilingPoint, nearCeilingPoint, camera, frame);
				const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
					voxelX, voxelY, voxelZ, fadingVoxels);

				// Ceiling.
				SoftwareRenderer::drawPerspectivePixels(x, drawRange, farPoint, nearPoint, farZ,
					nearZ, Double3::UnitY, textures.at(raisedData.ceilingID), fadePercent,
					shadingInfo, occlusion, frame);
			}
			else if (camera.eye.y < nearFloorPoint.y)
			{
				// Below platform.
				const Double3 farFloorPoint(
					farPoint.x,
					nearFloorPoint.y,
					farPoint.y);

				const auto drawRange = SoftwareRenderer::makeDrawRange(
					nearFloorPoint, farFloorPoint, camera, frame);
				const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
					voxelX, voxelY, voxelZ, fadingVoxels);

				// Floor.
				SoftwareRenderer::drawPerspectivePixels(x, drawRange, nearPoint, farPoint, nearZ,
					farZ, -Double3::UnitY, textures.at(raisedData.floorID), fadePercent,
					shadingInfo, occlusion, frame);
			}
			else
			{
				// Between top and bottom.
				const Double3 farCeilingPoint(
					farPoint.x,
					nearCeilingPoint.y,
					farPoint.y);
				const Double3 farFloorPoint(
					farPoint.x,
					nearFloorPoint.y,
					farPoint.y);

				const auto drawRanges = SoftwareRenderer::makeDrawRangeThreePart(
					nearCeilingPoint, farCeilingPoint, farFloorPoint, nearFloorPoint,
					camera, frame);
				const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
					voxelX, voxelY, voxelZ, fadingVoxels);

				// Ceiling.
				SoftwareRenderer::drawPerspectivePixels(x, drawRanges.at(0), nearPoint, farPoint,
					nearZ, farZ, -Double3::UnitY, textures.at(raisedData.ceilingID), fadePercent,
					shadingInfo, occlusion, frame);

				// Wall.
				SoftwareRenderer::drawTransparentPixels(x, drawRanges.at(1), farZ, wallU,
					raisedData.vTop, raisedData.vBottom, wallNormal,
					textures.at(raisedData.sideID), shadingInfo, occlusion, frame);

				// Floor.
				SoftwareRenderer::drawPerspectivePixels(x, drawRanges.at(2), farPoint, nearPoint,
					farZ, nearZ, Double3::UnitY, textures.at(raisedData.floorID), fadePercent,
					shadingInfo, occlusion, frame);
			}
		}
		else if (voxelData.dataType == VoxelDataType::Diagonal)
		{
			const VoxelData::DiagonalData &diagData = voxelData.diagonal;

			// Find intersection.
			RayHit hit;
			const bool success = diagData.type1 ?
				SoftwareRenderer::findDiag1Intersection(voxelX, voxelZ, nearPoint, farPoint, hit) :
				SoftwareRenderer::findDiag2Intersection(voxelX, voxelZ, nearPoint, farPoint, hit);

			if (success)
			{
				const Double3 diagTopPoint(
					hit.point.x,
					voxelYReal + voxelHeight,
					hit.point.y);
				const Double3 diagBottomPoint(
					diagTopPoint.x,
					voxelYReal,
					diagTopPoint.z);

				const auto drawRange = SoftwareRenderer::makeDrawRange(
					diagTopPoint, diagBottomPoint, camera, frame);
				const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
					voxelX, voxelY, voxelZ, fadingVoxels);

				SoftwareRenderer::drawPixels(x, drawRange, nearZ + hit.innerZ, hit.u, 0.0,
					Constants::JustBelowOne, hit.normal, textures.at(diagData.id), fadePercent,
					shadingInfo, occlusion, frame);
			}
		}
		else if (voxelData.dataType == VoxelDataType::TransparentWall)
		{
			// Do nothing. Transparent walls have no back-faces.
		}
		else if (voxelData.dataType == VoxelDataType::Edge)
		{
			const VoxelData::EdgeData &edgeData = voxelData.edge;

			// Find intersection.
			RayHit hit;
			const bool success = SoftwareRenderer::findInitialEdgeIntersection(
				voxelX, voxelZ, edgeData.facing, edgeData.flipped, nearPoint, farPoint,
				camera, ray, hit);

			if (success)
			{
				const Double3 edgeTopPoint(
					hit.point.x,
					voxelYReal + voxelHeight + edgeData.yOffset,
					hit.point.y);
				const Double3 edgeBottomPoint(
					hit.point.x,
					voxelYReal + edgeData.yOffset,
					hit.point.y);

				const auto drawRange = SoftwareRenderer::makeDrawRange(
					edgeTopPoint, edgeBottomPoint, camera, frame);

				SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ + hit.innerZ, hit.u,
					0.0, Constants::JustBelowOne, hit.normal, textures.at(edgeData.id),
					shadingInfo, occlusion, frame);
			}
		}
		else if (voxelData.dataType == VoxelDataType::Chasm)
		{
			// Ignore. Chasms should never be above the player's voxel.
		}
		else if (voxelData.dataType == VoxelDataType::Door)
		{
			const VoxelData::DoorData &doorData = voxelData.door;
			const double percentOpen = SoftwareRenderer::getDoorPercentOpen(
				voxelX, voxelZ, openDoors);

			RayHit hit;
			const bool success = SoftwareRenderer::findInitialDoorIntersection(voxelX, voxelZ,
				doorData.type, percentOpen, nearPoint, farPoint, camera, ray, voxelGrid, hit);

			if (success)
			{
				if (doorData.type == VoxelData::DoorData::Type::Swinging)
				{
					const Double3 doorTopPoint(
						hit.point.x,
						voxelYReal + voxelHeight,
						hit.point.y);
					const Double3 doorBottomPoint(
						doorTopPoint.x,
						voxelYReal,
						doorTopPoint.z);

					const auto drawRange = SoftwareRenderer::makeDrawRange(
						doorTopPoint, doorBottomPoint, camera, frame);

					SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ + hit.innerZ,
						hit.u, 0.0, Constants::JustBelowOne, hit.normal, textures.at(doorData.id),
						shadingInfo, occlusion, frame);
				}
				else if (doorData.type == VoxelData::DoorData::Type::Sliding)
				{
					const Double3 doorTopPoint(
						hit.point.x,
						voxelYReal + voxelHeight,
						hit.point.y);
					const Double3 doorBottomPoint(
						doorTopPoint.x,
						voxelYReal,
						doorTopPoint.z);

					const auto drawRange = SoftwareRenderer::makeDrawRange(
						doorTopPoint, doorBottomPoint, camera, frame);

					SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ + hit.innerZ,
						hit.u, 0.0, Constants::JustBelowOne, hit.normal, textures.at(doorData.id),
						shadingInfo, occlusion, frame);
				}
				else if (doorData.type == VoxelData::DoorData::Type::Raising)
				{
					// Top point is fixed, bottom point depends on percent open.
					const double minVisible = SoftwareRenderer::DOOR_MIN_VISIBLE;
					const double raisedAmount = (voxelHeight * (1.0 - minVisible)) * percentOpen;

					const Double3 doorTopPoint(
						hit.point.x,
						voxelYReal + voxelHeight,
						hit.point.y);
					const Double3 doorBottomPoint(
						doorTopPoint.x,
						voxelYReal + raisedAmount,
						doorTopPoint.z);

					const auto drawRange = SoftwareRenderer::makeDrawRange(
						doorTopPoint, doorBottomPoint, camera, frame);

					// The start of the vertical texture coordinate depends on the percent open.
					const double vStart = raisedAmount / voxelHeight;

					SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ + hit.innerZ,
						hit.u, vStart, Constants::JustBelowOne, hit.normal,
						textures.at(doorData.id), shadingInfo, occlusion, frame);
				}
				else if (doorData.type == VoxelData::DoorData::Type::Splitting)
				{
					const Double3 doorTopPoint(
						hit.point.x,
						voxelYReal + voxelHeight,
						hit.point.y);
					const Double3 doorBottomPoint(
						doorTopPoint.x,
						voxelYReal,
						doorTopPoint.z);

					const auto drawRange = SoftwareRenderer::makeDrawRange(
						doorTopPoint, doorBottomPoint, camera, frame);

					SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ + hit.innerZ,
						hit.u, 0.0, Constants::JustBelowOne, hit.normal, textures.at(doorData.id),
						shadingInfo, occlusion, frame);
				}
			}
		}
	};

	// Relative Y voxel coordinate of the camera, compensating for the ceiling height.
	const int adjustedVoxelY = camera.getAdjustedEyeVoxelY(ceilingHeight);

	// Draw the player's current voxel first.
	drawInitialVoxel(adjustedVoxelY);

	// Draw voxels below the player's voxel.
	for (int voxelY = (adjustedVoxelY - 1); voxelY >= 0; voxelY--)
	{
		drawInitialVoxelBelow(voxelY);
	}

	// Draw voxels above the player's voxel.
	for (int voxelY = (adjustedVoxelY + 1); voxelY < voxelGrid.getHeight(); voxelY++)
	{
		drawInitialVoxelAbove(voxelY);
	}
}

void SoftwareRenderer::drawVoxelColumn(int x, int voxelX, int voxelZ, const Camera &camera,
	const Ray &ray, VoxelData::Facing facing, const Double2 &nearPoint, const Double2 &farPoint,
	double nearZ, double farZ, const ShadingInfo &shadingInfo, double ceilingHeight,
	const std::vector<LevelData::DoorState> &openDoors,
	const std::vector<LevelData::FadeState> &fadingVoxels, const VoxelGrid &voxelGrid,
	const std::vector<VoxelTexture> &textures, OcclusionData &occlusion, const FrameView &frame)
{
	// Much of the code here is duplicated from the initial voxel column drawing method, but
	// there are a couple differences, like the horizontal texture coordinate being flipped,
	// and the drawing orders being slightly modified. The reason for having so much code is
	// so we cover all the different ray casting cases efficiently. It would slow down this 
	// method if it had to worry about an "initialColumn" boolean that's always false in the
	// general case.

	// When clamping Y values for drawing ranges, subtract 0.5 from starts and add 0.5 to 
	// ends before converting to integers because the drawing methods sample at the center 
	// of pixels. The clamping function depends on which side of the range is being clamped; 
	// either way, the drawing range should be contained within the projected range at the 
	// sub-pixel level. This ensures that the vertical texture coordinate is always within 0->1.

	// Horizontal texture coordinate for the wall, potentially shared between multiple voxels
	// in this voxel column.
	const double wallU = [&nearPoint, facing]()
	{
		const double uVal = [&nearPoint, facing]()
		{
			if (facing == VoxelData::Facing::PositiveX)
			{
				return Constants::JustBelowOne - (nearPoint.y - std::floor(nearPoint.y));
			}
			else if (facing == VoxelData::Facing::NegativeX)
			{
				return nearPoint.y - std::floor(nearPoint.y);
			}
			else if (facing == VoxelData::Facing::PositiveZ)
			{
				return nearPoint.x - std::floor(nearPoint.x);
			}
			else
			{
				return Constants::JustBelowOne - (nearPoint.x - std::floor(nearPoint.x));
			}
		}();

		return std::clamp(uVal, 0.0, Constants::JustBelowOne);
	}();

	// Normal of the wall for the incoming ray, potentially shared between multiple voxels in
	// this voxel column.
	const Double3 wallNormal = VoxelData::getNormal(facing);

	auto drawVoxel = [x, voxelX, voxelZ, &camera, &ray, facing, &wallNormal, &nearPoint,
		&farPoint, nearZ, farZ, wallU, &shadingInfo, ceilingHeight, &openDoors, &fadingVoxels,
		&voxelGrid, &textures, &occlusion, &frame](int voxelY)
	{
		const uint16_t voxelID = voxelGrid.getVoxel(voxelX, voxelY, voxelZ);
		const VoxelData &voxelData = voxelGrid.getVoxelData(voxelID);
		const double voxelHeight = ceilingHeight;
		const double voxelYReal = static_cast<double>(voxelY) * voxelHeight;

		if (voxelData.dataType == VoxelDataType::Wall)
		{
			// Draw side.
			const VoxelData::WallData &wallData = voxelData.wall;

			const Double3 nearCeilingPoint(
				nearPoint.x,
				voxelYReal + voxelHeight,
				nearPoint.y);
			const Double3 nearFloorPoint(
				nearPoint.x,
				voxelYReal,
				nearPoint.y);

			const auto drawRange = SoftwareRenderer::makeDrawRange(
				nearCeilingPoint, nearFloorPoint, camera, frame);
			const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
				voxelX, voxelY, voxelZ, fadingVoxels);

			SoftwareRenderer::drawPixels(x, drawRange, nearZ, wallU, 0.0,
				Constants::JustBelowOne, wallNormal, textures.at(wallData.sideID), fadePercent,
				shadingInfo, occlusion, frame);
		}
		else if (voxelData.dataType == VoxelDataType::Floor)
		{
			// Do nothing. Floors can only be seen from above.
		}
		else if (voxelData.dataType == VoxelDataType::Ceiling)
		{
			// Draw bottom of ceiling voxel if the camera is below it.
			if (camera.eye.y < voxelYReal)
			{
				const VoxelData::CeilingData &ceilingData = voxelData.ceiling;

				const Double3 nearFloorPoint(
					nearPoint.x,
					voxelYReal,
					nearPoint.y);
				const Double3 farFloorPoint(
					farPoint.x,
					nearFloorPoint.y,
					farPoint.y);

				const auto drawRange = SoftwareRenderer::makeDrawRange(
					nearFloorPoint, farFloorPoint, camera, frame);
				const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
					voxelX, voxelY, voxelZ, fadingVoxels);

				SoftwareRenderer::drawPerspectivePixels(x, drawRange, nearPoint, farPoint, nearZ,
					farZ, -Double3::UnitY, textures.at(ceilingData.id), fadePercent, shadingInfo,
					occlusion, frame);
			}
		}
		else if (voxelData.dataType == VoxelDataType::Raised)
		{
			const VoxelData::RaisedData &raisedData = voxelData.raised;

			const Double3 nearCeilingPoint(
				nearPoint.x,
				voxelYReal + ((raisedData.yOffset + raisedData.ySize) * voxelHeight),
				nearPoint.y);
			const Double3 nearFloorPoint(
				nearPoint.x,
				voxelYReal + (raisedData.yOffset * voxelHeight),
				nearPoint.y);

			// Draw order depends on the player's Y position relative to the platform.
			if (camera.eye.y > nearCeilingPoint.y)
			{
				// Above platform.
				const Double3 farCeilingPoint(
					farPoint.x,
					nearCeilingPoint.y,
					farPoint.y);

				const auto drawRanges = SoftwareRenderer::makeDrawRangeTwoPart(
					farCeilingPoint, nearCeilingPoint, nearFloorPoint, camera, frame);
				const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
					voxelX, voxelY, voxelZ, fadingVoxels);

				// Ceiling.
				SoftwareRenderer::drawPerspectivePixels(x, drawRanges.at(0), farPoint, nearPoint,
					farZ, nearZ, Double3::UnitY, textures.at(raisedData.ceilingID), fadePercent,
					shadingInfo, occlusion, frame);

				// Wall.
				SoftwareRenderer::drawTransparentPixels(x, drawRanges.at(1), nearZ, wallU,
					raisedData.vTop, raisedData.vBottom, wallNormal,
					textures.at(raisedData.sideID), shadingInfo, occlusion, frame);
			}
			else if (camera.eye.y < nearFloorPoint.y)
			{
				// Below platform.
				const Double3 farFloorPoint(
					farPoint.x,
					nearFloorPoint.y,
					farPoint.y);

				const auto drawRanges = SoftwareRenderer::makeDrawRangeTwoPart(
					nearCeilingPoint, nearFloorPoint, farFloorPoint, camera, frame);
				const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
					voxelX, voxelY, voxelZ, fadingVoxels);

				// Wall.
				SoftwareRenderer::drawTransparentPixels(x, drawRanges.at(0), nearZ, wallU,
					raisedData.vTop, raisedData.vBottom, wallNormal,
					textures.at(raisedData.sideID), shadingInfo, occlusion, frame);

				// Floor.
				SoftwareRenderer::drawPerspectivePixels(x, drawRanges.at(1), nearPoint, farPoint,
					nearZ, farZ, -Double3::UnitY, textures.at(raisedData.floorID), fadePercent,
					shadingInfo, occlusion, frame);
			}
			else
			{
				// Between top and bottom.
				const auto drawRange = SoftwareRenderer::makeDrawRange(
					nearCeilingPoint, nearFloorPoint, camera, frame);

				SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ, wallU,
					raisedData.vTop, raisedData.vBottom, wallNormal,
					textures.at(raisedData.sideID), shadingInfo, occlusion, frame);
			}
		}
		else if (voxelData.dataType == VoxelDataType::Diagonal)
		{
			const VoxelData::DiagonalData &diagData = voxelData.diagonal;

			// Find intersection.
			RayHit hit;
			const bool success = diagData.type1 ?
				SoftwareRenderer::findDiag1Intersection(voxelX, voxelZ, nearPoint, farPoint, hit) :
				SoftwareRenderer::findDiag2Intersection(voxelX, voxelZ, nearPoint, farPoint, hit);

			if (success)
			{
				const Double3 diagTopPoint(
					hit.point.x,
					voxelYReal + voxelHeight,
					hit.point.y);
				const Double3 diagBottomPoint(
					diagTopPoint.x,
					voxelYReal,
					diagTopPoint.z);

				const auto drawRange = SoftwareRenderer::makeDrawRange(
					diagTopPoint, diagBottomPoint, camera, frame);
				const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
					voxelX, voxelY, voxelZ, fadingVoxels);

				SoftwareRenderer::drawPixels(x, drawRange, nearZ + hit.innerZ, hit.u, 0.0,
					Constants::JustBelowOne, hit.normal, textures.at(diagData.id), fadePercent,
					shadingInfo, occlusion, frame);
			}
		}
		else if (voxelData.dataType == VoxelDataType::TransparentWall)
		{
			// Draw transparent side.
			const VoxelData::TransparentWallData &transparentWallData = voxelData.transparentWall;

			const Double3 nearCeilingPoint(
				nearPoint.x,
				voxelYReal + voxelHeight,
				nearPoint.y);
			const Double3 nearFloorPoint(
				nearPoint.x,
				voxelYReal,
				nearPoint.y);

			const auto drawRange = SoftwareRenderer::makeDrawRange(
				nearCeilingPoint, nearFloorPoint, camera, frame);

			SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ, wallU, 0.0,
				Constants::JustBelowOne, wallNormal, textures.at(transparentWallData.id),
				shadingInfo, occlusion, frame);
		}
		else if (voxelData.dataType == VoxelDataType::Edge)
		{
			const VoxelData::EdgeData &edgeData = voxelData.edge;

			// Find intersection.
			RayHit hit;
			const bool success = SoftwareRenderer::findEdgeIntersection(voxelX, voxelZ,
				edgeData.facing, edgeData.flipped, facing, nearPoint, farPoint, wallU,
				camera, ray, hit);

			if (success)
			{
				const Double3 edgeTopPoint(
					hit.point.x,
					voxelYReal + voxelHeight + edgeData.yOffset,
					hit.point.y);
				const Double3 edgeBottomPoint(
					hit.point.x,
					voxelYReal + edgeData.yOffset,
					hit.point.y);

				const auto drawRange = SoftwareRenderer::makeDrawRange(
					edgeTopPoint, edgeBottomPoint, camera, frame);

				SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ + hit.innerZ, hit.u,
					0.0, Constants::JustBelowOne, hit.normal, textures.at(edgeData.id),
					shadingInfo, occlusion, frame);
			}
		}
		else if (voxelData.dataType == VoxelDataType::Chasm)
		{
			// Render front and back-faces.
			const VoxelData::ChasmData &chasmData = voxelData.chasm;

			// Find which faces on the chasm were intersected.
			const VoxelData::Facing nearFacing = facing;
			const VoxelData::Facing farFacing = SoftwareRenderer::getChasmFarFacing(
				voxelX, voxelZ, nearFacing, camera, ray);

			// Near.
			if (chasmData.faceIsVisible(nearFacing))
			{
				const double nearU = Constants::JustBelowOne - wallU;
				const Double3 nearNormal = wallNormal;
				
				// Wet chasms and lava chasms are unaffected by ceiling height.
				const double chasmDepth = (chasmData.type == VoxelData::ChasmData::Type::Dry) ?
					voxelHeight : VoxelData::ChasmData::WET_LAVA_DEPTH;

				const Double3 nearCeilingPoint(
					nearPoint.x,
					voxelYReal + voxelHeight,
					nearPoint.y);
				const Double3 nearFloorPoint(
					nearPoint.x,
					nearCeilingPoint.y - chasmDepth,
					nearPoint.y);

				const auto drawRange = SoftwareRenderer::makeDrawRange(
					nearCeilingPoint, nearFloorPoint, camera, frame);

				SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ, nearU, 0.0,
					Constants::JustBelowOne, nearNormal, textures.at(chasmData.id),
					shadingInfo, occlusion, frame);
			}

			// Far.
			if (chasmData.faceIsVisible(farFacing))
			{
				const double farU = [&farPoint, farFacing]()
				{
					const double uVal = [&farPoint, farFacing]()
					{
						if (farFacing == VoxelData::Facing::PositiveX)
						{
							return farPoint.y - std::floor(farPoint.y);
						}
						else if (farFacing == VoxelData::Facing::NegativeX)
						{
							return Constants::JustBelowOne - (farPoint.y - std::floor(farPoint.y));
						}
						else if (farFacing == VoxelData::Facing::PositiveZ)
						{
							return Constants::JustBelowOne - (farPoint.x - std::floor(farPoint.x));
						}
						else
						{
							return farPoint.x - std::floor(farPoint.x);
						}
					}();

					return std::clamp(uVal, 0.0, Constants::JustBelowOne);
				}();

				const Double3 farNormal = -VoxelData::getNormal(farFacing);

				// Wet chasms and lava chasms are unaffected by ceiling height.
				const double chasmDepth = (chasmData.type == VoxelData::ChasmData::Type::Dry) ?
					voxelHeight : VoxelData::ChasmData::WET_LAVA_DEPTH;

				const Double3 farCeilingPoint(
					farPoint.x,
					voxelYReal + voxelHeight,
					farPoint.y);
				const Double3 farFloorPoint(
					farPoint.x,
					farCeilingPoint.y - chasmDepth,
					farPoint.y);

				const auto drawRange = SoftwareRenderer::makeDrawRange(
					farCeilingPoint, farFloorPoint, camera, frame);

				SoftwareRenderer::drawTransparentPixels(x, drawRange, farZ, farU, 0.0,
					Constants::JustBelowOne, farNormal, textures.at(chasmData.id),
					shadingInfo, occlusion, frame);
			}
		}
		else if (voxelData.dataType == VoxelDataType::Door)
		{
			const VoxelData::DoorData &doorData = voxelData.door;
			const double percentOpen = SoftwareRenderer::getDoorPercentOpen(
				voxelX, voxelZ, openDoors);

			RayHit hit;
			const bool success = SoftwareRenderer::findDoorIntersection(voxelX, voxelZ,
				doorData.type, percentOpen, facing, nearPoint, farPoint, wallU, hit);

			if (success)
			{
				if (doorData.type == VoxelData::DoorData::Type::Swinging)
				{
					const Double3 doorTopPoint(
						hit.point.x,
						voxelYReal + voxelHeight,
						hit.point.y);
					const Double3 doorBottomPoint(
						doorTopPoint.x,
						voxelYReal,
						doorTopPoint.z);

					const auto drawRange = SoftwareRenderer::makeDrawRange(
						doorTopPoint, doorBottomPoint, camera, frame);

					SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ + hit.innerZ,
						hit.u, 0.0, Constants::JustBelowOne, hit.normal, textures.at(doorData.id),
						shadingInfo, occlusion, frame);
				}
				else if (doorData.type == VoxelData::DoorData::Type::Sliding)
				{
					const Double3 doorTopPoint(
						hit.point.x,
						voxelYReal + voxelHeight,
						hit.point.y);
					const Double3 doorBottomPoint(
						doorTopPoint.x,
						voxelYReal,
						doorTopPoint.z);

					const auto drawRange = SoftwareRenderer::makeDrawRange(
						doorTopPoint, doorBottomPoint, camera, frame);

					SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ, hit.u, 0.0,
						Constants::JustBelowOne, hit.normal, textures.at(doorData.id),
						shadingInfo, occlusion, frame);
				}
				else if (doorData.type == VoxelData::DoorData::Type::Raising)
				{
					// Top point is fixed, bottom point depends on percent open.
					const double minVisible = SoftwareRenderer::DOOR_MIN_VISIBLE;
					const double raisedAmount = (voxelHeight * (1.0 - minVisible)) * percentOpen;

					const Double3 doorTopPoint(
						hit.point.x,
						voxelYReal + voxelHeight,
						hit.point.y);
					const Double3 doorBottomPoint(
						doorTopPoint.x,
						voxelYReal + raisedAmount,
						doorTopPoint.z);

					const auto drawRange = SoftwareRenderer::makeDrawRange(
						doorTopPoint, doorBottomPoint, camera, frame);

					// The start of the vertical texture coordinate depends on the percent open.
					const double vStart = raisedAmount / voxelHeight;

					SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ, hit.u, vStart,
						Constants::JustBelowOne, hit.normal, textures.at(doorData.id), shadingInfo,
						occlusion, frame);
				}
				else if (doorData.type == VoxelData::DoorData::Type::Splitting)
				{
					const Double3 doorTopPoint(
						hit.point.x,
						voxelYReal + voxelHeight,
						hit.point.y);
					const Double3 doorBottomPoint(
						doorTopPoint.x,
						voxelYReal,
						doorTopPoint.z);

					const auto drawRange = SoftwareRenderer::makeDrawRange(
						doorTopPoint, doorBottomPoint, camera, frame);

					SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ, hit.u, 0.0,
						Constants::JustBelowOne, hit.normal, textures.at(doorData.id),
						shadingInfo, occlusion, frame);
				}
			}
		}
	};

	auto drawVoxelBelow = [x, voxelX, voxelZ, &camera, &ray, facing, &wallNormal, &nearPoint,
		&farPoint, nearZ, farZ, wallU, &shadingInfo, ceilingHeight, &openDoors, &fadingVoxels,
		&voxelGrid, &textures, &occlusion, &frame](int voxelY)
	{
		const uint16_t voxelID = voxelGrid.getVoxel(voxelX, voxelY, voxelZ);
		const VoxelData &voxelData = voxelGrid.getVoxelData(voxelID);
		const double voxelHeight = ceilingHeight;
		const double voxelYReal = static_cast<double>(voxelY) * voxelHeight;

		if (voxelData.dataType == VoxelDataType::Wall)
		{
			const VoxelData::WallData &wallData = voxelData.wall;

			const Double3 farCeilingPoint(
				farPoint.x,
				voxelYReal + voxelHeight,
				farPoint.y);
			const Double3 nearCeilingPoint(
				nearPoint.x,
				farCeilingPoint.y,
				nearPoint.y);
			const Double3 nearFloorPoint(
				nearPoint.x,
				voxelYReal,
				nearPoint.y);

			const auto drawRanges = SoftwareRenderer::makeDrawRangeTwoPart(
				farCeilingPoint, nearCeilingPoint, nearFloorPoint, camera, frame);
			const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
				voxelX, voxelY, voxelZ, fadingVoxels);

			// Ceiling.
			SoftwareRenderer::drawPerspectivePixels(x, drawRanges.at(0), farPoint, nearPoint, farZ,
				nearZ, Double3::UnitY, textures.at(wallData.ceilingID), fadePercent, shadingInfo,
				occlusion, frame);

			// Wall.
			SoftwareRenderer::drawPixels(x, drawRanges.at(1), nearZ, wallU, 0.0,
				Constants::JustBelowOne, wallNormal, textures.at(wallData.sideID), fadePercent,
				shadingInfo, occlusion, frame);
		}
		else if (voxelData.dataType == VoxelDataType::Floor)
		{
			// Draw top of floor voxel.
			const VoxelData::FloorData &floorData = voxelData.floor;

			const Double3 farCeilingPoint(
				farPoint.x,
				voxelYReal + voxelHeight,
				farPoint.y);
			const Double3 nearCeilingPoint(
				nearPoint.x,
				farCeilingPoint.y,
				nearPoint.y);

			const auto drawRange = SoftwareRenderer::makeDrawRange(
				farCeilingPoint, nearCeilingPoint, camera, frame);
			const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
				voxelX, voxelY, voxelZ, fadingVoxels);

			SoftwareRenderer::drawPerspectivePixels(x, drawRange, farPoint, nearPoint, farZ,
				nearZ, Double3::UnitY, textures.at(floorData.id), fadePercent, shadingInfo, 
				occlusion, frame);
		}
		else if (voxelData.dataType == VoxelDataType::Ceiling)
		{
			// Do nothing. Ceilings can only be seen from below.
		}
		else if (voxelData.dataType == VoxelDataType::Raised)
		{
			const VoxelData::RaisedData &raisedData = voxelData.raised;

			const Double3 nearCeilingPoint(
				nearPoint.x,
				voxelYReal + ((raisedData.yOffset + raisedData.ySize) * voxelHeight),
				nearPoint.y);
			const Double3 nearFloorPoint(
				nearPoint.x,
				voxelYReal + (raisedData.yOffset * voxelHeight),
				nearPoint.y);

			// Draw order depends on the player's Y position relative to the platform.
			if (camera.eye.y > nearCeilingPoint.y)
			{
				// Above platform.
				const Double3 farCeilingPoint(
					farPoint.x,
					nearCeilingPoint.y,
					farPoint.y);

				const auto drawRanges = SoftwareRenderer::makeDrawRangeTwoPart(
					farCeilingPoint, nearCeilingPoint, nearFloorPoint, camera, frame);
				const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
					voxelX, voxelY, voxelZ, fadingVoxels);

				// Ceiling.
				SoftwareRenderer::drawPerspectivePixels(x, drawRanges.at(0), farPoint, nearPoint,
					farZ, nearZ, Double3::UnitY, textures.at(raisedData.ceilingID), fadePercent,
					shadingInfo, occlusion, frame);

				// Wall.
				SoftwareRenderer::drawTransparentPixels(x, drawRanges.at(1), nearZ, wallU,
					raisedData.vTop, raisedData.vBottom, wallNormal,
					textures.at(raisedData.sideID), shadingInfo, occlusion, frame);
			}
			else if (camera.eye.y < nearFloorPoint.y)
			{
				// Below platform.
				const Double3 farFloorPoint(
					farPoint.x,
					nearFloorPoint.y,
					farPoint.y);

				const auto drawRanges = SoftwareRenderer::makeDrawRangeTwoPart(
					nearCeilingPoint, nearFloorPoint, farFloorPoint, camera, frame);
				const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
					voxelX, voxelY, voxelZ, fadingVoxels);

				// Wall.
				SoftwareRenderer::drawTransparentPixels(x, drawRanges.at(0), nearZ, wallU,
					raisedData.vTop, raisedData.vBottom, wallNormal,
					textures.at(raisedData.sideID), shadingInfo, occlusion, frame);

				// Floor.
				SoftwareRenderer::drawPerspectivePixels(x, drawRanges.at(1), nearPoint, farPoint,
					nearZ, farZ, -Double3::UnitY, textures.at(raisedData.floorID), fadePercent,
					shadingInfo, occlusion, frame);
			}
			else
			{
				// Between top and bottom.
				const auto drawRange = SoftwareRenderer::makeDrawRange(
					nearCeilingPoint, nearFloorPoint, camera, frame);

				SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ, wallU,
					raisedData.vTop, raisedData.vBottom, wallNormal,
					textures.at(raisedData.sideID), shadingInfo, occlusion, frame);
			}
		}
		else if (voxelData.dataType == VoxelDataType::Diagonal)
		{
			const VoxelData::DiagonalData &diagData = voxelData.diagonal;

			// Find intersection.
			RayHit hit;
			const bool success = diagData.type1 ?
				SoftwareRenderer::findDiag1Intersection(voxelX, voxelZ, nearPoint, farPoint, hit) :
				SoftwareRenderer::findDiag2Intersection(voxelX, voxelZ, nearPoint, farPoint, hit);

			if (success)
			{
				const Double3 diagTopPoint(
					hit.point.x,
					voxelYReal + voxelHeight,
					hit.point.y);
				const Double3 diagBottomPoint(
					diagTopPoint.x,
					voxelYReal,
					diagTopPoint.z);

				const auto drawRange = SoftwareRenderer::makeDrawRange(
					diagTopPoint, diagBottomPoint, camera, frame);
				const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
					voxelX, voxelY, voxelZ, fadingVoxels);

				SoftwareRenderer::drawPixels(x, drawRange, nearZ + hit.innerZ, hit.u, 0.0,
					Constants::JustBelowOne, hit.normal, textures.at(diagData.id), fadePercent,
					shadingInfo, occlusion, frame);
			}
		}
		else if (voxelData.dataType == VoxelDataType::TransparentWall)
		{
			// Draw transparent side.
			const VoxelData::TransparentWallData &transparentWallData = voxelData.transparentWall;

			const Double3 nearCeilingPoint(
				nearPoint.x,
				voxelYReal + voxelHeight,
				nearPoint.y);
			const Double3 nearFloorPoint(
				nearPoint.x,
				voxelYReal,
				nearPoint.y);

			const auto drawRange = SoftwareRenderer::makeDrawRange(
				nearCeilingPoint, nearFloorPoint, camera, frame);

			SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ, wallU, 0.0,
				Constants::JustBelowOne, wallNormal, textures.at(transparentWallData.id),
				shadingInfo, occlusion, frame);
		}
		else if (voxelData.dataType == VoxelDataType::Edge)
		{
			const VoxelData::EdgeData &edgeData = voxelData.edge;

			// Find intersection.
			RayHit hit;
			const bool success = SoftwareRenderer::findEdgeIntersection(voxelX, voxelZ,
				edgeData.facing, edgeData.flipped, facing, nearPoint, farPoint, wallU,
				camera, ray, hit);

			if (success)
			{
				const Double3 edgeTopPoint(
					hit.point.x,
					voxelYReal + voxelHeight + edgeData.yOffset,
					hit.point.y);
				const Double3 edgeBottomPoint(
					hit.point.x,
					voxelYReal + edgeData.yOffset,
					hit.point.y);

				const auto drawRange = SoftwareRenderer::makeDrawRange(
					edgeTopPoint, edgeBottomPoint, camera, frame);

				SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ + hit.innerZ, hit.u,
					0.0, Constants::JustBelowOne, hit.normal, textures.at(edgeData.id),
					shadingInfo, occlusion, frame);
			}
		}
		else if (voxelData.dataType == VoxelDataType::Chasm)
		{
			// Render front and back-faces.
			const VoxelData::ChasmData &chasmData = voxelData.chasm;

			// Find which faces on the chasm were intersected.
			const VoxelData::Facing nearFacing = facing;
			const VoxelData::Facing farFacing = SoftwareRenderer::getChasmFarFacing(
				voxelX, voxelZ, nearFacing, camera, ray);

			// Near.
			if (chasmData.faceIsVisible(nearFacing))
			{
				const double nearU = Constants::JustBelowOne - wallU;
				const Double3 nearNormal = wallNormal;

				// Wet chasms and lava chasms are unaffected by ceiling height.
				const double chasmDepth = (chasmData.type == VoxelData::ChasmData::Type::Dry) ?
					voxelHeight : VoxelData::ChasmData::WET_LAVA_DEPTH;

				const Double3 nearCeilingPoint(
					nearPoint.x,
					voxelYReal + voxelHeight,
					nearPoint.y);
				const Double3 nearFloorPoint(
					nearPoint.x,
					nearCeilingPoint.y - chasmDepth,
					nearPoint.y);

				const auto drawRange = SoftwareRenderer::makeDrawRange(
					nearCeilingPoint, nearFloorPoint, camera, frame);

				SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ, nearU, 0.0,
					Constants::JustBelowOne, nearNormal, textures.at(chasmData.id),
					shadingInfo, occlusion, frame);
			}

			// Far.
			if (chasmData.faceIsVisible(farFacing))
			{
				const double farU = [&farPoint, farFacing]()
				{
					const double uVal = [&farPoint, farFacing]()
					{
						if (farFacing == VoxelData::Facing::PositiveX)
						{
							return farPoint.y - std::floor(farPoint.y);
						}
						else if (farFacing == VoxelData::Facing::NegativeX)
						{
							return Constants::JustBelowOne - (farPoint.y - std::floor(farPoint.y));
						}
						else if (farFacing == VoxelData::Facing::PositiveZ)
						{
							return Constants::JustBelowOne - (farPoint.x - std::floor(farPoint.x));
						}
						else
						{
							return farPoint.x - std::floor(farPoint.x);
						}
					}();

					return std::clamp(uVal, 0.0, Constants::JustBelowOne);
				}();

				const Double3 farNormal = -VoxelData::getNormal(farFacing);

				// Wet chasms and lava chasms are unaffected by ceiling height.
				const double chasmDepth = (chasmData.type == VoxelData::ChasmData::Type::Dry) ?
					voxelHeight : VoxelData::ChasmData::WET_LAVA_DEPTH;

				const Double3 farCeilingPoint(
					farPoint.x,
					voxelYReal + voxelHeight,
					farPoint.y);
				const Double3 farFloorPoint(
					farPoint.x,
					farCeilingPoint.y - chasmDepth,
					farPoint.y);

				const auto drawRange = SoftwareRenderer::makeDrawRange(
					farCeilingPoint, farFloorPoint, camera, frame);

				SoftwareRenderer::drawTransparentPixels(x, drawRange, farZ, farU, 0.0,
					Constants::JustBelowOne, farNormal, textures.at(chasmData.id),
					shadingInfo, occlusion, frame);
			}
		}
		else if (voxelData.dataType == VoxelDataType::Door)
		{
			const VoxelData::DoorData &doorData = voxelData.door;
			const double percentOpen = SoftwareRenderer::getDoorPercentOpen(
				voxelX, voxelZ, openDoors);

			RayHit hit;
			const bool success = SoftwareRenderer::findDoorIntersection(voxelX, voxelZ,
				doorData.type, percentOpen, facing, nearPoint, farPoint, wallU, hit);

			if (success)
			{
				if (doorData.type == VoxelData::DoorData::Type::Swinging)
				{
					const Double3 doorTopPoint(
						hit.point.x,
						voxelYReal + voxelHeight,
						hit.point.y);
					const Double3 doorBottomPoint(
						doorTopPoint.x,
						voxelYReal,
						doorTopPoint.z);

					const auto drawRange = SoftwareRenderer::makeDrawRange(
						doorTopPoint, doorBottomPoint, camera, frame);

					SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ + hit.innerZ,
						hit.u, 0.0, Constants::JustBelowOne, hit.normal, textures.at(doorData.id),
						shadingInfo, occlusion, frame);
				}
				else if (doorData.type == VoxelData::DoorData::Type::Sliding)
				{
					const Double3 doorTopPoint(
						hit.point.x,
						voxelYReal + voxelHeight,
						hit.point.y);
					const Double3 doorBottomPoint(
						doorTopPoint.x,
						voxelYReal,
						doorTopPoint.z);

					const auto drawRange = SoftwareRenderer::makeDrawRange(
						doorTopPoint, doorBottomPoint, camera, frame);

					SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ, hit.u, 0.0,
						Constants::JustBelowOne, hit.normal, textures.at(doorData.id),
						shadingInfo, occlusion, frame);
				}
				else if (doorData.type == VoxelData::DoorData::Type::Raising)
				{
					// Top point is fixed, bottom point depends on percent open.
					const double minVisible = SoftwareRenderer::DOOR_MIN_VISIBLE;
					const double raisedAmount = (voxelHeight * (1.0 - minVisible)) * percentOpen;

					const Double3 doorTopPoint(
						hit.point.x,
						voxelYReal + voxelHeight,
						hit.point.y);
					const Double3 doorBottomPoint(
						doorTopPoint.x,
						voxelYReal + raisedAmount,
						doorTopPoint.z);

					const auto drawRange = SoftwareRenderer::makeDrawRange(
						doorTopPoint, doorBottomPoint, camera, frame);

					// The start of the vertical texture coordinate depends on the percent open.
					const double vStart = raisedAmount / voxelHeight;

					SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ, hit.u, vStart,
						Constants::JustBelowOne, hit.normal, textures.at(doorData.id), shadingInfo,
						occlusion, frame);
				}
				else if (doorData.type == VoxelData::DoorData::Type::Splitting)
				{
					const Double3 doorTopPoint(
						hit.point.x,
						voxelYReal + voxelHeight,
						hit.point.y);
					const Double3 doorBottomPoint(
						doorTopPoint.x,
						voxelYReal,
						doorTopPoint.z);

					const auto drawRange = SoftwareRenderer::makeDrawRange(
						doorTopPoint, doorBottomPoint, camera, frame);

					SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ, hit.u, 0.0,
						Constants::JustBelowOne, hit.normal, textures.at(doorData.id),
						shadingInfo, occlusion, frame);
				}
			}
		}
	};

	auto drawVoxelAbove = [x, voxelX, voxelZ, &camera, &ray, facing, &wallNormal, &nearPoint,
		&farPoint, nearZ, farZ, wallU, &shadingInfo, ceilingHeight, &openDoors, &fadingVoxels,
		&voxelGrid, &textures, &occlusion, &frame](int voxelY)
	{
		const uint16_t voxelID = voxelGrid.getVoxel(voxelX, voxelY, voxelZ);
		const VoxelData &voxelData = voxelGrid.getVoxelData(voxelID);
		const double voxelHeight = ceilingHeight;
		const double voxelYReal = static_cast<double>(voxelY) * voxelHeight;

		if (voxelData.dataType == VoxelDataType::Wall)
		{
			const VoxelData::WallData &wallData = voxelData.wall;

			const Double3 nearCeilingPoint(
				nearPoint.x,
				voxelYReal + voxelHeight,
				nearPoint.y);
			const Double3 nearFloorPoint(
				nearPoint.x,
				voxelYReal,
				nearPoint.y);
			const Double3 farFloorPoint(
				farPoint.x,
				nearFloorPoint.y,
				farPoint.y);

			const auto drawRanges = SoftwareRenderer::makeDrawRangeTwoPart(
				nearCeilingPoint, nearFloorPoint, farFloorPoint, camera, frame);
			const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
				voxelX, voxelY, voxelZ, fadingVoxels);
			
			// Wall.
			SoftwareRenderer::drawPixels(x, drawRanges.at(0), nearZ, wallU, 0.0,
				Constants::JustBelowOne, wallNormal, textures.at(wallData.sideID), fadePercent,
				shadingInfo, occlusion, frame);

			// Floor.
			SoftwareRenderer::drawPerspectivePixels(x, drawRanges.at(1), nearPoint, farPoint,
				nearZ, farZ, -Double3::UnitY, textures.at(wallData.floorID), fadePercent,
				shadingInfo, occlusion, frame);
		}
		else if (voxelData.dataType == VoxelDataType::Floor)
		{
			// Do nothing. Floors can only be seen from above.
		}
		else if (voxelData.dataType == VoxelDataType::Ceiling)
		{
			// Draw bottom of ceiling voxel.
			const VoxelData::CeilingData &ceilingData = voxelData.ceiling;

			const Double3 nearFloorPoint(
				nearPoint.x,
				voxelYReal,
				nearPoint.y);
			const Double3 farFloorPoint(
				farPoint.x,
				nearFloorPoint.y,
				farPoint.y);

			const auto drawRange = SoftwareRenderer::makeDrawRange(
				nearFloorPoint, farFloorPoint, camera, frame);
			const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
				voxelX, voxelY, voxelZ, fadingVoxels);

			SoftwareRenderer::drawPerspectivePixels(x, drawRange, nearPoint, farPoint, nearZ,
				farZ, -Double3::UnitY, textures.at(ceilingData.id), fadePercent, shadingInfo,
				occlusion, frame);
		}
		else if (voxelData.dataType == VoxelDataType::Raised)
		{
			const VoxelData::RaisedData &raisedData = voxelData.raised;

			const Double3 nearCeilingPoint(
				nearPoint.x,
				voxelYReal + ((raisedData.yOffset + raisedData.ySize) * voxelHeight),
				nearPoint.y);
			const Double3 nearFloorPoint(
				nearPoint.x,
				voxelYReal + (raisedData.yOffset * voxelHeight),
				nearPoint.y);

			// Draw order depends on the player's Y position relative to the platform.
			if (camera.eye.y > nearCeilingPoint.y)
			{
				// Above platform.
				const Double3 farCeilingPoint(
					farPoint.x,
					nearCeilingPoint.y,
					farPoint.y);

				const auto drawRanges = SoftwareRenderer::makeDrawRangeTwoPart(
					farCeilingPoint, nearCeilingPoint, nearFloorPoint, camera, frame);
				const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
					voxelX, voxelY, voxelZ, fadingVoxels);

				// Ceiling.
				SoftwareRenderer::drawPerspectivePixels(x, drawRanges.at(0), farPoint, nearPoint,
					farZ, nearZ, Double3::UnitY, textures.at(raisedData.ceilingID), fadePercent,
					shadingInfo, occlusion, frame);

				// Wall.
				SoftwareRenderer::drawTransparentPixels(x, drawRanges.at(1), nearZ, wallU,
					raisedData.vTop, raisedData.vBottom, wallNormal,
					textures.at(raisedData.sideID), shadingInfo, occlusion, frame);
			}
			else if (camera.eye.y < nearFloorPoint.y)
			{
				// Below platform.
				const Double3 farFloorPoint(
					farPoint.x,
					nearFloorPoint.y,
					farPoint.y);

				const auto drawRanges = SoftwareRenderer::makeDrawRangeTwoPart(
					nearCeilingPoint, nearFloorPoint, farFloorPoint, camera, frame);
				const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
					voxelX, voxelY, voxelZ, fadingVoxels);

				// Wall.
				SoftwareRenderer::drawTransparentPixels(x, drawRanges.at(0), nearZ, wallU,
					raisedData.vTop, raisedData.vBottom, wallNormal,
					textures.at(raisedData.sideID), shadingInfo, occlusion, frame);

				// Floor.
				SoftwareRenderer::drawPerspectivePixels(x, drawRanges.at(1), nearPoint, farPoint,
					nearZ, farZ, -Double3::UnitY, textures.at(raisedData.floorID), fadePercent,
					shadingInfo, occlusion, frame);
			}
			else
			{
				// Between top and bottom.
				const auto drawRange = SoftwareRenderer::makeDrawRange(
					nearCeilingPoint, nearFloorPoint, camera, frame);

				SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ, wallU,
					raisedData.vTop, raisedData.vBottom, wallNormal,
					textures.at(raisedData.sideID), shadingInfo, occlusion, frame);
			}
		}
		else if (voxelData.dataType == VoxelDataType::Diagonal)
		{
			const VoxelData::DiagonalData &diagData = voxelData.diagonal;

			// Find intersection.
			RayHit hit;
			const bool success = diagData.type1 ?
				SoftwareRenderer::findDiag1Intersection(voxelX, voxelZ, nearPoint, farPoint, hit) :
				SoftwareRenderer::findDiag2Intersection(voxelX, voxelZ, nearPoint, farPoint, hit);

			if (success)
			{
				const Double3 diagTopPoint(
					hit.point.x,
					voxelYReal + voxelHeight,
					hit.point.y);
				const Double3 diagBottomPoint(
					diagTopPoint.x,
					voxelYReal,
					diagTopPoint.z);

				const auto drawRange = SoftwareRenderer::makeDrawRange(
					diagTopPoint, diagBottomPoint, camera, frame);
				const double fadePercent = SoftwareRenderer::getFadingVoxelPercent(
					voxelX, voxelY, voxelZ, fadingVoxels);

				SoftwareRenderer::drawPixels(x, drawRange, nearZ + hit.innerZ, hit.u, 0.0,
					Constants::JustBelowOne, hit.normal, textures.at(diagData.id), fadePercent,
					shadingInfo, occlusion, frame);
			}
		}
		else if (voxelData.dataType == VoxelDataType::TransparentWall)
		{
			// Draw transparent side.
			const VoxelData::TransparentWallData &transparentWallData = voxelData.transparentWall;

			const Double3 nearCeilingPoint(
				nearPoint.x,
				voxelYReal + voxelHeight,
				nearPoint.y);
			const Double3 nearFloorPoint(
				nearPoint.x,
				voxelYReal,
				nearPoint.y);

			const auto drawRange = SoftwareRenderer::makeDrawRange(
				nearCeilingPoint, nearFloorPoint, camera, frame);

			SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ, wallU, 0.0,
				Constants::JustBelowOne, wallNormal, textures.at(transparentWallData.id),
				shadingInfo, occlusion, frame);
		}
		else if (voxelData.dataType == VoxelDataType::Edge)
		{
			const VoxelData::EdgeData &edgeData = voxelData.edge;

			// Find intersection.
			RayHit hit;
			const bool success = SoftwareRenderer::findEdgeIntersection(voxelX, voxelZ,
				edgeData.facing, edgeData.flipped, facing, nearPoint, farPoint, wallU,
				camera, ray, hit);

			if (success)
			{
				const Double3 edgeTopPoint(
					hit.point.x,
					voxelYReal + voxelHeight + edgeData.yOffset,
					hit.point.y);
				const Double3 edgeBottomPoint(
					hit.point.x,
					voxelYReal + edgeData.yOffset,
					hit.point.y);

				const auto drawRange = SoftwareRenderer::makeDrawRange(
					edgeTopPoint, edgeBottomPoint, camera, frame);

				SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ + hit.innerZ, hit.u,
					0.0, Constants::JustBelowOne, hit.normal, textures.at(edgeData.id),
					shadingInfo, occlusion, frame);
			}
		}
		else if (voxelData.dataType == VoxelDataType::Chasm)
		{
			// Ignore. Chasms should never be above the player's voxel.
		}
		else if (voxelData.dataType == VoxelDataType::Door)
		{
			const VoxelData::DoorData &doorData = voxelData.door;
			const double percentOpen = SoftwareRenderer::getDoorPercentOpen(
				voxelX, voxelZ, openDoors);

			RayHit hit;
			const bool success = SoftwareRenderer::findDoorIntersection(voxelX, voxelZ,
				doorData.type, percentOpen, facing, nearPoint, farPoint, wallU, hit);

			if (success)
			{
				if (doorData.type == VoxelData::DoorData::Type::Swinging)
				{
					const Double3 doorTopPoint(
						hit.point.x,
						voxelYReal + voxelHeight,
						hit.point.y);
					const Double3 doorBottomPoint(
						doorTopPoint.x,
						voxelYReal,
						doorTopPoint.z);

					const auto drawRange = SoftwareRenderer::makeDrawRange(
						doorTopPoint, doorBottomPoint, camera, frame);

					SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ + hit.innerZ,
						hit.u, 0.0, Constants::JustBelowOne, hit.normal, textures.at(doorData.id),
						shadingInfo, occlusion, frame);
				}
				else if (doorData.type == VoxelData::DoorData::Type::Sliding)
				{
					const Double3 doorTopPoint(
						hit.point.x,
						voxelYReal + voxelHeight,
						hit.point.y);
					const Double3 doorBottomPoint(
						doorTopPoint.x,
						voxelYReal,
						doorTopPoint.z);

					const auto drawRange = SoftwareRenderer::makeDrawRange(
						doorTopPoint, doorBottomPoint, camera, frame);

					SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ, hit.u, 0.0,
						Constants::JustBelowOne, hit.normal, textures.at(doorData.id),
						shadingInfo, occlusion, frame);
				}
				else if (doorData.type == VoxelData::DoorData::Type::Raising)
				{
					// Top point is fixed, bottom point depends on percent open.
					const double minVisible = SoftwareRenderer::DOOR_MIN_VISIBLE;
					const double raisedAmount = (voxelHeight * (1.0 - minVisible)) * percentOpen;

					const Double3 doorTopPoint(
						hit.point.x,
						voxelYReal + voxelHeight,
						hit.point.y);
					const Double3 doorBottomPoint(
						doorTopPoint.x,
						voxelYReal + raisedAmount,
						doorTopPoint.z);

					const auto drawRange = SoftwareRenderer::makeDrawRange(
						doorTopPoint, doorBottomPoint, camera, frame);

					// The start of the vertical texture coordinate depends on the percent open.
					const double vStart = raisedAmount / voxelHeight;

					SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ, hit.u, vStart,
						Constants::JustBelowOne, hit.normal, textures.at(doorData.id), shadingInfo,
						occlusion, frame);
				}
				else if (doorData.type == VoxelData::DoorData::Type::Splitting)
				{
					const Double3 doorTopPoint(
						hit.point.x,
						voxelYReal + voxelHeight,
						hit.point.y);
					const Double3 doorBottomPoint(
						doorTopPoint.x,
						voxelYReal,
						doorTopPoint.z);

					const auto drawRange = SoftwareRenderer::makeDrawRange(
						doorTopPoint, doorBottomPoint, camera, frame);

					SoftwareRenderer::drawTransparentPixels(x, drawRange, nearZ, hit.u, 0.0,
						Constants::JustBelowOne, hit.normal, textures.at(doorData.id),
						shadingInfo, occlusion, frame);
				}
			}
		}
	};

	// Relative Y voxel coordinate of the camera, compensating for the ceiling height.
	const int adjustedVoxelY = camera.getAdjustedEyeVoxelY(ceilingHeight);

	// Draw voxel straight ahead first.
	drawVoxel(adjustedVoxelY);

	// Draw voxels below the voxel.
	for (int voxelY = (adjustedVoxelY - 1); voxelY >= 0; voxelY--)
	{
		drawVoxelBelow(voxelY);
	}

	// Draw voxels above the voxel.
	for (int voxelY = (adjustedVoxelY + 1); voxelY < voxelGrid.getHeight(); voxelY++)
	{
		drawVoxelAbove(voxelY);
	}
}

void SoftwareRenderer::drawFlat(int startX, int endX, const VisibleFlat &flat,
	const Double3 &normal, const Double2 &eye, const ShadingInfo &shadingInfo,
	const FlatTexture &texture, const FrameView &frame)
{
	// Contribution from the sun.
	const double lightNormalDot = std::max(0.0, shadingInfo.sunDirection.dot(normal));
	const Double3 sunComponent = (shadingInfo.sunColor * lightNormalDot).clamped(
		0.0, 1.0 - shadingInfo.ambient);

	// X percents across the screen for the given start and end columns.
	const double startXPercent = (static_cast<double>(startX) + 0.50) / 
		static_cast<double>(frame.width);
	const double endXPercent = (static_cast<double>(endX) + 0.50) /
		static_cast<double>(frame.width);

	const bool startsInRange =
		(flat.startX >= startXPercent) && (flat.startX <= endXPercent);
	const bool endsInRange = 
		(flat.endX >= startXPercent) && (flat.endX <= endXPercent);
	const bool coversRange =
		(flat.startX <= startXPercent) && (flat.endX >= endXPercent);
	
	// Throw out the draw call if the flat is not in the X range.
	if (!startsInRange && !endsInRange && !coversRange)
	{
		return;
	}

	// Get the min and max X range of coordinates in screen-space. This range is completely 
	// contained within the flat.
	const double clampedStartXPercent = std::clamp(startXPercent, flat.startX, flat.endX);
	const double clampedEndXPercent = std::clamp(endXPercent, flat.startX, flat.endX);

	// The percentages from start to end within the flat.
	const double startFlatPercent = (clampedStartXPercent - flat.startX) /
		(flat.endX - flat.startX);
	const double endFlatPercent = (clampedEndXPercent - flat.startX) /
		(flat.endX - flat.startX);

	// Points interpolated between for per-column depth calculations in the XZ plane.
	const Double3 startTopPoint = flat.topLeft.lerp(flat.topRight, startFlatPercent);
	const Double3 endTopPoint = flat.topLeft.lerp(flat.topRight, endFlatPercent);

	// Horizontal texture coordinates in the flat. Although the flat percent can be
	// equal to 1.0, the texture coordinate needs to be less than 1.0.
	const double startU = std::clamp(startFlatPercent, 0.0, Constants::JustBelowOne);
	const double endU = std::clamp(endFlatPercent, 0.0, Constants::JustBelowOne);

	// Get the start and end coordinates of the projected points (Y values potentially
	// outside the screen).
	const double projectedXStart = clampedStartXPercent * frame.widthReal;
	const double projectedXEnd = clampedEndXPercent * frame.widthReal;
	const double projectedYStart = flat.startY * frame.heightReal;
	const double projectedYEnd = flat.endY * frame.heightReal;

	// Clamp the coordinates for where the flat starts and stops on the screen.
	const int xStart = SoftwareRenderer::getLowerBoundedPixel(projectedXStart, frame.width);
	const int xEnd = SoftwareRenderer::getUpperBoundedPixel(projectedXEnd, frame.width);
	const int yStart = SoftwareRenderer::getLowerBoundedPixel(projectedYStart, frame.height);
	const int yEnd = SoftwareRenderer::getUpperBoundedPixel(projectedYEnd, frame.height);

	// Shading on the texture.
	// - @todo: contribution from lights.
	const Double3 shading(
		shadingInfo.ambient + sunComponent.x,
		shadingInfo.ambient + sunComponent.y,
		shadingInfo.ambient + sunComponent.z);

	// Draw by-column, similar to wall rendering.
	for (int x = xStart; x < xEnd; x++)
	{
		const double xPercent = ((static_cast<double>(x) + 0.50) - projectedXStart) /
			(projectedXEnd - projectedXStart);

		// Horizontal texture coordinate.
		const double u = startU + ((endU - startU) * xPercent);

		// Horizontal texel position.
		const int textureX = static_cast<int>(u * static_cast<double>(texture.width));

		const Double3 topPoint = startTopPoint.lerp(endTopPoint, xPercent);

		// Get the true XZ distance for the depth.
		const double depth = (Double2(topPoint.x, topPoint.z) - eye).length();

		// Linearly interpolated fog.
		const Double3 &fogColor = shadingInfo.getFogColor();
		const double fogPercent = std::min(depth / shadingInfo.fogDistance, 1.0);

		for (int y = yStart; y < yEnd; y++)
		{
			const int index = x + (y * frame.width);

			if (depth <= frame.depthBuffer[index])
			{
				const double yPercent = ((static_cast<double>(y) + 0.50) - projectedYStart) /
					(projectedYEnd - projectedYStart);

				// Vertical texture coordinate.
				const double startV = 0.0;
				const double endV = Constants::JustBelowOne;
				const double v = startV + ((endV - startV) * yPercent);

				// Vertical texel position.
				const int textureY = static_cast<int>(v * static_cast<double>(texture.height));

				// Alpha is checked in this loop, and transparent texels are not drawn.
				// Flats do not have emission, so ignore it.
				const int textureIndex = textureX + (textureY * texture.width);
				const FlatTexel &texel = texture.texels[textureIndex];

				if (texel.a > 0.0)
				{
					// Special case (for true color): if texel alpha is between 0 and 1,
					// the previously rendered pixel is diminished by some amount.
					double colorR, colorG, colorB;
					if (texel.a < 1.0)
					{
						// Diminish the previous color in the frame buffer.
						const Double3 prevColor = Double3::fromRGB(frame.colorBuffer[index]);
						const double visPercent = std::clamp(1.0 - texel.a, 0.0, 1.0);
						colorR = prevColor.x * visPercent;
						colorG = prevColor.y * visPercent;
						colorB = prevColor.z * visPercent;
					}
					else
					{
						// Texture color with shading.
						const double shadingMax = 1.0;
						colorR = texel.r * std::min(shading.x, shadingMax);
						colorG = texel.g * std::min(shading.y, shadingMax);
						colorB = texel.b * std::min(shading.z, shadingMax);
					}

					// Linearly interpolate with fog.
					colorR += (fogColor.x - colorR) * fogPercent;
					colorG += (fogColor.y - colorG) * fogPercent;
					colorB += (fogColor.z - colorB) * fogPercent;

					// Clamp maximum (don't worry about negative values).
					const double high = 1.0;
					colorR = (colorR > high) ? high : colorR;
					colorG = (colorG > high) ? high : colorG;
					colorB = (colorB > high) ? high : colorB;

					// Convert floats to integers.
					const uint32_t colorRGB = static_cast<uint32_t>(
						((static_cast<uint8_t>(colorR * 255.0)) << 16) |
						((static_cast<uint8_t>(colorG * 255.0)) << 8) |
						((static_cast<uint8_t>(colorB * 255.0))));

					frame.colorBuffer[index] = colorRGB;
					frame.depthBuffer[index] = depth;
				}
			}
		}
	}
}

void SoftwareRenderer::rayCast2D(int x, const Camera &camera, const Ray &ray,
	const ShadingInfo &shadingInfo, double ceilingHeight,
	const std::vector<LevelData::DoorState> &openDoors,
	const std::vector<LevelData::FadeState> &fadingVoxels, const VoxelGrid &voxelGrid,
	const std::vector<VoxelTexture> &textures, OcclusionData &occlusion, const FrameView &frame)
{
	// Initially based on Lode Vandevenne's algorithm, this method of 2.5D ray casting is more 
	// expensive as it does not stop at the first wall intersection, and it also renders voxels 
	// above and below the current floor.

	// Some floating point behavior assumptions:
	// -> (value / 0.0) == infinity
	// -> (value / infinity) == 0.0
	// -> (int)(-0.8) == 0
	// -> (int)floor(-0.8) == -1
	// -> (int)ceil(-0.8) == 0

	const double dirXSquared = ray.dirX * ray.dirX;
	const double dirZSquared = ray.dirZ * ray.dirZ;

	const double deltaDistX = std::sqrt(1.0 + (dirZSquared / dirXSquared));
	const double deltaDistZ = std::sqrt(1.0 + (dirXSquared / dirZSquared));

	const bool nonNegativeDirX = ray.dirX >= 0.0;
	const bool nonNegativeDirZ = ray.dirZ >= 0.0;

	int stepX, stepZ;
	double sideDistX, sideDistZ;
	if (nonNegativeDirX)
	{
		stepX = 1;
		sideDistX = (camera.eyeVoxelReal.x + 1.0 - camera.eye.x) * deltaDistX;
	}
	else
	{
		stepX = -1;
		sideDistX = (camera.eye.x - camera.eyeVoxelReal.x) * deltaDistX;
	}

	if (nonNegativeDirZ)
	{
		stepZ = 1;
		sideDistZ = (camera.eyeVoxelReal.z + 1.0 - camera.eye.z) * deltaDistZ;
	}
	else
	{
		stepZ = -1;
		sideDistZ = (camera.eye.z - camera.eyeVoxelReal.z) * deltaDistZ;
	}

	// The Z distance from the camera to the wall, and the X or Z normal of the intersected
	// voxel face. The first Z distance is a special case, so it's brought outside the 
	// DDA loop.
	double zDistance;
	VoxelData::Facing facing;

	// Verify that the initial voxel coordinate is within the world bounds.
	bool voxelIsValid = 
		(camera.eyeVoxel.x >= 0) && 
		(camera.eyeVoxel.y >= 0) && 
		(camera.eyeVoxel.z >= 0) &&
		(camera.eyeVoxel.x < voxelGrid.getWidth()) && 
		(camera.eyeVoxel.y < voxelGrid.getHeight()) &&
		(camera.eyeVoxel.z < voxelGrid.getDepth());

	if (voxelIsValid)
	{
		// Decide how far the wall is, and which voxel face was hit.
		if (sideDistX < sideDistZ)
		{
			zDistance = sideDistX;
			facing = nonNegativeDirX ? VoxelData::Facing::NegativeX : 
				VoxelData::Facing::PositiveX;
		}
		else
		{
			zDistance = sideDistZ;
			facing = nonNegativeDirZ ? VoxelData::Facing::NegativeZ : 
				VoxelData::Facing::PositiveZ;
		}

		// The initial near point is directly in front of the player in the near Z 
		// camera plane.
		const Double2 initialNearPoint(
			camera.eye.x + (ray.dirX * SoftwareRenderer::NEAR_PLANE),
			camera.eye.z + (ray.dirZ * SoftwareRenderer::NEAR_PLANE));

		// The initial far point is the wall hit. This is used with the player's position 
		// for drawing the initial floor and ceiling.
		const Double2 initialFarPoint(
			camera.eye.x + (ray.dirX * zDistance),
			camera.eye.z + (ray.dirZ * zDistance));

		// Draw all voxels in a column at the player's XZ coordinate.
		SoftwareRenderer::drawInitialVoxelColumn(x, camera.eyeVoxel.x, camera.eyeVoxel.z,
			camera, ray, facing, initialNearPoint, initialFarPoint, SoftwareRenderer::NEAR_PLANE, 
			zDistance, shadingInfo, ceilingHeight, openDoors, fadingVoxels, voxelGrid, textures,
			occlusion, frame);
	}

	// The current voxel coordinate in the DDA loop. For all intents and purposes,
	// the Y cell coordinate is constant.
	Int3 cell(camera.eyeVoxel.x, camera.eyeVoxel.y, camera.eyeVoxel.z);

	// Lambda for stepping to the next XZ coordinate in the grid and updating the Z
	// distance for the current edge point.
	auto doDDAStep = [&camera, &ray, &voxelGrid, &sideDistX, &sideDistZ, &cell,
		&facing, &voxelIsValid, &zDistance, deltaDistX, deltaDistZ, stepX, stepZ,
		nonNegativeDirX, nonNegativeDirZ]()
	{
		if (sideDistX < sideDistZ)
		{
			sideDistX += deltaDistX;
			cell.x += stepX;
			facing = nonNegativeDirX ? VoxelData::Facing::NegativeX : 
				VoxelData::Facing::PositiveX;
			voxelIsValid &= (cell.x >= 0) && (cell.x < voxelGrid.getWidth());
		}
		else
		{
			sideDistZ += deltaDistZ;
			cell.z += stepZ;
			facing = nonNegativeDirZ ? VoxelData::Facing::NegativeZ : 
				VoxelData::Facing::PositiveZ;
			voxelIsValid &= (cell.z >= 0) && (cell.z < voxelGrid.getDepth());
		}

		const bool onXAxis = (facing == VoxelData::Facing::PositiveX) || 
			(facing == VoxelData::Facing::NegativeX);

		// Update the Z distance depending on which axis was stepped with.
		if (onXAxis)
		{
			zDistance = (static_cast<double>(cell.x) - 
				camera.eye.x + static_cast<double>((1 - stepX) / 2)) / ray.dirX;
		}
		else
		{
			zDistance = (static_cast<double>(cell.z) -
				camera.eye.z + static_cast<double>((1 - stepZ) / 2)) / ray.dirZ;
		}
	};

	// Step forward in the grid once to leave the initial voxel and update the Z distance.
	doDDAStep();

	// Step through the voxel grid while the current coordinate is valid, the 
	// distance stepped is less than the distance at which fog is maximum, and
	// the column is not completely occluded.
	while (voxelIsValid && (zDistance < shadingInfo.fogDistance) && 
		(occlusion.yMin != occlusion.yMax))
	{
		// Store the cell coordinates, axis, and Z distance for wall rendering. The
		// loop needs to do another DDA step to calculate the far point.
		const int savedCellX = cell.x;
		const int savedCellZ = cell.z;
		const VoxelData::Facing savedFacing = facing;
		const double wallDistance = zDistance;

		// Decide which voxel in the XZ plane to step to next, and update the Z distance.
		doDDAStep();

		// Near and far points in the XZ plane. The near point is where the wall is, and 
		// the far point is used with the near point for drawing the floor and ceiling.
		const Double2 nearPoint(
			camera.eye.x + (ray.dirX * wallDistance),
			camera.eye.z + (ray.dirZ * wallDistance));
		const Double2 farPoint(
			camera.eye.x + (ray.dirX * zDistance),
			camera.eye.z + (ray.dirZ * zDistance));

		// Draw all voxels in a column at the given XZ coordinate.
		SoftwareRenderer::drawVoxelColumn(x, savedCellX, savedCellZ, camera, ray, savedFacing,
			nearPoint, farPoint, wallDistance, zDistance, shadingInfo, ceilingHeight, 
			openDoors, fadingVoxels, voxelGrid, textures, occlusion, frame);
	}
}

void SoftwareRenderer::drawSkyGradient(int startY, int endY, double gradientProjYTop,
	double gradientProjYBottom, std::vector<Double3> &skyGradientRowCache,
	std::atomic<bool> &shouldDrawStars, const ShadingInfo &shadingInfo, const FrameView &frame)
{
	// Lambda for drawing one row of colors and depth in the frame buffer.
	auto drawSkyRow = [&frame](int y, const Double3 &color)
	{
		uint32_t *colorPtr = frame.colorBuffer;
		double *depthPtr = frame.depthBuffer;
		const int startIndex = y * frame.width;
		const int endIndex = (y + 1) * frame.width;
		const uint32_t colorValue = color.toRGB();
		constexpr double depthValue = std::numeric_limits<double>::infinity();

		// Clear the color and depth of one row.
		for (int i = startIndex; i < endIndex; i++)
		{
			colorPtr[i] = colorValue;
			depthPtr[i] = depthValue;
		}
	};

	// While drawing the sky gradient, determine if it is dark enough for stars to be visible.
	bool isDarkEnough = false;

	for (int y = startY; y < endY; y++)
	{
		// Y percent across the screen.
		const double yPercent = (static_cast<double>(y) + 0.50) / frame.heightReal;

		// Y percent within the sky gradient.
		const double gradientPercent = SoftwareRenderer::getSkyGradientPercent(
			yPercent, gradientProjYTop, gradientProjYBottom);

		// Color of the sky gradient at the given percentage.
		const Double3 color = SoftwareRenderer::getSkyGradientRowColor(
			gradientPercent, shadingInfo);

		// Cache row color for star rendering.
		skyGradientRowCache.at(y) = color;

		// Update star visibility.
		const double maxComp = std::max(std::max(color.x, color.y), color.z);
		isDarkEnough |= maxComp <= ShadingInfo::STAR_VIS_THRESHOLD;

		drawSkyRow(y, color);
	}

	if (isDarkEnough)
	{
		shouldDrawStars = true;
	}
}

void SoftwareRenderer::drawDistantSky(int startX, int endX, bool parallaxSky, 
	const VisDistantObjects &visDistantObjs, const std::vector<SkyTexture> &skyTextures,
	const std::vector<Double3> &skyGradientRowCache, bool shouldDrawStars,
	const ShadingInfo &shadingInfo, const FrameView &frame)
{
	enum class DistantRenderType { General, Moon, Star };

	// For each visible distant object, if it is at least partially within the start and end
	// X, then draw.
	auto drawDistantObj = [startX, endX, parallaxSky, &skyTextures, &skyGradientRowCache,
		&shadingInfo, &frame](const VisDistantObject &obj, DistantRenderType renderType)
	{
		const SkyTexture &texture = *obj.texture;
		const DrawRange &drawRange = obj.drawRange;
		const double xProjStart = obj.xProjStart;
		const double xProjEnd = obj.xProjEnd;
		const int xDrawStart = std::max(obj.xStart, startX);
		const int xDrawEnd = std::min(obj.xEnd, endX);
		const bool emissive = obj.emissive;

		if (parallaxSky)
		{
			// Parallax rendering. Render the object based on its left and right edges.

			// @todo: see if these are necessary.
			//const double xAngleStart = obj.xAngleStart;
			//const double xAngleEnd = obj.xAngleEnd;

			for (int x = xDrawStart; x < xDrawEnd; x++)
			{
				// Percent X across the screen.
				const double xPercent = (static_cast<double>(x) + 0.50) / frame.widthReal;

				// Percentage across the horizontal span of the object in screen space.
				const double widthPercent = std::clamp(
					(xPercent - xProjStart) / (xProjEnd - xProjStart),
					0.0, Constants::JustBelowOne);

				// Horizontal texture coordinate, accounting for parallax.
				// @todo: incorporate angle/field of view/delta angle from center of view into this.
				const double u = widthPercent;

				if (renderType == DistantRenderType::General)
				{
					SoftwareRenderer::drawDistantPixels(x, drawRange, u, 0.0,
						Constants::JustBelowOne, texture, emissive, shadingInfo, frame);
				}
				else if (renderType == DistantRenderType::Moon)
				{
					SoftwareRenderer::drawMoonPixels(x, drawRange, u, 0.0, Constants::JustBelowOne,
						texture, shadingInfo, frame);
				}
				else if (renderType == DistantRenderType::Star)
				{
					SoftwareRenderer::drawStarPixels(x, drawRange, u, 0.0, Constants::JustBelowOne,
						texture, skyGradientRowCache, shadingInfo, frame);
				}
			}
		}
		else
		{
			// Classic rendering. Render the object based on its midpoint.
			for (int x = xDrawStart; x < xDrawEnd; x++)
			{
				// Percent X across the screen.
				const double xPercent = (static_cast<double>(x) + 0.50) / frame.widthReal;

				// Percentage across the horizontal span of the object in screen space.
				const double widthPercent = std::clamp(
					(xPercent - xProjStart) / (xProjEnd - xProjStart),
					0.0, Constants::JustBelowOne);

				// Horizontal texture coordinate, not accounting for parallax.
				const double u = widthPercent;

				if (renderType == DistantRenderType::General)
				{
					SoftwareRenderer::drawDistantPixels(x, drawRange, u, 0.0,
						Constants::JustBelowOne, texture, emissive, shadingInfo, frame);
				}
				else if (renderType == DistantRenderType::Moon)
				{
					SoftwareRenderer::drawMoonPixels(x, drawRange, u, 0.0, Constants::JustBelowOne,
						texture, shadingInfo, frame);
				}
				else if (renderType == DistantRenderType::Star)
				{
					SoftwareRenderer::drawStarPixels(x, drawRange, u, 0.0, Constants::JustBelowOne,
						texture, skyGradientRowCache, shadingInfo, frame);
				}
			}
		}
	};

	// Lambda for drawing a group of distant objects with some render type.
	auto drawDistantObjRange = [&visDistantObjs, &drawDistantObj](int start, int end,
		DistantRenderType renderType)
	{
		DebugAssert(start >= 0);
		DebugAssert(end <= visDistantObjs.objs.size());

		// Reverse iterate so objects are drawn far to near.
		const VisDistantObject *objs = visDistantObjs.objs.data();
		for (int i = end - 1; i >= start; i--)
		{
			drawDistantObj(objs[i], renderType);
		}
	};

	// Stars are only drawn when the sky gradient is dark enough. This saves on performance during
	// the daytime.
	if (shouldDrawStars)
	{
		drawDistantObjRange(visDistantObjs.starStart, visDistantObjs.starEnd, DistantRenderType::Star);
	}

	drawDistantObjRange(visDistantObjs.sunStart, visDistantObjs.sunEnd, DistantRenderType::General);
	drawDistantObjRange(visDistantObjs.moonStart, visDistantObjs.moonEnd, DistantRenderType::Moon);
	drawDistantObjRange(visDistantObjs.airStart, visDistantObjs.airEnd, DistantRenderType::General);
	drawDistantObjRange(visDistantObjs.animLandStart, visDistantObjs.animLandEnd, DistantRenderType::General);
	drawDistantObjRange(visDistantObjs.landStart, visDistantObjs.landEnd, DistantRenderType::General);
}

void SoftwareRenderer::drawVoxels(int startX, int stride, const Camera &camera,
	double ceilingHeight, const std::vector<LevelData::DoorState> &openDoors,
	const std::vector<LevelData::FadeState> &fadingVoxels, const VoxelGrid &voxelGrid,
	const std::vector<VoxelTexture> &voxelTextures, std::vector<OcclusionData> &occlusion,
	const ShadingInfo &shadingInfo, const FrameView &frame)
{
	const Double2 forwardZoomed(camera.forwardZoomedX, camera.forwardZoomedZ);
	const Double2 rightAspected(camera.rightAspectedX, camera.rightAspectedZ);

	// Draw pixel columns with spacing determined by the number of render threads.
	for (int x = startX; x < frame.width; x += stride)
	{
		// X percent across the screen.
		const double xPercent = (static_cast<double>(x) + 0.50) / frame.widthReal;

		// "Right" component of the ray direction, based on current screen X.
		const Double2 rightComp = rightAspected * ((2.0 * xPercent) - 1.0);

		// Calculate the ray direction through the pixel.
		// - If un-normalized, it uses the Z distance, but the insides of voxels
		//   don't look right then.
		const Double2 direction = (forwardZoomed + rightComp).normalized();
		const Ray ray(direction.x, direction.y);

		// Cast the 2D ray and fill in the column's pixels with color.
		SoftwareRenderer::rayCast2D(x, camera, ray, shadingInfo, ceilingHeight, openDoors,
			fadingVoxels, voxelGrid, voxelTextures, occlusion.at(x), frame);
	}
}

void SoftwareRenderer::drawFlats(int startX, int endX, const Camera &camera,
	const Double3 &flatNormal, const std::vector<VisibleFlat> &visibleFlats,
	const std::unordered_map<int, FlatTextureGroup> &flatTextureGroups,
	const ShadingInfo &shadingInfo, const FrameView &frame)
{
	// Iterate through all flats, rendering those visible within the given X range of 
	// the screen.
	for (const VisibleFlat &flat : visibleFlats)
	{
		// Texture of the flat. It might be flipped horizontally as well, given by
		// the "flat.flipped" value.
		const int flatIndex = flat.flatIndex;
		const auto iter = flatTextureGroups.find(flatIndex);
		if (iter == flatTextureGroups.end())
		{
			// No flat texture group available for the flat.
			continue;
		}

		const FlatTextureGroup &flatTextureGroup = iter->second;
		const FlatTextureGroup::TextureList *textureList =
			flatTextureGroup.getTextureList(flat.animStateType, flat.anglePercent);

		if (textureList == nullptr)
		{
			// No flat textures allocated for the animation state.
			continue;
		}

		const FlatTexture &texture = (*textureList)[flat.textureID];
		const Double2 eye2D(camera.eye.x, camera.eye.z);

		SoftwareRenderer::drawFlat(startX, endX, flat, flatNormal, eye2D,
			shadingInfo, texture, frame);
	}
}

void SoftwareRenderer::renderThreadLoop(RenderThreadData &threadData, int threadIndex, int startX,
	int endX, int startY, int endY)
{
	while (true)
	{
		// Initial wait condition. The lock must be unlocked after wait() so other threads can
		// lock it.
		std::unique_lock<std::mutex> lk(threadData.mutex);
		threadData.condVar.wait(lk, [&threadData]() { return threadData.go; });
		lk.unlock();

		// Received a go signal. Check if the renderer is being destroyed before doing anything.
		if (threadData.isDestructing)
		{
			break;
		}

		// Lambda for making a thread wait until others are finished rendering something. The last
		// thread to call this calls notify on all others.
		auto threadBarrier = [&threadData, &lk](auto &data)
		{
			lk.lock();
			data.threadsDone++;

			// If this was the last thread, notify all to continue.
			if (data.threadsDone == threadData.totalThreads)
			{
				lk.unlock();
				threadData.condVar.notify_all();
			}
			else
			{
				// Wait for other threads to finish.
				threadData.condVar.wait(lk, [&threadData, &data]()
				{
					return data.threadsDone == threadData.totalThreads;
				});

				lk.unlock();
			}
		};

		// Draw this thread's portion of the sky gradient.
		RenderThreadData::SkyGradient &skyGradient = threadData.skyGradient;
		SoftwareRenderer::drawSkyGradient(startY, endY, skyGradient.projectedYTop,
			skyGradient.projectedYBottom, *skyGradient.rowCache, skyGradient.shouldDrawStars,
			*threadData.shadingInfo, *threadData.frame);

		// Wait for other threads to finish the sky gradient.
		threadBarrier(skyGradient);

		// Wait for the visible distant object testing to finish.
		RenderThreadData::DistantSky &distantSky = threadData.distantSky;
		lk.lock();
		threadData.condVar.wait(lk, [&distantSky]() { return distantSky.doneVisTesting; });
		lk.unlock();

		// Draw this thread's portion of distant sky objects.
		SoftwareRenderer::drawDistantSky(startX, endX, distantSky.parallaxSky,
			*distantSky.visDistantObjs, *distantSky.skyTextures, *skyGradient.rowCache,
			skyGradient.shouldDrawStars, *threadData.shadingInfo, *threadData.frame);

		// Wait for other threads to finish distant sky objects.
		threadBarrier(distantSky);

		// Number of columns to skip per ray cast (for interleaved ray casting as a means of
		// load-balancing).
		const int strideX = threadData.totalThreads;

		// Draw this thread's portion of voxels.
		RenderThreadData::Voxels &voxels = threadData.voxels;
		SoftwareRenderer::drawVoxels(threadIndex, strideX, *threadData.camera,
			voxels.ceilingHeight, *voxels.openDoors, *voxels.fadingVoxels, *voxels.voxelGrid,
			*voxels.voxelTextures, *voxels.occlusion, *threadData.shadingInfo, *threadData.frame);

		// Wait for other threads to finish voxels.
		threadBarrier(voxels);

		// Wait for the visible flat sorting to finish.
		RenderThreadData::Flats &flats = threadData.flats;
		lk.lock();
		threadData.condVar.wait(lk, [&flats]() { return flats.doneSorting; });
		lk.unlock();

		// Draw this thread's portion of flats.
		SoftwareRenderer::drawFlats(startX, endX, *threadData.camera, *flats.flatNormal,
			*flats.visibleFlats, *flats.flatTextureGroups, *threadData.shadingInfo, *threadData.frame);

		// Wait for other threads to finish flats.
		threadBarrier(flats);
	}
}

void SoftwareRenderer::render(const Double3 &eye, const Double3 &direction, double fovY,
	double ambient, double daytimePercent, double latitude, bool parallaxSky, double ceilingHeight,
	const std::vector<LevelData::DoorState> &openDoors,
	const std::vector<LevelData::FadeState> &fadingVoxels, const VoxelGrid &voxelGrid,
	const EntityManager &entityManager, uint32_t *colorBuffer)
{
	// Constants for screen dimensions.
	const double widthReal = static_cast<double>(this->width);
	const double heightReal = static_cast<double>(this->height);
	const double aspect = widthReal / heightReal;

	// To account for tall pixels.
	const double projectionModifier = SoftwareRenderer::TALL_PIXEL_RATIO;

	// 2.5D camera definition.
	const Camera camera(eye, direction, fovY, aspect, projectionModifier);

	// Normal of all flats (always facing the camera).
	const Double3 flatNormal = Double3(-camera.forwardX, 0.0, -camera.forwardZ).normalized();

	// Calculate shading information for this frame. Create some helper structs to keep similar
	// values together.
	const ShadingInfo shadingInfo(this->skyPalette, daytimePercent, latitude,
		ambient, this->fogDistance);
	const FrameView frame(colorBuffer, this->depthBuffer.data(), this->width, this->height);

	// Projected Y range of the sky gradient.
	double gradientProjYTop, gradientProjYBottom;
	SoftwareRenderer::getSkyGradientProjectedYRange(camera, gradientProjYTop, gradientProjYBottom);

	// Set all the render-thread-specific shared data for this frame.
	this->threadData.init(static_cast<int>(this->renderThreads.size()),
		camera, shadingInfo, frame);
	this->threadData.skyGradient.init(gradientProjYTop, gradientProjYBottom,
		this->skyGradientRowCache);
	this->threadData.distantSky.init(parallaxSky, this->visDistantObjs, this->skyTextures);
	this->threadData.voxels.init(ceilingHeight, openDoors, fadingVoxels, voxelGrid,
		this->voxelTextures, this->occlusion);
	this->threadData.flats.init(flatNormal, this->visibleFlats, this->flatTextureGroups);

	// Give the render threads the go signal. They can work on the sky and voxels while this thread
	// does things like resetting occlusion and doing visible flat determination.
	// - Note about locks: they must always be locked before wait(), and stay locked after wait().
	std::unique_lock<std::mutex> lk(this->threadData.mutex);
	this->threadData.go = true;
	lk.unlock();
	this->threadData.condVar.notify_all();

	// Reset occlusion. Don't need to reset sky gradient row cache because it is written to before
	// it is read.
	std::fill(this->occlusion.begin(), this->occlusion.end(), OcclusionData(0, this->height));

	// Refresh the visible distant objects.
	this->updateVisibleDistantObjects(parallaxSky, shadingInfo, camera, frame);

	lk.lock();
	this->threadData.condVar.wait(lk, [this]()
	{
		return this->threadData.skyGradient.threadsDone == this->threadData.totalThreads;
	});

	// Keep the render threads from getting the go signal again before the next frame.
	this->threadData.go = false;

	// Let the render threads know that they can start drawing distant objects.
	this->threadData.distantSky.doneVisTesting = true;

	lk.unlock();
	this->threadData.condVar.notify_all();

	// Refresh the visible flats. This should erase the old list, calculate a new list, and sort
	// it by depth.
	this->updateVisibleFlats(camera, ceilingHeight, fogDistance, voxelGrid, entityManager);

	lk.lock();
	this->threadData.condVar.wait(lk, [this]()
	{
		return this->threadData.voxels.threadsDone == this->threadData.totalThreads;
	});

	// Let the render threads know that they can start drawing flats.
	this->threadData.flats.doneSorting = true;

	lk.unlock();
	this->threadData.condVar.notify_all();

	// Wait until render threads are done drawing flats.
	lk.lock();
	this->threadData.condVar.wait(lk, [this]()
	{
		return this->threadData.flats.threadsDone == this->threadData.totalThreads;
	});
}
