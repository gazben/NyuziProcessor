// 
// Copyright 2011-2015 Jeff Bush
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// 


#include <assert.h>
#include <stdio.h>
#include <math.h>
#include "Texture.h"
#include "Shader.h"

using namespace librender;

namespace {
	
// Convert a 32-bit RGBA color (packed in an integer) into four floating point (0.0 - 1.0) 
// color channels.
void unpackRGBA(veci16_t packedColor, vecf16_t outColor[3])
{
	outColor[kColorR] = __builtin_convertvector(packedColor & splati(255), vecf16_t)
		/ splatf(255.0f);
	outColor[kColorG] = __builtin_convertvector((packedColor >> splati(8)) & splati(255),
		vecf16_t) / splatf(255.0f);
	outColor[kColorB] = __builtin_convertvector((packedColor >> splati(16)) & splati(255),
		vecf16_t) / splatf(255.0f);
	outColor[kColorA] = __builtin_convertvector((packedColor >> splati(24)) & splati(255),
		vecf16_t) / splatf(255.0f);
}

}

Texture::Texture()
{
	for (int i = 0; i < kMaxMipLevels; i++)
		fMipSurfaces[i] = nullptr;
}

void Texture::setMipSurface(int mipLevel, const Surface *surface)
{
	assert(mipLevel < kMaxMipLevels);

	fMipSurfaces[mipLevel] = surface;
	if (mipLevel > fMaxMipLevel)
		fMaxMipLevel = mipLevel;

	if (mipLevel == 0)
	{
		fBaseMipBits = __builtin_clz(surface->getWidth()) + 1;
		
		// Clear out lower mip levels
		for (int i = 1; i < fMaxMipLevel; i++)
			fMipSurfaces[i] = 0;
		
		fMaxMipLevel = 0;
	}
	else
	{
		assert(surface->getWidth() == fMipSurfaces[0]->getWidth() >> mipLevel);
	}
}

void Texture::readPixels(vecf16_t u, vecf16_t v, unsigned short mask,
	vecf16_t outColor[4]) const
{
	// Determine the closest mip-level. Determine the pitch between the top
	// two pixels. The reciprocal of this is the scaled texture size. log2 of this
	// is the mip level.
	int mipLevel = __builtin_clz(int(1.0f / __builtin_fabsf(u[1] - u[0]))) - fBaseMipBits;
	if (mipLevel > fMaxMipLevel)
		mipLevel = fMaxMipLevel;
	else if (mipLevel < 0)
		mipLevel = 0;

	const Surface *surface = fMipSurfaces[mipLevel];
	int mipWidth = surface->getWidth();
	int mipHeight = surface->getHeight();

	// Convert from texture space (0.0-1.0, 1.0-0.0) to raster coordinates 
	// (0-(width - 1), 0-(height - 1)). Note that the top of the texture corresponds
	// to v of 1.0. Coordinates will wrap.
	// XXX when a coordinate goes negative, this won't work correctly.
	// the absfv is quick and dirty.
	vecf16_t uRaster = absfv(fracfv(u)) * splatf(mipWidth - 1);
	vecf16_t vRaster = (splatf(1.0) - absfv(fracfv(v))) * splatf(mipHeight - 1);
	veci16_t tx = __builtin_convertvector(uRaster, veci16_t);
	veci16_t ty = __builtin_convertvector(vRaster, veci16_t);

	if (fEnableBilinearFiltering)
	{
		// Load four overlapping pixels
		vecf16_t tlColor[4];	// top left
		vecf16_t trColor[4];	// top right
		vecf16_t blColor[4];	// bottom left
		vecf16_t brColor[4];	// bottom right

		// XXX these calculations do not repeat correctly for the outer pixels; 
		// they will go past the edge, wrapping to the next row or past the
		// bottom of the surface.
		unpackRGBA(surface->readPixels(tx, ty, mask), tlColor);
		unpackRGBA(surface->readPixels(tx, ty + splati(1), mask), blColor);
		unpackRGBA(surface->readPixels(tx + splati(1), ty, mask), trColor);
		unpackRGBA(surface->readPixels(tx + splati(1), ty + splati(1), mask), brColor);

		// Compute weights
		vecf16_t wu = fracfv(uRaster);
		vecf16_t wv = fracfv(vRaster);
		vecf16_t tlWeight = (splatf(1.0) - wu) * (splatf(1.0) - wv);
		vecf16_t trWeight = wu * (splatf(1.0) - wv);
		vecf16_t blWeight = (splatf(1.0) - wu) * wv;
		vecf16_t brWeight = wu * wv;

		// Apply weights & blend
		for (int channel = 0; channel < 4; channel++)
		{
			outColor[channel] = (tlColor[channel] * tlWeight) 
				+ (blColor[channel] * blWeight)
				+ (trColor[channel] * trWeight) 
				+ (brColor[channel] * brWeight);
		}
	}
	else
	{
		// Nearest neighbor
		unpackRGBA(surface->readPixels(tx, ty, mask), outColor);
	}
}

