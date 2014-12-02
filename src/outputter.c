#include "outputter.h"

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <time.h>
#include <sys/mman.h>

//TODO

#define PAGE_SIZE (4*1024)
#define BLOCK_SIZE (4*1024)

// I/O access
volatile unsigned *gpio;
volatile unsigned *allof7e;

int  mem_fd;
char *gpio_mem, *gpio_map;
char *spi0_mem, *spi0_map;

// GPIO setup macros. Always use INP_GPIO(x) before using OUT_GPIO(x) or SET_GPIO_ALT(x,y)
#define INP_GPIO(g) *(gpio+((g)/10)) &= ~(7<<(((g)%10)*3))
#define OUT_GPIO(g) *(gpio+((g)/10)) |=  (1<<(((g)%10)*3))
#define SET_GPIO_ALT(g,a) *(gpio+(((g)/10))) |= (((a)<=3?(a)+4:(a)==4?3:2)<<(((g)%10)*3))

#define GPIO_SET *(gpio+7)  // sets   bits which are 1 ignores bits which are 0
#define GPIO_CLR *(gpio+10) // clears bits which are 1 ignores bits which are 0
#define GPIO_GET *(gpio+13)  // sets   bits which are 1 ignores bits which are 0

#define ACCESS(base) *(volatile int*)((int)allof7e+base-0x7e000000)
#define SETBIT(base, bit) ACCESS(base) |= 1<<bit
#define CLRBIT(base, bit) ACCESS(base) &= ~(1<<bit)

#define CM_GP0CTL (0x7e101070)
#define GPFSEL0 (0x7E200000)
#define CM_GP0DIV (0x7e101074)
#define CLKBASE (0x7E101000)
#define DMABASE (0x7E007000)
#define PWMBASE  (0x7e20C000) /* PWM controller */

struct GPCTL {
  char SRC         : 4;
  char ENAB        : 1;
  char KILL        : 1;
  char             : 1;
  char BUSY        : 1;
  char FLIP        : 1;
  char MASH        : 2;
  unsigned int     : 13;
  char PASSWD      : 8;
};

static void get_real_mem_page(void** vAddr, void** pAddr) {
  void* a = valloc(4096);
  ((int*)a)[0] = 1;  /* use page to force allocation */

  mlock(a, 4096);  /* lock into RAM */
  *vAddr = a;  /* yay - we know the virtual address */

  unsigned long long frameinfo;

  int fp = open("/proc/self/pagemap", O_RDONLY);
  lseek(fp, ((int)a)/4096*8, SEEK_SET);
  read(fp, &frameinfo, sizeof(frameinfo));

  *pAddr = (void*)((int)(frameinfo*4096));
}

static void free_real_mem_page(void* vAddr) {
  munlock(vAddr, 4096);  /* unlock RAM */
  free(vAddr);
}


int jackpifm_setup_fm() {
  /* open /dev/mem */
  if ((mem_fd = open("/dev/mem", O_RDWR|O_SYNC) ) < 0) {
    fprintf(stderr, "Can't open /dev/mem: %s\n", strerror(errno));
    return 1;
  }

  allof7e = (unsigned *)mmap(
      NULL,
      0x01000000,  //len
      PROT_READ|PROT_WRITE,
      MAP_SHARED,
      mem_fd,
      0x20000000  //base
  );

  if ((int)allof7e==-1) exit(-1);

  SETBIT(GPFSEL0 , 14);
  CLRBIT(GPFSEL0 , 13);
  CLRBIT(GPFSEL0 , 12);

  struct GPCTL setupword = {6/*SRC*/, 1, 0, 0, 0, 1,0x5a};
  ACCESS(CM_GP0CTL) = *((int*)&setupword);
  return 0;
}

static void modulate(int m) {
  ACCESS(CM_GP0DIV) = (0x5a << 24) + 0x4d72 + m;
}


struct CB {
  volatile unsigned int TI;
  volatile unsigned int SOURCE_AD;
  volatile unsigned int DEST_AD;
  volatile unsigned int TXFR_LEN;
  volatile unsigned int STRIDE;
  volatile unsigned int NEXTCONBK;
  volatile unsigned int RES1;
  volatile unsigned int RES2;
};

