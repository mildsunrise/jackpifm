// Originally created by Oliver Mattos and Oskar Weigl.

// To compile, install all the things:
//
//     $ sudo apt-get install g++ jackd2 libjack-jackd2-dev
//
// Compile with:
//
//     $ g++-O3 -Wall -Wextra -Wno-unused-parameter -l jack -o jackpifm jackpifm.cc
//
// Then use it!
//
//     $ ./jackpifm -h
//     $ sudo ./jackpifm <frequency in MHz>
//

//FIXME: format this code

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <math.h>
#include <fcntl.h>
#include <assert.h>
#include <malloc.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <signal.h>
#include <unistd.h>
#include <jack/jack.h>

#define PAGE_SIZE (4*1024)
#define BLOCK_SIZE (4*1024)

#define PI 3.14159265

int  mem_fd;
char *gpio_mem, *gpio_map;
char *spi0_mem, *spi0_map;


// I/O access
volatile unsigned *gpio;
volatile unsigned *allof7e;

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



void getRealMemPage(void** vAddr, void** pAddr) {
    void* a = valloc(4096);
    
    ((int*)a)[0] = 1;  // use page to force allocation.
    
    mlock(a, 4096);  // lock into ram.
    
    *vAddr = a;  // yay - we know the virtual address
    
    unsigned long long frameinfo;
    
    int fp = open("/proc/self/pagemap", 'r');
    lseek(fp, ((int)a)/4096*8, SEEK_SET);
    read(fp, &frameinfo, sizeof(frameinfo));
    
    *pAddr = (void*)((int)(frameinfo*4096));
}

void freeRealMemPage(void* vAddr) {
    
    munlock(vAddr, 4096);  // unlock ram.
    
    free(vAddr);
}

