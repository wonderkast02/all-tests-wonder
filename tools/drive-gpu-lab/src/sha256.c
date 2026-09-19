/* SPDX-License-Identifier: MIT */
#include "sha256.h"
#include <stdio.h>
#include <string.h>

#define ROR(x,n) (((x) >> (n)) | ((x) << (32-(n))))
static const uint32_t K[64] = {
0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
static uint32_t be32(const uint8_t *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static void putbe32(uint8_t *p,uint32_t x){p[0]=(uint8_t)(x>>24);p[1]=(uint8_t)(x>>16);p[2]=(uint8_t)(x>>8);p[3]=(uint8_t)x;}
static void transform(dgl_sha256_ctx *c,const uint8_t b[64]){
    uint32_t w[64],a,bv,cc,d,e,f,g,h,t1,t2; int i;
    for(i=0;i<16;i++) w[i]=be32(b+4*i);
    for(i=16;i<64;i++){uint32_t s0=ROR(w[i-15],7)^ROR(w[i-15],18)^(w[i-15]>>3);uint32_t s1=ROR(w[i-2],17)^ROR(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+s0+w[i-7]+s1;}
    a=c->state[0];bv=c->state[1];cc=c->state[2];d=c->state[3];e=c->state[4];f=c->state[5];g=c->state[6];h=c->state[7];
    for(i=0;i<64;i++){uint32_t S1=ROR(e,6)^ROR(e,11)^ROR(e,25);uint32_t ch=(e&f)^((~e)&g);t1=h+S1+ch+K[i]+w[i];uint32_t S0=ROR(a,2)^ROR(a,13)^ROR(a,22);uint32_t maj=(a&bv)^(a&cc)^(bv&cc);t2=S0+maj;h=g;g=f;f=e;e=d+t1;d=cc;cc=bv;bv=a;a=t1+t2;}
    c->state[0]+=a;c->state[1]+=bv;c->state[2]+=cc;c->state[3]+=d;c->state[4]+=e;c->state[5]+=f;c->state[6]+=g;c->state[7]+=h;
}
void dgl_sha256_init(dgl_sha256_ctx *c){static const uint32_t s[8]={0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};memcpy(c->state,s,sizeof s);c->bitlen=0;c->used=0;}
void dgl_sha256_update(dgl_sha256_ctx *c,const uint8_t *data,size_t len){while(len){size_t n=64-c->used;if(n>len)n=len;memcpy(c->block+c->used,data,n);c->used+=n;data+=n;len-=n;if(c->used==64){transform(c,c->block);c->bitlen+=512;c->used=0;}}}
void dgl_sha256_final(dgl_sha256_ctx *c,uint8_t out[32]){uint64_t total=c->bitlen+(uint64_t)c->used*8;size_t i;c->block[c->used++]=0x80;if(c->used>56){while(c->used<64)c->block[c->used++]=0;transform(c,c->block);c->used=0;}while(c->used<56)c->block[c->used++]=0;for(i=0;i<8;i++)c->block[63-i]=(uint8_t)(total>>(8*i));transform(c,c->block);for(i=0;i<8;i++)putbe32(out+4*i,c->state[i]);}
int dgl_sha256_file(const char *path,char hex[65]){FILE *f=fopen(path,"rb");uint8_t buf[65536],dig[32];size_t n,i;dgl_sha256_ctx c;if(!f)return -1;dgl_sha256_init(&c);while((n=fread(buf,1,sizeof buf,f))>0)dgl_sha256_update(&c,buf,n);if(ferror(f)){fclose(f);return -2;}fclose(f);dgl_sha256_final(&c,dig);for(i=0;i<32;i++)sprintf(hex+2*i,"%02x",dig[i]);hex[64]=0;return 0;}
