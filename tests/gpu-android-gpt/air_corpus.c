/* Corpus probe: explicit per-function AIR translation (no module fallback).
 * Run each fixture in its own process. Output blobs for external validation.
 * Only structure is checked here; this is not a pixel/semantic proof.
 */
#include "air/bitcode_reader.h"
#include "air2spv/translate_function.h"
#include "air2spv/translate.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
static int scan(const uint8_t *b,size_t n,int *pos,int *img,int *sample,int *undef){
 if(n<20||n%4)return 0;
 const uint32_t *w=(const uint32_t*)b;if(w[0]!=0x07230203)return 0;
 for(size_t i=5;i<n/4;){unsigned wc=w[i]>>16,op=w[i]&65535;if(!wc||wc>n/4-i)return 0;
 if(op==71&&wc>=4&&w[i+2]==11&&w[i+3]==0)(*pos)++;
 if(op==25)(*img)++;if(op==87||op==88)(*sample)++;if(op==45)(*undef)++;i+=wc;}
 return 1;
}
int main(int argc,char **argv){
 if(argc!=4)return 2;FILE *f=fopen(argv[1],"rb");if(!f)return 2;
 if(fseek(f,0,SEEK_END))return 2;long n=ftell(f);if(n<=0||n>4194304)return 2;rewind(f);
 uint8_t *air=malloc(n);if(!air||fread(air,1,n,f)!=(size_t)n)return 2;fclose(f);
 lagfx_air_module_t *m=NULL;if(lagfx_air_module_open(air,n,&m)!=LAGFX_OK||!m){puts("MODULE FAIL");return 1;}
 uint32_t nf=0;const lagfx_air_function_t *fn=lagfx_air_module_functions(m,&nf);int count=0,bad=0;
 for(uint32_t i=0;i<nf;i++){
  if(fn[i].is_proto)continue;
  const char *name=lagfx_air_module_string(m,fn[i].name_offset);
  /* The corpus manifest declares stage; helper functions are not assumed
     to be entry points. All defined functions are reported individually. */
  lagfx_xlate_stage_t stage=!strcmp(argv[2],"vertex")?LAGFX_XLATE_STAGE_VERTEX:LAGFX_XLATE_STAGE_FRAGMENT;
  uint8_t *blob=NULL;size_t sz=0;int st=lagfx_air2spv_translate_function(m,i,stage,&blob,&sz);
  int pos=0,img=0,sample=0,undef=0;int ok=st==LAGFX_OK&&blob&&scan(blob,sz,&pos,&img,&sample,&undef);
  printf("FUNCTION %u name=%s status=%d bytes=%zu position=%d image=%d sample=%d undef=%d structure=%s\n",i,name?name:"?",st,sz,pos,img,sample,undef,ok?"PASS":"FAIL");
  if(ok){char path[1024];int k=snprintf(path,sizeof path,"%s/%u.spv",argv[3],i);if(k<0||k>=(int)sizeof path)return 2;FILE *out=fopen(path,"wb");if(!out)return 2;size_t z=fwrite(blob,1,sz,out);int e=fclose(out);if(z!=sz||e)return 2;count++;}else bad++;
  free(blob);
 }
 lagfx_air_module_free(m);free(air);printf("TRANSLATED %d FAILED %d\n",count,bad);return count&&!bad?0:1;
}
