#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "air2spv/emit_position.h"
#include "air2spv/emit_render_target.h"
static uint8_t *rd(const char *p, size_t *n){FILE*f=fopen(p,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long l=ftell(f);fseek(f,0,SEEK_SET);uint8_t*b=malloc(l);if(fread(b,1,l,f)!=(size_t)l){fclose(f);free(b);return NULL;}fclose(f);*n=l;return b;}
int main(int c,char**v){
  uint8_t *pb=NULL,*rb=NULL; size_t ps=0,rs=0;
  lagfx_air2spv_emit_position_stub(&pb,&ps);
  lagfx_air2spv_emit_render_target_stub(&rb,&rs);
  printf("position_stub size=%zu render_target_stub size=%zu\n", ps, rs);
  for(int i=1;i<c;i++){ size_t n=0; uint8_t*b=rd(v[i],&n); if(!b) continue;
    printf("%s size=%zu equals_position_stub=%d equals_render_target_stub=%d\n", v[i], n,
      n==ps && pb && memcmp(b,pb,n)==0, n==rs && rb && memcmp(b,rb,n)==0); free(b);}
  return 0; }
