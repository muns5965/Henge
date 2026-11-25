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
#include <time.h>
static double ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000.0+t.tv_nsec/1e6;}
static int cmp_double(const void *a,const void *b){double x=*(const double*)a,y=*(const double*)b;return (x>y)-(x<y);}
static int scan(const uint8_t *b,size_t n,int *pos,int *img,int *sample,int *undef){
 if(n<20||n%4)return 0;
 const uint32_t *w=(const uint32_t*)b;if(w[0]!=0x07230203)return 0;
 for(size_t i=5;i<n/4;){unsigned wc=w[i]>>16,op=w[i]&65535;if(!wc||wc>n/4-i)return 0;
 if(op==71&&wc>=4&&w[i+2]==11&&w[i+3]==0)(*pos)++;
 if(op==25)(*img)++;if(op==87||op==88)(*sample)++;if(op==1)(*undef)++;i+=wc;}
 return 1;
}
int main(int argc,char **argv){
 if(argc!=4&&argc!=5)return 2;int repeats=argc==5?atoi(argv[4]):0;if(repeats<0||repeats>1000)return 2;FILE *f=fopen(argv[1],"rb");if(!f)return 2;
 if(fseek(f,0,SEEK_END))return 2;long n=ftell(f);if(n<=0||n>4194304)return 2;rewind(f);
 uint8_t *air=malloc(n);if(!air||fread(air,1,n,f)!=(size_t)n)return 2;fclose(f);
 double start=ms();lagfx_air_module_t *m=NULL;if(lagfx_air_module_open(air,n,&m)!=LAGFX_OK||!m){puts("MODULE FAIL");return 1;}
 printf("AIR_PARSE_MS %.6f\n",ms()-start);
 uint32_t nf=0;const lagfx_air_function_t *fn=lagfx_air_module_functions(m,&nf);int count=0,bad=0;
 for(uint32_t i=0;i<nf;i++){
  if(fn[i].is_proto)continue;
  const char *name=lagfx_air_module_string(m,fn[i].name_offset);
  /* The corpus manifest declares stage; helper functions are not assumed
     to be entry points. All defined functions are reported individually. */
  lagfx_xlate_stage_t stage=!strcmp(argv[2],"vertex")?LAGFX_XLATE_STAGE_VERTEX:LAGFX_XLATE_STAGE_FRAGMENT;
  lagfx_air_function_body_t *body=NULL;
  if(lagfx_air_function_body_open(m,i,&body)==LAGFX_OK&&body){
   uint32_t ni=0;const lagfx_air_inst_t *ins=lagfx_air_function_body_instructions(body,&ni);unsigned un=0;
   for(uint32_t j=0;j<ni;j++)if(ins[j].code==LAGFX_AIR_INST_UNOP||ins[j].raw_code==56){un++;printf("AIR_UNOP fn=%u inst=%u raw=%u code=%u",i,j,ins[j].raw_code,(unsigned)ins[j].code);for(uint32_t k=0;k<ins[j].num_ops;k++)printf(" %llu",(unsigned long long)ins[j].ops[k]);puts("");}
   printf("AIR_UNOP_COUNT fn=%u count=%u\n",i,un);lagfx_air_function_body_free(body);
  }else printf("AIR_BODY_DIAGNOSTIC_UNAVAILABLE fn=%u\n",i);
  double begin=ms();uint8_t *blob=NULL;size_t sz=0;int st=lagfx_air2spv_translate_function(m,i,stage,&blob,&sz);
  double elapsed=ms()-begin;unsigned neg=0,ops=0;
  if(blob&&sz>=20&&sz%4==0){const uint32_t *w=(const uint32_t*)blob;for(size_t j=5;j<sz/4;){unsigned wc=w[j]>>16;if(!wc||wc>sz/4-j)break;ops++;if((w[j]&65535)==127)neg++;j+=wc;}}
  printf("TRANSLATE_METRIC fn=%u ms=%.6f instructions=%u OpFNegate=%u\n",i,elapsed,ops,neg);
  if(repeats&&st==LAGFX_OK&&blob&&sz){
   double times[1000];int stable=1;
   for(int k=0;k<repeats;k++){uint8_t *again=NULL;size_t bytes=0;double t0=ms();
    int rc=lagfx_air2spv_translate_function(m,i,stage,&again,&bytes);times[k]=ms()-t0;
    if(rc!=LAGFX_OK||!again||bytes!=sz||memcmp(blob,again,sz))stable=0;
    free(again);if(!stable){bad++;break;}
   }
   if(stable){qsort(times,repeats,sizeof(double),cmp_double);
    printf("NATIVE_BENCH fn=%u first_ms=%.6f warm_n=%d median_ms=%.6f p95_ms=%.6f max_ms=%.6f repeat_equal=1\n",i,elapsed,repeats,times[repeats/2],times[(repeats*95+99)/100-1],times[repeats-1]);
   }else printf("NATIVE_BENCH fn=%u repeat_equal=0\n",i);
  }
  int pos=0,img=0,sample=0,undef=0;int ok=st==LAGFX_OK&&blob&&scan(blob,sz,&pos,&img,&sample,&undef);
  printf("FUNCTION %u name=%s status=%d bytes=%zu position=%d image=%d sample=%d undef=%d structure=%s\n",i,name?name:"?",st,sz,pos,img,sample,undef,ok?"PASS":"FAIL");
  if(ok){char path[1024];int k=snprintf(path,sizeof path,"%s/%u.spv",argv[3],i);if(k<0||k>=(int)sizeof path)return 2;FILE *out=fopen(path,"wb");if(!out)return 2;size_t z=fwrite(blob,1,sz,out);int e=fclose(out);if(z!=sz||e)return 2;count++;}else bad++;
  free(blob);
 }
 lagfx_air_module_free(m);free(air);printf("TRANSLATED %d FAILED %d\n",count,bad);return count&&!bad?0:1;
}
