// FIR25 bit-parity test against the physical-tested R61 reference filter.
// Host-only regression: no device actions. Includes ring-wrap, reset and high amplitudes.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../components/px68k/fmgen/r47_ym2151_fir25.h"

struct LegacyFIR {int32_t l[25],r[25];uint32_t write;};
static void original_tick(LegacyFIR *s,int32_t in_l,int32_t in_r,int32_t *ol,int32_t *orr){
 const uint32_t w=s->write;
 s->l[w]=in_l;s->r[w]=in_r;
 int64_t al=0,ar=0;
 uint32_t newer=w,older=(w+1u)%25u;
 for(uint32_t j=0;j<12u;++j){
   al+=((int64_t)s->l[newer]+s->l[older])*r47_ym2151_fir_q15[j];
   ar+=((int64_t)s->r[newer]+s->r[older])*r47_ym2151_fir_q15[j];
   newer=(newer==0u)?24u:newer-1u;
   older=(older==24u)?0u:older+1u;
 }
 al+=(int64_t)s->l[newer]*r47_ym2151_fir_q15[12];
 ar+=(int64_t)s->r[newer]*r47_ym2151_fir_q15[12];
 *ol=(int32_t)(al>>15);*orr=(int32_t)(ar>>15);
 s->write=(w==24u)?0u:w+1u;
}
static uint32_t seed=0x13489abfu;
static uint32_t random_u32(){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
int main(){
 LegacyFIR original={};R47YM2151Fir25 optimized={};r47_ym2151_fir_reset(&optimized);
 uint64_t digest=1469598103934665603ull;
 constexpr int samples=350000;
 for(int i=0;i<samples;i++){
   if(i==79111 || i==242421){original=LegacyFIR{};r47_ym2151_fir_reset(&optimized);}
   int32_t l=(int32_t)(random_u32()&0x7ffff)-262144;
   int32_t r=(int32_t)(random_u32()&0x7ffff)-262144;
   if(i%97==0)l=2147483647;
   if(i%101==0)r=(-2147483647-1);
   int32_t ol,orr,ml,mr;
   original_tick(&original,l,r,&ol,&orr);
   r47_ym2151_fir_tick(&optimized,l,r,&ml,&mr);
   if(ol!=ml || orr!=mr || original.write!=optimized.write){
     fprintf(stderr,"FIR divergence at sample %d: %d/%d vs %d/%d\n",i,ol,orr,ml,mr);
     return 1;
   }
   digest^=(uint32_t)ml;digest*=1099511628211ull;
   digest^=(uint32_t)mr;digest*=1099511628211ull;
 }
 printf("R61 FIR25 parity %d samples PASS, including wrap/reset/extremes; digest=%016llx\n",samples,(unsigned long long)digest);
 return 0;
}