void setupFM() {

    /* open /dev/mem */
    if ((mem_fd = open("/dev/mem", O_RDWR|O_SYNC) ) < 0) {
        printf("can't open /dev/mem \n");
        exit (-1);
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
}


void modulate(int m)
{
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
#define BUFFERINSTRUCTIONS 65536
struct PageInfo instrs[BUFFERINSTRUCTIONS];



typedef jack_default_audio_sample_t sample_t;

class Sink {
public:
    virtual void consume(sample_t* data, size_t num) {};
};

class StereoSink {
public:
    virtual void consume(sample_t* left, sample_t* right, size_t num) {};
};

class StereoAdapter : public StereoSink {
public:

    sample_t* data;
    size_t num;
    class End : public Sink {
    public:
        StereoAdapter* a;
        End(StereoAdapter* a): a(a) {}
        void consume(sample_t* data, size_t num) {
            a->data = data;
            a->num = num;
        }
    };
    End end;

    StereoSink* next;
    StereoAdapter(StereoSink* next): end(this), next(next) {}

    Sink* left;
    Sink* right;
    void attach(Sink* left, Sink *right) {
        this->left = left;
        this->right = right;
    }
    
    void consume(sample_t* left, sample_t* right, size_t num) {
        this->left->consume(left, num);
        left = data;
        this->right->consume(right, num);
        right = data;

        next->consume(left, right, this->num);
    }
};

double clocksPerSampleRatio = 1407.05;  // for timing, determined by experiment

class Outputter : public Sink {
public:
    size_t bufferOffset;
    double clocksPerSample;
    const int sleeptime;
    double fracerror;
    double timeErr;
    
    int data_pipe [2];
    FILE* read_end;
    FILE* write_end;
    pthread_t thread;
    
    Outputter(int sampleRate):
        bufferOffset(0),
        sleeptime((double)1e6 * BUFFERINSTRUCTIONS/((double) 4*sampleRate*2)), // sleep time is half of the time to empty the buffer
        fracerror(0),
        timeErr(0) {
        
        // setup data pipe
        pipe(data_pipe);
        fcntl(data_pipe[1], F_SETFL, O_NONBLOCK);
        read_end = fdopen(data_pipe[0], "r");
        write_end = fdopen(data_pipe[1], "w");
        
        clocksPerSample = (double) 22050/sampleRate * clocksPerSampleRatio;
        
        // start thread
        pthread_create(&thread, NULL, thread_write_wrap, this);
    }
    
    void consume(sample_t* data, size_t num) {
        fwrite(data, sizeof(sample_t), num, write_end);
    }
    
    void thread_write() {
        sample_t value;
        while (fread(&value, sizeof(sample_t), 1, read_end)) {
            value *= 8;  // modulation index (AKA volume!)
           
            // dump raw baseband data to stdout for audacity analysis.
            //write(1, &value, 4);
            
            // debug code.  Replaces data with a set of tones.
            //static int debugCount;
            //debugCount++;
            //value = (debugCount & 0x1000)?0.5:0;  // two different tests
            //value += 0.2 * ((debugCount & 0x8)?1.0:-1.0);   // tone
            //if (debugCount & 0x2000) value = 0;   // silence 
            // end debug code
            
            value += fracerror;  // error that couldn't be encoded from last time.
            
            int intval = (int)(round(value));  // integer component
            double frac = (value - (double)intval + 1)/2;
            unsigned int fracval = round(frac*clocksPerSample); // the fractional component
            
            // we also record time error so that if one sample is output
            // for slightly too long, the next sample will be shorter.
            timeErr = timeErr - int(timeErr) + clocksPerSample;
            
            fracerror = (frac - (double)fracval*(1.0-2.3/clocksPerSample)/clocksPerSample)*2;  // error to feed back for delta sigma
            
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
            
            // not necessary (nor recommended) since we're getting real-time input
            //while((ACCESS(DMABASE + 0x04 /* CurBlock*/) & ~ 0x7F) ==  (int)(instrs[bufferOffset].p)) {
            //    usleep(sleeptime);  // are we anywhere in the next 4 instructions?
            //}
            
            // Create DMA command to set clock controller to output FM signal for PWM "LOW" time.
            ((struct CB*)(instrs[bufferOffset].v))->SOURCE_AD = (int)constPage.p + 2048 + intval*4 - 4 ;
            bufferOffset++;
            
            // Create DMA command to delay using serializer module for suitable time.
            ((struct CB*)(instrs[bufferOffset].v))->TXFR_LEN = (int)timeErr-fracval;
            bufferOffset++;
            
            // Create DMA command to set clock controller to output FM signal for PWM "HIGH" time.
            ((struct CB*)(instrs[bufferOffset].v))->SOURCE_AD = (int)constPage.p + 2048 + intval*4 + 4;
            bufferOffset++;
            
            // Create DMA command for more delay.
            ((struct CB*)(instrs[bufferOffset].v))->TXFR_LEN = fracval;
            bufferOffset=(bufferOffset+1) % (BUFFERINSTRUCTIONS);
        }
    }
    
    static void* thread_write_wrap(void* opaque) {
        ((Outputter*)opaque)->thread_write();
        return NULL;
    }
};

class PreEmp : public Sink {
public:
    float fmconstant;
    sample_t dataold;
    sample_t* buffer;
    Sink* next;
    
    // this isn't the right filter...  But it's close...
    // Something todo with a bilinear transform not being right...
    PreEmp(size_t bufferSize, int sampleRate, Sink* next):
        fmconstant(sampleRate * 75.0e-6), // for pre-emphisis filter.  75us time constant
        dataold(0),
        buffer(new sample_t[bufferSize]),
        next(next) {}
    
    ~PreEmp() {
        delete[] buffer;
    }
    
    void consume(sample_t* data, size_t num) {
        for (size_t i=0; i<num; i++) {
            sample_t value = data[i];
            sample_t sample = value + (dataold-value) / (1-fmconstant);  // fir of 1 + s tau
            dataold = value;
            buffer[i] = sample;
        }
        next->consume(buffer, num);
    }
};


class Resamp : public Sink { //TODO: better handle this
public:
    static const int QUALITY = 5;    // comp. complexity goes up linearly with this.
    static const int SQUALITY = 10;  // start time quality (defines max phase error of filter vs ram used & cache thrashing)
    float dataOld[QUALITY];
    sample_t sincLUT[SQUALITY][QUALITY]; // [startime][samplenum]
    float outTimeLeft;
    double ratio;
    size_t bufferOffset;
    size_t bufferSize;
    sample_t* buffer;
    Sink* next;
    
    Resamp(size_t bufferSizeIn, int rateIn, int rateOut, Sink* next):
        outTimeLeft(1.0),
        ratio((double)rateIn/rateOut),
        bufferOffset(0),
        bufferSize(round((double)bufferSizeIn/ratio)),
        buffer(new sample_t[bufferSize]),
        next(next) {
        
        for(int i=0; i<QUALITY; i++) {  // sample
          for(int j=0; j<SQUALITY; j++) {  // starttime
            sample_t x = PI * ((double)j/SQUALITY + (QUALITY-1-i) - (QUALITY-1)/2.0);
            if (x==0)
              sincLUT[j][i] = 1.0;  // sin(0)/(0) == 1, says my limits theory
            else
              sincLUT[j][i] = sin(x)/x;
          }
        }
    }
    
    ~Resamp() {
        delete[] buffer;
    }
    
    void consume(sample_t* data, size_t num) {
        for (size_t i=0; i<num; i++) {
            // shift old data along
            for (int j=0; j<QUALITY-1; j++) {
              dataOld[j] = dataOld[j+1];
            }
            
            // put in new sample
            dataOld[QUALITY-1] = data[i];
            outTimeLeft -= 1.0;
            
            // go output this stuff!
            while (outTimeLeft<1.0) {
                sample_t outSample = 0;
                int lutNum = (int)(outTimeLeft*SQUALITY);
                for (int j=0; j<QUALITY; j++) {
                    outSample += dataOld[j] * sincLUT[lutNum][j];
                }
                buffer[bufferOffset++] = outSample;
                outTimeLeft += ratio;
                
                // if we have lots of data, shunt it to the next stage.
                if (bufferOffset >= bufferSize) {
                  next->consume(buffer, bufferOffset);
                  bufferOffset = 0;
                }
            }
        }
    }
};

const unsigned char RDSDATA[] = {
// RDS data.  Send MSB first.  Google search gr_rds_data_encoder.cc to make your own data.
  0x50, 0xFF, 0xA9, 0x01, 0x02, 0x1E, 0xB0, 0x00, 0x05, 0xA1, 0x41, 0xA4, 0x12,
  0x50, 0xFF, 0xA9, 0x01, 0x02, 0x45, 0x20, 0x00, 0x05, 0xA1, 0x19, 0xB6, 0x8C,
  0x50, 0xFF, 0xA9, 0x01, 0x02, 0xA9, 0x90, 0x00, 0x05, 0xA0, 0x80, 0x80, 0xDC,
  0x50, 0xFF, 0xA9, 0x01, 0x03, 0xC7, 0xD0, 0x00, 0x05, 0xA0, 0x80, 0x80, 0xDC,
  0x50, 0xFF, 0xA9, 0x09, 0x00, 0x14, 0x75, 0x47, 0x51, 0x7D, 0xB9, 0x95, 0x79,
  0x50, 0xFF, 0xA9, 0x09, 0x00, 0x4F, 0xE7, 0x32, 0x02, 0x21, 0x99, 0xC8, 0x09,
  0x50, 0xFF, 0xA9, 0x09, 0x00, 0xA3, 0x56, 0xF6, 0xD9, 0xE8, 0x81, 0xE5, 0xEE,
  0x50, 0xFF, 0xA9, 0x09, 0x00, 0xF8, 0xC6, 0xF7, 0x5B, 0x19, 0xC8, 0x80, 0x88,
  0x50, 0xFF, 0xA9, 0x09, 0x01, 0x21, 0xA5, 0x26, 0x19, 0xD5, 0xCD, 0xC3, 0xDC,
  0x50, 0xFF, 0xA9, 0x09, 0x01, 0x7A, 0x36, 0x26, 0x56, 0x31, 0xC9, 0xC8, 0x72,
  0x50, 0xFF, 0xA9, 0x09, 0x01, 0x96, 0x87, 0x92, 0x09, 0xA5, 0x41, 0xA4, 0x12,
  0x50, 0xFF, 0xA9, 0x09, 0x01, 0xCD, 0x12, 0x02, 0x8C, 0x0D, 0xBD, 0xB6, 0xA6,
  0x50, 0xFF, 0xA9, 0x09, 0x02, 0x24, 0x46, 0x17, 0x4B, 0xB9, 0xD1, 0xBC, 0xE2,
  0x50, 0xFF, 0xA9, 0x09, 0x02, 0x7F, 0xD7, 0x34, 0x09, 0xE1, 0x9D, 0xB5, 0xFF,
  0x50, 0xFF, 0xA9, 0x09, 0x02, 0x93, 0x66, 0x16, 0x92, 0xD9, 0xB0, 0xB9, 0x3E,
  0x50, 0xFF, 0xA9, 0x09, 0x02, 0xC8, 0xF6, 0x36, 0xF4, 0x85, 0xB4, 0xA4, 0x74,
  0x50, 0xFF, 0xA9, 0x09, 0x03, 0x11, 0x92, 0x02, 0x00, 0x00, 0x80, 0x80, 0xDC,
  0x50, 0xFF, 0xA9, 0x09, 0x03, 0x4A, 0x02, 0x02, 0x00, 0x00, 0x80, 0x80, 0xDC,
  0x50, 0xFF, 0xA9, 0x09, 0x03, 0xA6, 0xB2, 0x02, 0x00, 0x00, 0x80, 0x80, 0xDC,
  0x50, 0xFF, 0xA9, 0x09, 0x03, 0xFD, 0x22, 0x02, 0x00, 0x00, 0x80, 0x80, 0xDC,
};

class RDSEncoder: public Sink {
public:
    sample_t sinLut[8];
    int bitNum;
    int lastBit;
    int time;
    sample_t lastValue;
    Sink* next;
    
    RDSEncoder(Sink* next):
        bitNum(0),
        lastBit(0),
        time(0),
        lastValue(0),
        next(next) {
        for (int i=0; i<8; i++) {
            sinLut[i] = sin((double)i*2.0*PI*3/8);
        }
    }
    
    void consume(sample_t* data, size_t num) {
        for (size_t i=0; i<num; i++) {
            if (!time) {
              // time for a new bit
              int newBit = (RDSDATA[bitNum/8]>>(7-(bitNum%8)))&1;
              lastBit = lastBit^newBit;  // differential encoding
              
              bitNum = (bitNum+1)%(20*13*8);
            }
            
            int outputBit = (time<192)?lastBit:1-lastBit; // manchester encoding
            
            lastValue = lastValue*0.99 + (((sample_t)outputBit)*2-1)*0.01;  // very simple IIR filter to hopefully reduce sidebands.
            data[i] += lastValue*sinLut[time%8]*0.05;
            
            time = (time+1)%384;
        }
        next->consume(data, num);
    }
};

// Takes 2 input signals at 152kHz and stereo modulates it.
class StereoModulator: public StereoSink {
public:
    sample_t sinLut[16];
    int state; // 8 state state machine.
    sample_t* buffer;
    Sink* next;
    
    StereoModulator(size_t bufferSize, int sampleRate, Sink* next):
        state(0),
        buffer(new sample_t[bufferSize]),
        next(next) {
        for (int i=0; i<16; i++) {
            sinLut[i] = sin((double)i*2.0*PI/8);
        }
    }
    
    ~StereoModulator() {
        delete[] buffer;
    }
    
    void consume(sample_t* left, sample_t* right, size_t num) {
        for (size_t i=0; i<num; i++) {
            state = (state+1) %8;
            // equation straight from wikipedia...
            buffer[i] = ((left[i]+right[i])/2 + (left[i]-right[i])/2*sinLut[state*2])*0.9 + 0.1*sinLut[state]; 
        }
        next->consume(buffer, num);
    }
};



size_t bufferSize;
int sampleRate;

int jackBufferSizeChanged(jack_nframes_t num, void *arg) {
    if (bufferSize == (size_t)num) return 0;
    fprintf(stderr, "Buffer size changed. I can't stand that (yet).\n");
    exit(5);
}
int jackSampleRateChanged(jack_nframes_t num, void *arg) {
    if (sampleRate == (int)num) return 0;
    fprintf(stderr, "Sample rate changed. I can't stand that (yet).\n");
    exit(5);
}

jack_client_t* client;
jack_port_t* ports [2];

int processStereo(jack_nframes_t num, void *arg) {
    StereoSink* sink = (StereoSink*) arg;
    sample_t* left = (sample_t*) jack_port_get_buffer(ports[0], num);
    sample_t* right = (sample_t*) jack_port_get_buffer(ports[1], num);
    sink->consume(left, right, num);
    return 0;
}
int processMono(jack_nframes_t num, void *arg) {
    Sink* sink = (Sink*) arg;
    sample_t* data = (sample_t*) jack_port_get_buffer(ports[0], num);
    sink->consume(data, num);
    return 0;
}

void openJACK(int channels, const char* clientName, const char** target) {
    // Open client
    client = jack_client_open(clientName, JackNoStartServer, NULL);
    if (!client) {
      fprintf(stderr, "Couldn't connect to JACK.\n");
      exit(5);
    }
    
    sampleRate = jack_get_sample_rate(client);
    bufferSize = jack_get_buffer_size(client);
    
    // Register callbacks
    if (jack_set_sample_rate_callback(client, jackSampleRateChanged, NULL) ||
        jack_set_buffer_size_callback(client, jackBufferSizeChanged, NULL)) {
      fprintf(stderr, "Couldn't set callbacks.\n");
      exit(5);
    }
    
    // Prepare audio
    if (channels == 2) {
      ports[0] = jack_port_register(client, "left",  JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput | JackPortIsTerminal, 0);
      ports[1] = jack_port_register(client, "right", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput | JackPortIsTerminal, 0);
      
      StereoAdapter* sink = new StereoAdapter(
                            /* PreEmph and Resamp */
                            new StereoModulator(bufferSize, sampleRate,
                            new RDSEncoder(
                            new Outputter(152000
                            ))));
      sink->attach(
        new PreEmp(bufferSize, sampleRate, new Resamp(bufferSize, sampleRate, 152000, &sink->end)), // left
        new PreEmp(bufferSize, sampleRate, new Resamp(bufferSize, sampleRate, 152000, &sink->end))  // right
      );
      
      jack_set_process_callback(client, processStereo, sink);
    } else {
      ports[0] = jack_port_register(client, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput | JackPortIsTerminal, 0);
      
      // FIXME: preamp should be customizable via options
      Sink* sink = new PreEmp(bufferSize, sampleRate, new Outputter(sampleRate));
      
      jack_set_process_callback(client, processMono, sink);
    }
    
    // FINALLY!
    if (jack_activate(client)) {
      fprintf(stderr, "Couldn't activate client.\n");
      exit(5);
    }
    clientName = jack_get_client_name(client);
    fprintf(stderr, "All ready, listening at %s.\n", clientName);
    
    // Connect to target
    if (channels == 1) {
      // there's only one channel!
      for (size_t i=0; i<2; i++) {
        if (!target[i]) break;
        jack_connect(client, target[i], jack_port_name(ports[0]));
      }
    } else if (!target[1]) {
      // there's only one target!
      for (size_t i=0; i<2; i++) {
        if (!target[i]) break;
        jack_connect(client, target[0], jack_port_name(ports[i]));
      }
    } else {
      for (size_t i=0; i<2; i++) {
        if (!target[i]) break;
        jack_connect(client, target[i], jack_port_name(ports[i]));
      }
    }
}

void unSetupDMA() {
    jack_client_close(client);

    fprintf(stderr, "exiting.\n");
    struct DMAregs* DMA0 = (struct DMAregs*)&(ACCESS(DMABASE));
    DMA0->CS =1<<31;  // reset dma controller
}

void handSig(int dunno) {
    exit(0);
}

void setupDMA(float centerFreq) {
    //FIXME: allow pin to be changed
    atexit(unSetupDMA);
    signal (SIGINT, handSig);
    signal (SIGTERM, handSig);
    signal (SIGHUP, handSig);
    signal (SIGQUIT, handSig);

    // allocate a few pages of ram
    getRealMemPage(&constPage.v, &constPage.p);
    
    int centerFreqDivider = (int)((500.0 / centerFreq) * (float)(1<<12) + 0.5);
    
    // make data page contents - it's essientially 1024 different commands for the
    // DMA controller to send to the clock module at the correct time.
    for (int i=0; i<1024; i++)
      ((int*)(constPage.v))[i] = (0x5a << 24) + centerFreqDivider - 512 + i;
    
    
    int instrCnt = 0;
    
    while (instrCnt<BUFFERINSTRUCTIONS) {
      getRealMemPage(&instrPage.v, &instrPage.p);
      
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
        instr0->TI = (1/* DREQ  */<<6) | (5 /* PWM */<<16) |  (1<<26/* no wide*/) ;
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

    // set up pwm
    ACCESS(PWMBASE + 0x0 /* CTRL*/) = 0;
    usleep(1000);
    ACCESS(PWMBASE + 0x4 /* status*/) = -1;  // clear errors
    usleep(1000);
    ACCESS(PWMBASE + 0x0 /* CTRL*/) = -1; //(1<<13 /* Use fifo */) | (1<<10 /* repeat */) | (1<<9 /* serializer */) | (1<<8 /* enable ch */) ;
    usleep(1000);
    ACCESS(PWMBASE + 0x8 /* DMAC*/) = (1<<31 /* DMA enable */) | 0x0707;
    
    //activate dma
    struct DMAregs* DMA0 = (struct DMAregs*)&(ACCESS(DMABASE));
    DMA0->CS =1<<31;  // reset
    DMA0->CONBLK_AD=0;
    DMA0->TI=0;
    DMA0->CONBLK_AD = (unsigned int)(instrPage.p);
    DMA0->CS =(1<<0)|(255 <<16);  // enable bit = 0, clear end flag = 1, prio=19-16
}



int main(int argc, char **argv) {
    int channels = 1;
    const char* clientName = "jackpifm";
    const char* target [2] = {NULL, NULL};
    float freq;

    // this is very C-ish, I know //TODO: add version & makefile
    int opt;
    while ((opt = getopt(argc, argv, "c:n:t:h")) != -1) {
      if (opt == 'c') {
        channels = atoi(optarg);
        if (channels < 1 || channels > 2) {
          fprintf(stderr, "%s: Incorrect number of channels.\n", argv[0]);
          goto fail;
        }
        continue;
      }
      
      if (opt == 'C') {
        clocksPerSampleRatio = atof(optarg);
        continue;
      }
      
      if (opt == 'n') {
        clientName = optarg;
        continue;
      }
      
      if (opt == 't') {
        for (size_t i=0; i<2; i++) {
          target[i] = optarg;
          char* end = strchr(optarg, ',');
          if (end) {
            *end = '\0';
            optarg = end+1;
          } else break;
        }
        continue;
      }
      
      if (opt == 'h') {
        printf("Usage: %1$s [options] <freq>\n"
               "       %1$s -h\n"
               "\n", argv[0]);
        
        printf("Broadcasts audio from JACK over FM, at frequency <freq> (in MHz).\n"
               "Outputs the FM wave at pin 7 (GPIO 4), be sure to attach an antenna.\n"
               "\n");
        
        printf("Emission options:\n"
               "  -c (1|2)    Number of channels to broadcast, 1 (mono) or 2 (stereo). Default is 1.\n"
               "              Broadcasting stereo also enables RDS.\n"
               "  -C <rate>   [debug] Override the default clock-per-sample constant.\n"
               "\n"
               
               "JACK options:\n"
               "  -n <name>   Name to use in JACK. Default is 'jackpifm'.\n"
               "  -t <ports>  Comma separated list of port names to connect to,\n"
               "              after the client has been started.\n"
               "\n"
               
               "RDS options:\n"
               //TODO
               "\n");
        return 0;
      }
      
      fprintf(stderr, "%s: Wrong option -%c.\n", argv[0], opt);
      goto fail;
    }
    
    if (argc-optind != 1) {
      fprintf(stderr, "%s: Incorrect number of arguments.\n", argv[0]);
      goto fail;
    }
    
    freq = atof(argv[optind]);

    setupFM();
    setupDMA(freq);
    openJACK(channels, clientName, target);
    
    sleep (-1);
    
    return 0;
    
fail:
    fprintf(stderr, "Try '%s -h' for help.\n", argv[0]);
    return 1;
}
