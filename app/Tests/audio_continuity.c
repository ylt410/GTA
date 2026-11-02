#include "NativeAudioContinuity.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void near(float a, float b) { assert(fabsf(a-b) < 0.000001f); }
int main(void) {
  float ring[512], left[512], right[512];
  PGAudioContinuity state = {0};
  for (unsigned i=0;i<256;i++) { ring[i*2]=(float)i/256; ring[i*2+1]=-(float)i/256; }
  // Ordinary PCM stays exact, including ring wrap and uint32 counter wrap.
  const uint32_t starts[]={0,240,UINT32_MAX-15};
  for(unsigned s=0;s<3;s++) {
    pg_audio_continuity_reset(&state);
    pg_audio_continuity_render(&state,ring,256,starts[s],128,left,right,128);
    for(unsigned i=0;i<128;i++) {
      assert(!memcmp(&left[i],&ring[((starts[s]+i)&255)*2],4));
      assert(!memcmp(&right[i],&ring[((starts[s]+i)&255)*2+1],4));
    }
  }
  // Gap fade spans callback boundaries. No repeated click every empty callback.
  pg_audio_continuity_reset(&state);
  for(unsigned i=0;i<256;i++) { ring[i*2]=1; ring[i*2+1]=-0.5f; }
  pg_audio_continuity_render(&state,ring,256,0,5,left,right,13);
  for(unsigned i=0;i<5;i++) { near(left[i],1); near(right[i],-0.5f); }
  for(unsigned i=5;i<13;i++) near(left[i],1-(float)(i-4)/32);
  pg_audio_continuity_render(&state,ring,256,5,0,left,right,24);
  near(left[0],23.0f/32); near(left[23],0);
  pg_audio_continuity_render(&state,ring,256,5,0,left,right,512);
  for(unsigned i=0;i<512;i++) { near(left[i],0); near(right[i],0); }
  // Resume edges span short buffers, then stop modifying the stream entirely.
  pg_audio_continuity_render(&state,ring,256,5,7,left,right,7);
  near(left[0],1.0f/32); near(left[6],7.0f/32);
  pg_audio_continuity_render(&state,ring,256,12,40,left,right,40);
  near(left[0],8.0f/32); near(left[24],1); near(left[39],1);
  assert(!state.edge&&!state.gap);
  // A short interruption resumes from its tail, never back to zero.
  pg_audio_continuity_render(&state,ring,256,52,0,left,right,3);
  near(left[2],29.0f/32);
  pg_audio_continuity_render(&state,ring,256,52,1,left,right,1);
  near(left[0],29.0f/32 + (3.0f/32)/32);
  float resumed=left[0];
  pg_audio_continuity_render(&state,ring,256,53,0,left,right,1);
  near(left[0],resumed*31.0f/32);
  // Empty startup remains silent and never reads a nonexistent producer frame.
  pg_audio_continuity_reset(&state);
  pg_audio_continuity_render(&state,NULL,256,0,0,left,right,128);
  for(unsigned i=0;i<128;i++) { near(left[i],0); near(right[i],0); }
  // Do not prolong an invalid producer sample throughout a silent interval.
  pg_audio_continuity_reset(&state);
  ring[0]=NAN; ring[1]=INFINITY;
  pg_audio_continuity_render(&state,ring,256,0,1,left,right,128);
  for(unsigned i=1;i<128;i++) { near(left[i],0); near(right[i],0); }
  puts("PASS: bit-exact normal audio, ring/counter wrap, callback-spanning underrun/resume ramps, repeated starvation and empty startup");
  return 0;
}
