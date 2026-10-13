#include "tamtypes.h"
#define GS_PSM_CT32 0
#define GS_PSMZ_16S 0xA
#define GS_SETTING_OFF 0
#define GS_SETTING_ON 1
#define GS_ONESHOT 1
#define GS_ZTEST_OFF 0
#define GS_FILTER_NEAREST 0
#define GS_FILTER_LINEAR 1
#define GS_MODE_NTSC 2
#define GS_MODE_PAL 3
#define GSKIT_ALLOC_USERBUFFER 1
#define GS_SETREG_RGBAQ(r,g,b,a,q) ((u64)(r)|((u64)(g)<<8)|((u64)(b)<<16)|((u64)(a)<<24)|((u64)(q)<<32))
#define GS_SETREG_ALPHA(a,b,c,d,fix) ((u64)(a)|((u64)(b)<<2)|((u64)(c)<<4)|((u64)(d)<<6)|((u64)(fix)<<32))
typedef struct { int Width, Height, PSM, Filter; u32 *Mem; u32 Vram; } GSTEXTURE;
typedef struct { int Width, Height, PSM, PSMZ, ZBuffering, DoubleBuffering, PrimAlphaEnable, Mode; } GSGLOBAL;
GSGLOBAL *gsKit_init_global(void); void gsKit_init_screen(GSGLOBAL*); void gsKit_mode_switch(GSGLOBAL*,int);
void gsKit_set_test(GSGLOBAL*,int); void gsKit_set_primalpha(GSGLOBAL*,u64,int);
u32 gsKit_vram_alloc(GSGLOBAL*,u32,u8); u32 gsKit_texture_size(int,int,int); void gsKit_texture_upload(GSGLOBAL*,GSTEXTURE*);
void gsKit_clear(GSGLOBAL*,u64); void gsKit_prim_sprite(GSGLOBAL*,float,float,float,float,int,u64);
void gsKit_prim_quad_texture(GSGLOBAL*,GSTEXTURE*,float,float,float,float,float,float,float,float,float,float,float,float,float,float,float,float,int,u64);
void gsKit_queue_exec(GSGLOBAL*); void gsKit_sync_flip(GSGLOBAL*);
