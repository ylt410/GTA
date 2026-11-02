#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *PGDecodeBC(int format,uint32_t width,uint32_t height,uint32_t depth,uint32_t rowPitch,uint32_t rowsPerImage,const void *source,size_t sourceSize,size_t *decodedSize);
// Validate the compressed region and report its tightly packed decoded size.
// No allocation or destination writes occur.
int PGDecodeBCOutputSize(int format,uint32_t width,uint32_t height,uint32_t depth,uint32_t rowPitch,uint32_t rowsPerImage,const void *source,size_t sourceSize,size_t *decodedSize);
// Decode into caller-owned storage. Returns nonzero on success; decodedSize is
// zeroed on entry and set only after the full tightly packed image is written.
int PGDecodeBCInto(int format,uint32_t width,uint32_t height,uint32_t depth,uint32_t rowPitch,uint32_t rowsPerImage,const void *source,size_t sourceSize,void *destination,size_t destinationCapacity,size_t *decodedSize);
// Original decoder used only by the explicit benchmark reference option.
void *PGDecodeBCReference(int format,uint32_t width,uint32_t height,uint32_t depth,uint32_t rowPitch,uint32_t rowsPerImage,const void *source,size_t sourceSize,size_t *decodedSize);
#ifdef __cplusplus
}
#endif
