#include "BCTextureDecoder.h"
#include <stdlib.h>
#include <string.h>
#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#include "../Tools/bcdec/bcdec.h"
#include "BCTextureDecoderReference.inc"

static void decodeBlock(int format,const void *compressed,void *destination,int pitch){
  switch(format){
    case 1:bcdec_bc1(compressed,destination,pitch);break;
    case 2:bcdec_bc2(compressed,destination,pitch);break;
    case 3:bcdec_bc3(compressed,destination,pitch);break;
    case 4:bcdec_bc4(compressed,destination,pitch);break;
    case 5:bcdec_bc5(compressed,destination,pitch);break;
    case 7:bcdec_bc7(compressed,destination,pitch);break;
  }
}

typedef struct {
  size_t pixelBytes,destinationPitch,destinationSlice,outputBytes,blockBytes,sourceRowPitch,sourceSlice;
  uint32_t blocksX,blocksY;
} BCLayout;

static int bcLayout(int format,uint32_t width,uint32_t height,uint32_t depth,uint32_t rowPitch,uint32_t rowsPerImage,
                    const void *source,size_t sourceSize,BCLayout *layout){
  if(format<1||format>7||!width||!height||!depth||!source||!layout)return 0;
  size_t pixelBytes=format==4?1:format==5?2:format==6?8:4;
  const size_t limit=256*1024*1024;
  if(width>limit/pixelBytes)return 0;
  size_t destinationPitch=(size_t)width*pixelBytes;
  if(height>limit/destinationPitch)return 0;
  size_t destinationSlice=destinationPitch*height;
  if(depth>limit/destinationSlice)return 0;
  size_t outputBytes=destinationSlice*depth;
  uint32_t blocksX=width/4+(width%4!=0),blocksY=height/4+(height%4!=0);
  size_t blockBytes=format==1||format==4?8:16;
  size_t blockRowBytes=(size_t)blocksX*blockBytes;
  if(!rowPitch)rowPitch=(uint32_t)blockRowBytes;
  if(rowPitch<blockRowBytes||(size_t)(blocksY-1)>(SIZE_MAX-blockRowBytes)/rowPitch)return 0;
  size_t rowEnd=(size_t)(blocksY-1)*rowPitch+blockRowBytes;
  if(rowEnd>sourceSize)return 0;
  size_t sourceRows=rowsPerImage?rowsPerImage:blocksY;
  if(sourceRows<blocksY)return 0;
  if(sourceRows&&(size_t)rowPitch>SIZE_MAX/sourceRows)return 0;
  size_t sourceSlice=(size_t)rowPitch*sourceRows;
  // Preserve the original tightly packed depth fallback, without overflow.
  if(depth>1&&sourceSlice>(sourceSize-rowEnd)/(depth-1))sourceSlice=sourceSize/depth;
  if(depth>1&&sourceSlice>(sourceSize-rowEnd)/(depth-1))return 0;
  *layout=(BCLayout){pixelBytes,destinationPitch,destinationSlice,outputBytes,blockBytes,rowPitch,sourceSlice,blocksX,blocksY};
  return 1;
}

int PGDecodeBCOutputSize(int format,uint32_t width,uint32_t height,uint32_t depth,uint32_t rowPitch,uint32_t rowsPerImage,const void *source,size_t sourceSize,size_t *decodedSize){
  if(decodedSize)*decodedSize=0;
  BCLayout layout;
  if(!decodedSize||!bcLayout(format,width,height,depth,rowPitch,rowsPerImage,source,sourceSize,&layout))return 0;
  *decodedSize=layout.outputBytes;
  return 1;
}

int PGDecodeBCInto(int format,uint32_t width,uint32_t height,uint32_t depth,uint32_t rowPitch,uint32_t rowsPerImage,const void *source,size_t sourceSize,void *destination,size_t destinationCapacity,size_t *decodedSize){
  if(decodedSize)*decodedSize=0;
  BCLayout layout;
  if(!decodedSize||!destination||!bcLayout(format,width,height,depth,rowPitch,rowsPerImage,source,sourceSize,&layout)||destinationCapacity<layout.outputBytes)return 0;
  size_t pixelBytes=layout.pixelBytes,destinationPitch=layout.destinationPitch,destinationSlice=layout.destinationSlice;
  size_t blockBytes=layout.blockBytes,sourceRowPitch=layout.sourceRowPitch,slicePitch=layout.sourceSlice;
  uint32_t blocksX=layout.blocksX,blocksY=layout.blocksY;
  size_t bytes=layout.outputBytes;
  uint8_t *decoded=destination;
  for(uint32_t z=0;z<depth;z++)for(uint32_t by=0;by<blocksY;by++)for(uint32_t bx=0;bx<blocksX;bx++){
    const uint8_t *compressed=(const uint8_t *)source+z*slicePitch+by*sourceRowPitch+bx*blockBytes;
    uint8_t *destination=decoded+z*destinationSlice+(size_t)by*4*destinationPitch+(size_t)bx*4*pixelBytes;
    uint32_t rows=height-by*4;if(rows>4)rows=4;
    uint32_t columns=width-bx*4;if(columns>4)columns=4;
    if(format!=6&&rows==4&&columns==4){
      // bcdec accepts the complete texture's byte pitch. Interior blocks can
      // write directly, avoiding a scratch block and four separate copies.
      decodeBlock(format,compressed,destination,(int)destinationPitch);
    }else{
      _Alignas(16) uint8_t block[128];
      if(format==6){uint16_t rgb[48];bcdec_bc6h_half(compressed,rgb,12,0);uint16_t *rgba=(uint16_t *)block;for(int p=0;p<16;p++){memcpy(rgba+p*4,rgb+p*3,6);rgba[p*4+3]=0x3c00;}}
      else decodeBlock(format,compressed,block,(int)(4*pixelBytes));
      for(uint32_t y=0;y<rows;y++)memcpy(destination+y*destinationPitch,block+y*4*pixelBytes,columns*pixelBytes);
    }
  }
  *decodedSize=bytes;return 1;
}

void *PGDecodeBC(int format,uint32_t width,uint32_t height,uint32_t depth,uint32_t rowPitch,uint32_t rowsPerImage,const void *source,size_t sourceSize,size_t *decodedSize){
  if(decodedSize)*decodedSize=0;
  size_t bytes=0;
  if(!decodedSize||!PGDecodeBCOutputSize(format,width,height,depth,rowPitch,rowsPerImage,source,sourceSize,&bytes))return NULL;
  void *decoded=malloc(bytes);
  if(!decoded)return NULL;
  if(!PGDecodeBCInto(format,width,height,depth,rowPitch,rowsPerImage,source,sourceSize,decoded,bytes,decodedSize)){
    free(decoded);return NULL;
  }
  return decoded;
}
