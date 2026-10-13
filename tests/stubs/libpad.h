#include "tamtypes.h"
#define PAD_STATE_STABLE 6
#define PAD_STATE_FINDCTP1 2
#define PAD_CROSS 0x4000
#define PAD_CIRCLE 0x2000
#define PAD_TRIANGLE 0x1000
#define PAD_START 0x0008
struct padButtonStatus { u8 ok, mode; u16 btns; };
int padInit(int); int padPortOpen(int,int,void*); int padGetState(int,int); int padRead(int,int,struct padButtonStatus*);