struct DMAregs {
  volatile unsigned int CS;
  volatile unsigned int CONBLK_AD;
  volatile unsigned int TI;
  volatile unsigned int SOURCE_AD;
  volatile unsigned int DEST_AD;
  volatile unsigned int TXFR_LEN;
  volatile unsigned int STRIDE;
  volatile unsigned int NEXTCONBK;
  volatile unsigned int DEBUG;
};

struct PageInfo {
  void* p;  // physical address
  void* v;   // virtual address
};

struct PageInfo constPage;
struct PageInfo instrPage;
#define BUFFERINSTRUCTIONS JACKPIFM_BUFFERINSTRUCTIONS
struct PageInfo instrs[BUFFERINSTRUCTIONS];


static int bufPtr = 0;
static float clocksPerSample;
static struct timespec sleeptime = {0, 0};
static float fracerror = 0;
static float timeErr = 0;

void jackpifm_outputter_setup(double sample_rate, size_t period_size) {
  //sleeptime = (float)1e9 * BUFFERINSTRUCTIONS/(4 * sample_rate *2));
  sleeptime.tv_nsec = round(((double)1e9 * period_size) / sample_rate);
  clocksPerSample = 22500.0 / sample_rate * 1373.5;  // for timing, determined by experiment
}

void jackpifm_outputter_sync() {
  void *pos = (void *)(ACCESS(DMABASE + 0x04 /* CurBlock*/) & ~ 0x7F);
  for (bufPtr = 0; bufPtr < BUFFERINSTRUCTIONS; bufPtr += 4)
    if (instrs[bufPtr].p == pos) return;

  // We should never get here
  fprintf(stderr, "Ooops.\n"); //FIXME
}

void jackpifm_outputter_output(const jackpifm_sample_t *data, size_t size) {
  for (size_t i = 0; i < size; i++) {
    float value = data[i]*8;  // modulation index (AKA volume!)
    value += fracerror;  // error that couldn't be encoded from last time.

    int intval = (int)(round(value));  // integer component
    float frac = (value - (float)intval + 1)/2;
    unsigned int fracval = round(frac*clocksPerSample); // the fractional component

    // we also record time error so that if one sample is output
    // for slightly too long, the next sample will be shorter.
    timeErr = timeErr - (int)(timeErr) + clocksPerSample;

    fracerror = (frac - (float)fracval*(1.0-2.3/clocksPerSample)/clocksPerSample)*2;  // error to feed back for delta sigma

    // Note, the 2.3 constant is because our PWM isn't perfect.
    // There is a finite time for the DMA controller to load a new value from memory,
    // Therefore the width of each pulse we try to insert has a constant added to it.
    // That constant is about 2.3 bytes written to the serializer, or about 18 cycles.  We use delta sigma
    // to correct for this error and the pwm timing quantization error.

    // To reduce noise, rather than just rounding to the nearest clock we can use, we PWM between
    // the two nearest values.

    // delay if necessary.  We can also print debug stuff here while not breaking timing.
    static int time;
    time++;

    while( (ACCESS(DMABASE + 0x04 /* CurBlock*/) & ~ 0x7F) ==  (int)(instrs[bufPtr].p)) {
      nanosleep(&sleeptime);  // are we anywhere in the next 4 instructions?
    }

    // Create DMA command to set clock controller to output FM signal for PWM "LOW" time.
    ((struct CB*)(instrs[bufPtr].v))->SOURCE_AD = (int)constPage.p + 2048 + intval*4 - 4 ;
    bufPtr++;

    // Create DMA command to delay using serializer module for suitable time.
    ((struct CB*)(instrs[bufPtr].v))->TXFR_LEN = (int)timeErr-fracval;
    bufPtr++;

    // Create DMA command to set clock controller to output FM signal for PWM "HIGH" time.
    ((struct CB*)(instrs[bufPtr].v))->SOURCE_AD = (int)constPage.p + 2048 + intval*4 + 4;
    bufPtr++;

    // Create DMA command for more delay.
    ((struct CB*)(instrs[bufPtr].v))->TXFR_LEN = fracval;
    bufPtr=(bufPtr+1) % (BUFFERINSTRUCTIONS);
  }
}


void jackpifm_setup_dma(float centerFreq) {
  // allocate a few pages of ram
  get_real_mem_page(&constPage.v, &constPage.p);

  int centerFreqDivider = (int)((500.0 / centerFreq) * (float)(1<<12) + 0.5);

  // make data page contents - it's essientially 1024 different commands for the
  // DMA controller to send to the clock module at the correct time.
  for (int i=0; i<1024; i++)
    ((int*)(constPage.v))[i] = (0x5a << 24) + centerFreqDivider - 512 + i;

  int instrCnt = 0;

  while (instrCnt<BUFFERINSTRUCTIONS) {
    get_real_mem_page(&instrPage.v, &instrPage.p);

    // make copy instructions
    struct CB* instr0= (struct CB*)instrPage.v;

    for (int i=0; i<4096/sizeof(struct CB); i++) {
      instrs[instrCnt].v = (void*)((int)instrPage.v + sizeof(struct CB)*i);
      instrs[instrCnt].p = (void*)((int)instrPage.p + sizeof(struct CB)*i);
      instr0->SOURCE_AD = (unsigned int)constPage.p+2048;
      instr0->DEST_AD = PWMBASE+0x18 /* FIF1 */;
      instr0->TXFR_LEN = 4;
      instr0->STRIDE = 0;
      //instr0->NEXTCONBK = (int)instrPage.p + sizeof(struct CB)*(i+1);
      instr0->TI = (1/* DREQ  */<<6) | (5 /* PWM */<<16) |  (1<<26/* no wide*/);
      instr0->RES1 = 0;
      instr0->RES2 = 0;

      if (!(i%2)) {
        instr0->DEST_AD = CM_GP0DIV;
        instr0->STRIDE = 4;
        instr0->TI = (1<<26/* no wide*/) ;
      }

      if (instrCnt!=0) ((struct CB*)(instrs[instrCnt-1].v))->NEXTCONBK = (int)instrs[instrCnt].p;
      instr0++;
      instrCnt++;
    }
  }
  ((struct CB*)(instrs[BUFFERINSTRUCTIONS-1].v))->NEXTCONBK = (int)instrs[0].p;

  // set up a clock for the PWM
  ACCESS(CLKBASE + 40*4 /*PWMCLK_CNTL*/) = 0x5A000026;
  usleep(1000);
  ACCESS(CLKBASE + 41*4 /*PWMCLK_DIV*/)  = 0x5A002800;
  ACCESS(CLKBASE + 40*4 /*PWMCLK_CNTL*/) = 0x5A000016;
  usleep(1000);

  // set up PWM
  ACCESS(PWMBASE + 0x0 /* CTRL*/) = 0;
  usleep(1000);
  ACCESS(PWMBASE + 0x4 /* status*/) = -1;  // clear errors
  usleep(1000);
  ACCESS(PWMBASE + 0x0 /* CTRL*/) = -1; //(1<<13 /* Use fifo */) | (1<<10 /* repeat */) | (1<<9 /* serializer */) | (1<<8 /* enable ch */) ;
  usleep(1000);
  ACCESS(PWMBASE + 0x8 /* DMAC*/) = (1<<31 /* DMA enable */) | 0x0707;

  //activate DMA
  struct DMAregs* DMA0 = (struct DMAregs*)&(ACCESS(DMABASE));
  DMA0->CS =1<<31;  // reset
  DMA0->CONBLK_AD=0;
  DMA0->TI=0;
  DMA0->CONBLK_AD = (unsigned int)(instrPage.p);
  DMA0->CS =(1<<0)|(255 <<16);  // enable bit = 0, clear end flag = 1, prio=19-16
}

void jackpifm_unsetup_dma() {
  struct DMAregs* DMA0 = (struct DMAregs*)&(ACCESS(DMABASE));
  DMA0->CS= 1<<31;  // reset DMA controller
}
