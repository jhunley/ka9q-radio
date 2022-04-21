// $Id: main.c,v 1.245 2022/04/21 08:11:30 karn Exp $
// Read samples from multicast stream
// downconvert, filter, demodulate, multicast output
// Copyright 2017-2022, Phil Karn, KA9Q, karn@ka9q.net
#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <math.h>
#include <complex.h>
#undef I
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <locale.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <signal.h>
#include <getopt.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <iniparser/iniparser.h>

#include "misc.h"
#include "multicast.h"
#include "radio.h"
#include "filter.h"
#include "status.h"
#include "config.h"

// Configuration constants & defaults
static char const *Wisdom_file = "/var/lib/ka9q-radio/wisdom";

static int const DEFAULT_IP_TOS = 48;
static int const DEFAULT_MCAST_TTL = 1;
static float const DEFAULT_BLOCKTIME = 20.0;
static int const DEFAULT_OVERLAP = 5;
static int const DEFAULT_FFT_THREADS = 1;
static int const DEFAULT_SAMPRATE = 24000;
static float const DEFAULT_KAISER_BETA = 11.0;   // reasonable tradeoff between skirt sharpness and sidelobe height
static float const DEFAULT_LOW = -5000.0;        // Ballpark numbers, should be properly set for each mode
static float const DEFAULT_HIGH = 5000.0;
static float const DEFAULT_HEADROOM = -15.0;     // keep gaussian signals from clipping
static float const DEFAULT_SQUELCH_OPEN = 8.0;   // open when SNR > 8 dB
static float const DEFAULT_SQUELCH_CLOSE = 7.0;  // close when SNR < 7 dB
static float const DEFAULT_RECOVERY_RATE = 20.0; // 20 dB/s gain increase
static float const DEFAULT_THRESHOLD = -15.0;    // Don't let noise rise above -15 relative to headroom
static float const DEFAULT_GAIN = 80.0;          // Unused in FM, usually adjusted automatically in linear
static float const DEFAULT_HANGTIME = 1.1;       // keep low gain 1.1 sec before increasing
static float const DEFAULT_PLL_BW = 100.0;       // Reasonable for AM
static int const DEFAULT_SQUELCHTAIL = 1;        // close on frame *after* going below threshold, may let partial frame noise through
static float const DEFAULT_NBFM_TC = 530.5;      // Time constant for NBFM emphasis (300 Hz corner)
static float const DEFAULT_WFM_TC = 75.0;        // Time constant for FM broadcast (North America/Korea standard)



char const *Modefile = "/usr/local/share/ka9q-radio/modes.conf";

// Command line and environ params
int Verbose;
static char const *Locale = "en_US.UTF-8";
dictionary *Configtable; // Configtable file descriptor for iniparser
dictionary *Modetable;

struct demod *Dynamic_demod; // Prototype for dynamically created demods

int Mcast_ttl;
int IP_tos; // AF12 left shifted 2 bits
int RTCP_enable;
int SAP_enable;
static int Overlap;
char const *Name;

static struct timespec Starttime;      // System clock at timestamp 0, for RTCP
pthread_t Status_thread;
pthread_t Demod_reaper_thread;
struct sockaddr_storage Metadata_source_address;   // Source of SDR metadata
struct sockaddr_storage Metadata_dest_address;      // Dest of metadata (typically multicast)b
char Metadata_dest_string[_POSIX_HOST_NAME_MAX+20]; // Allow room for :portnum
uint64_t Metadata_packets;
uint32_t Command_tag;
uint64_t Commands;

static void closedown(int);
static int setup_frontend(char const *arg);
static int loadconfig(char const *file);

// The main program sets up the demodulator parameter defaults,
// overwrites them with command-line arguments and/or state file settings,
// initializes the various local oscillators, pthread mutexes and conditions
// sets up multicast I/Q input and PCM audio output
// Sets up the input half of the pre-detection filter
// starts the RTP input and downconverter/filter threads
// sets the initial demodulation mode, which starts the demodulator thread
// catches signals and eventually becomes the user interface/display loop
int main(int argc,char *argv[]){
  // if we have root, up our priority and drop privileges
  int prio = getpriority(PRIO_PROCESS,0);
  prio = setpriority(PRIO_PROCESS,0,prio - 10);

  // Quickly drop root if we have it
  // The sooner we do this, the fewer options there are for abuse
  if(seteuid(getuid()) != 0)
    perror("seteuid");

  setlinebuf(stdout);
  clock_gettime(CLOCK_REALTIME,&Starttime);

  // Set up program defaults
  // Some can be overridden by command line args
  {
    // The display thread assumes en_US.UTF-8, or anything with a thousands grouping character
    // Otherwise the cursor movements will be wrong
    char const * const cp = getenv("LANG");
    if(cp != NULL)
      Locale = cp;
  }
  setlocale(LC_ALL,Locale); // Set either the hardwired default or the value of $LANG if it exists
  fprintf(stdout,"KA9Q Multichannel SDR\n");
  fprintf(stdout,"Copyright 2018-2022 by Phil Karn, KA9Q; may be used under the terms of the GNU General Public License\n");
#ifndef NDEBUG
  fprintf(stdout,"Assertion checking enabled\n");
#endif

  int c;
  while((c = getopt(argc,argv,"N:v")) != -1){
    switch(c){
    case 'v':
      Verbose++;
      break;
    case 'N':
      Name = optarg;
      break;
    default:
      fprintf(stdout,"Unknown command line option %c\n",c);
      break;
    }
  }

  
  // Graceful signal catch
  signal(SIGPIPE,closedown);
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);        
  signal(SIGPIPE,SIG_IGN);
  
  char const *configfile;
  if(argc <= optind){
    fprintf(stdout,"Configtable file missing\n");
    exit(1);
  }
  configfile = argv[optind];
  if(Name == NULL)
    Name = argv[optind];
  
  fprintf(stdout,"Loading config file %s...\n",configfile);
  fflush(stdout);
  int n = loadconfig(argv[optind]);
  fprintf(stdout,"%d total demodulators started\n",n);
  fflush(stdout);

  // all done, but we have to stay alive
  while(1)
    sleep(100);

  exit(0);
}

static int Frontend_started;

static int setup_frontend(char const *arg){
  if(Frontend_started)
    return 0;  // Only do this once
  Frontend.sdr.gain = 1; // In case it's never sent by front end

  fftwf_init_threads();
  fftwf_make_planner_thread_safe();
  int r = fftwf_import_system_wisdom();
  fprintf(stdout,"fftwf_import_system_wisdom() %s\n",r == 1 ? "succeeded" : "failed");
  r = fftwf_import_wisdom_from_filename(Wisdom_file);
  fprintf(stdout,"fftwf_import_wisdom_from_filename(%s) %s\n",Wisdom_file,r == 1 ? "succeeded" : "failed");

  pthread_mutex_init(&Frontend.sdr.status_mutex,NULL);
  pthread_cond_init(&Frontend.sdr.status_cond,NULL);

  Frontend.input.status_fd = -1;

  strlcpy(Frontend.input.metadata_dest_string,arg,sizeof(Frontend.input.metadata_dest_string));
  {
    char iface[1024];
    resolve_mcast(Frontend.input.metadata_dest_string,&Frontend.input.metadata_dest_address,DEFAULT_STAT_PORT,iface,sizeof(iface));
    Frontend.input.status_fd = listen_mcast(&Frontend.input.metadata_dest_address,iface);

    if(Frontend.input.status_fd < 3){
      fprintf(stdout,"%s: Can't set up SDR status socket\n",Frontend.input.metadata_dest_string);
      return -1;
    }
    Frontend.input.ctl_fd = connect_mcast(&Frontend.input.metadata_dest_address,iface,Mcast_ttl,IP_tos);
  }

  if(Frontend.input.ctl_fd < 3){
    fprintf(stdout,"%s: Can't set up SDR control socket\n",Frontend.input.metadata_dest_string);
    return -1;
  }
  {
    char addrtmp[256];
    addrtmp[0] = 0;
    switch(Frontend.input.metadata_dest_address.ss_family){
    case AF_INET:
      {
	struct sockaddr_in *sin = (struct sockaddr_in *)&Frontend.input.metadata_dest_address;
	inet_ntop(AF_INET,&sin->sin_addr,addrtmp,sizeof(addrtmp));
      }
      break;
    case AF_INET6:
      {
	struct sockaddr_in6 *sin = (struct sockaddr_in6 *)&Frontend.input.metadata_dest_address;
	inet_ntop(AF_INET6,&sin->sin6_addr,addrtmp,sizeof(addrtmp));
      }
      break;
    }
    fprintf(stdout,"Front end control stream %s (%s)\n",Frontend.input.metadata_dest_string,addrtmp);
  }    
  // Start status thread - will also listen for SDR commands
  if(Verbose)
    fprintf(stdout,"Starting front end status thread\n");
  pthread_create(&Frontend.status_thread,NULL,sdr_status,&Frontend);

  // We must acquire a status stream before we can proceed further
  pthread_mutex_lock(&Frontend.sdr.status_mutex);
  while(Frontend.sdr.samprate == 0 || Frontend.input.data_dest_address.ss_family == 0)
    pthread_cond_wait(&Frontend.sdr.status_cond,&Frontend.sdr.status_mutex);
  pthread_mutex_unlock(&Frontend.sdr.status_mutex);

  {
    char addrtmp[256];
    addrtmp[0] = 0;
    switch(Frontend.input.data_dest_address.ss_family){
    case AF_INET:
      {
	struct sockaddr_in *sin = (struct sockaddr_in *)&Frontend.input.data_dest_address;
	inet_ntop(AF_INET,&sin->sin_addr,addrtmp,sizeof(addrtmp));
      }
      break;
    case AF_INET6:
      {
	struct sockaddr_in6 *sin = (struct sockaddr_in6 *)&Frontend.input.data_dest_address;
	inet_ntop(AF_INET6,&sin->sin6_addr,addrtmp,sizeof(addrtmp));
      }
      break;
    }
    fprintf(stdout,"Front end data stream %s\n",addrtmp);
  }  
  fprintf(stdout,"Input sample rate %'d Hz, %s; block time %.1f ms, %.1f Hz\n",
	  Frontend.sdr.samprate,Frontend.sdr.isreal ? "real" : "complex",Blocktime,1000./Blocktime);
  fflush(stdout);

  // Input socket for I/Q data from SDR, set from OUTPUT_DEST_SOCKET in SDR metadata
  Frontend.input.data_fd = listen_mcast(&Frontend.input.data_dest_address,NULL);
  if(Frontend.input.data_fd < 3){
    fprintf(stdout,"Can't set up IF input\n");
    return -1;
  }
  // Create input filter now that we know the parameters
  // FFT and filter sizes now computed from specified block duration and sample rate
  // L = input data block size
  // M = filter impulse response duration
  // N = FFT size = L + M - 1
  // Note: no checking that N is an efficient FFT blocksize; choose your parameters wisely
  int L = (long long)llroundf(Frontend.sdr.samprate * Blocktime / 1000); // Blocktime is in milliseconds
  int M = L / (Overlap - 1) + 1;
  Frontend.in = create_filter_input(L,M, Frontend.sdr.isreal ? REAL : COMPLEX);
  if(Frontend.in == NULL){
    fprintf(stdout,"Input filter setup failed\n");
    return -1;
  }

  // Launch procsamp to process incoming samples and execute the forward FFT
  pthread_t procsamp_thread;
  pthread_create(&procsamp_thread,NULL,proc_samples,NULL);

#if 0 // Turned off in favor of per-demod estimation
  // Launch thread to estimate noise spectral density N0
  // Is this always necessary? It's not always used
  pthread_t n0_thread;
  pthread_create(&n0_thread,NULL,estimate_n0,NULL);
#endif

  Frontend_started++; // Only do this once!!
  return 0;
}

static int loadconfig(char const * const file){
  if(file == NULL || strlen(file) == 0)
    return -1;

  int ndemods = 0;
  int base_address = 0;
  for(int i=0;i<3;i++){
    base_address <<= 8;
    base_address += Name[i];
  }
  Configtable = iniparser_load(file);
  if(Configtable == NULL){
    fprintf(stdout,"Can't load config file %s\n",file);
    exit(1);
  }
  // Process [global] section applying to all demodulator blocks
  char const * const global = "global";
  if(config_getboolean(Configtable,global,"verbose",0))
    Verbose++;
  IP_tos = config_getint(Configtable,global,"tos",DEFAULT_IP_TOS);
  Mcast_ttl = config_getint(Configtable,global,"ttl",DEFAULT_MCAST_TTL);
  Blocktime = fabs(config_getdouble(Configtable,global,"blocktime",DEFAULT_BLOCKTIME));
  Overlap = abs(config_getint(Configtable,global,"overlap",DEFAULT_OVERLAP));
  Nthreads = config_getint(Configtable,global,"fft-threads",DEFAULT_FFT_THREADS);
  RTCP_enable = config_getboolean(Configtable,global,"rtcp",0);
  SAP_enable = config_getboolean(Configtable,global,"sap",0);
  Modefile = config_getstring(Configtable,global,"mode-file",Modefile);
  Wisdom_file = config_getstring(Configtable,global,"wisdom-file",Wisdom_file);
  char const * const input = config_getstring(Configtable,global,"input",NULL);
  if(input == NULL){
    // Mandatory
    fprintf(stdout,"input not specified in [%s]\n",global);
    exit(1);
  }
  if(setup_frontend(input) == -1){
    fprintf(stdout,"Front end setup of %s failed\n",input);
    exit(1);
  }
  {
    char const * const status = config_getstring(Configtable,global,"status",NULL); // Status/command thread for all demodulators
    if(status != NULL){
      // Target for status/control stream. Optional.
      strlcpy(Metadata_dest_string,status,sizeof(Metadata_dest_string));
      char service_name[1024];
      snprintf(service_name,sizeof(service_name),"%s radio (%s)",Name,status);
      char description[1024];
      snprintf(description,sizeof(description),"input=%s",input);
      avahi_start(service_name,"_ka9q-ctl._udp",DEFAULT_STAT_PORT,Metadata_dest_string,ElfHashString(Metadata_dest_string),description);
      base_address += 16;
      char iface[1024];
      resolve_mcast(Metadata_dest_string,&Metadata_dest_address,DEFAULT_STAT_PORT,iface,sizeof(iface));
      Status_fd = connect_mcast(&Metadata_dest_address,iface,Mcast_ttl,IP_tos);
      if(Status_fd < 3){
	fprintf(stdout,"Can't send status to %s\n",Metadata_dest_string);
      } else {
	socklen_t len = sizeof(Metadata_source_address);
	getsockname(Status_fd,(struct sockaddr *)&Metadata_source_address,&len);  
	// Same remote socket as status
	Ctl_fd = setup_mcast(NULL,(struct sockaddr *)&Metadata_dest_address,0,Mcast_ttl,IP_tos,2);
	if(Ctl_fd < 3)
	  fprintf(stdout,"can't listen for commands from %s\n",Metadata_dest_string);
      }
    }
  }
  // Process individual demodulator sections
  if(Modetable == NULL){
    Modetable = iniparser_load(Modefile); // Kept open for duration of program
    if(Modetable == NULL){
      fprintf(stdout,"Can't load mode file %s\n",Modefile);
      return -1;
    }
  }
  int const nsect = iniparser_getnsec(Configtable);
  for(int sect = 0; sect < nsect; sect++){
    char const * const sname = iniparser_getsecname(Configtable,sect);
    if(strcmp(sname,global) == 0)
      continue; // Already processed above

    fprintf(stdout,"Processing [%s]\n",sname);
    if(config_getboolean(Configtable,sname,"disable",0))
	continue; // section is disabled

    // Structure is created and initialized before being put on list
    struct demod *demod = alloc_demod();
    demod->tp1 = demod->tp2 = NAN;
    demod->tune.doppler = 0;
    demod->tune.doppler_rate = 0;
    // De-emphasis defaults to off, enabled only in FM modes
    demod->deemph.rate = 0;
    demod->deemph.gain = 1.0;

    // fall back to setting in [global] if parameter not specified in individual section
    // Set parameters even when unused for the current demodulator in case the demod is changed later
    char const * mode = config2_getstring(Configtable,Configtable,global,sname,"mode",NULL);
    if(mode == NULL || strlen(mode) == 0)
      fprintf(stdout,"warning: mode preset not selected, using built-in defaults\n");

    {
      char const * demod_name = config2_getstring(Modetable,Configtable,mode,sname,"demod",NULL);
      if(demod_name == NULL){
	fprintf(stdout,"Demodulator name missing\n");
	free_demod(&demod);
	continue;
      }
      demod->demod_type = demod_type_from_name(demod_name);
      if(demod->demod_type < 0){
	fprintf(stderr,"Demodulator '%s' unknown\n",demod_name);
	free_demod(&demod);
	continue;
      }
    }
    demod->output.rtp.ssrc = (uint32_t)config_getdouble(Configtable,sname,"ssrc",0); // Default triggers auto gen from freq
    demod->output.samprate = config2_getint(Modetable,Configtable,mode,global,"samprate",DEFAULT_SAMPRATE);
    demod->output.samprate = abs(demod->output.samprate);
    if(demod->output.samprate == 0){
      fprintf(stdout,"Error! samprate is zero\n");
      free_demod(&demod);
      continue;
    }
    demod->filter.kaiser_beta = config2_getfloat(Modetable,Configtable,mode,global,"kaiser-beta",DEFAULT_KAISER_BETA);
    {
      // Pre-detection filter limits
      float low = config2_getfloat(Modetable,Configtable,mode,global,"low",DEFAULT_LOW);
      float high = config2_getfloat(Modetable,Configtable,mode,global,"high",DEFAULT_HIGH);
      if(low > high){
	// Ensure high > low
	float t = low;
	low = high;
	high = t;
      }
      demod->filter.max_IF = high;
      demod->filter.min_IF = low;
    }
    {
      float squelch_open = config2_getfloat(Modetable,Configtable,mode,global,"squelch-open",DEFAULT_SQUELCH_OPEN);
      float squelch_close = config2_getfloat(Modetable,Configtable,mode,global,"squelch-close",DEFAULT_SQUELCH_CLOSE);
      if(squelch_close > squelch_open){
	fprintf(stdout,"warning: setting squelch_close = squelch_open\n");
	squelch_close = squelch_open;
      }
      demod->squelch_open = dB2power(squelch_open);
      demod->squelch_close = dB2power(squelch_close);
    }
    demod->squelchtail = config2_getint(Modetable,Configtable,mode,global,"squelchtail",DEFAULT_SQUELCHTAIL);
    demod->squelchtail = abs(demod->squelchtail); // ensure not negative
    {
      float headroom = config2_getfloat(Modetable,Configtable,mode,global,"headroom",DEFAULT_HEADROOM);
      demod->output.headroom = dB2voltage(-fabsf(headroom)); // always treat as <= 0 dB
    }
    {
      int channels = config2_getint(Modetable,Configtable,mode,global,"channels",1); // Default mono, i.e., not IQ
      if(config2_getboolean(Modetable,Configtable,mode,global,"stereo",0))
	channels = 2;
      if(config2_getboolean(Modetable,Configtable,mode,global,"mono",0))
	channels = 1;
      
      if(channels != 1 && channels != 2){
	fprintf(stdout,"Invalid channel count %d, forcing to 1\n",demod->output.channels);
	channels = 1;
      }
      demod->output.channels = channels;
    }
    demod->tune.shift = config2_getfloat(Modetable,Configtable,mode,sname,"shift",0.0);
    {
      // dB/sec -> voltage ratio/block
      float x = config2_getfloat(Modetable,Configtable,mode,sname,"recovery-rate",DEFAULT_RECOVERY_RATE);
      demod->linear.recovery_rate = dB2voltage(fabsf(x) * .001f * Blocktime);
    }
    {
      // time in seconds -> time in blocks
      float x = config2_getfloat(Modetable,Configtable,mode,sname,"hang-time",DEFAULT_HANGTIME);
      demod->linear.hangtime = fabsf(x) / (.001 * Blocktime); // Always >= 0
    }
    {
      float x = config2_getfloat(Modetable,Configtable,mode,sname,"threshold",DEFAULT_THRESHOLD);
      demod->linear.threshold = dB2voltage(-fabsf(x)); // Always <= unity
    }
    {
      float x = config2_getfloat(Modetable,Configtable,mode,sname,"gain",DEFAULT_GAIN);
      demod->output.gain = dB2voltage(x); // Can be more or less than unity
    }
    demod->linear.env = config2_getboolean(Modetable,Configtable,mode,sname,"envelope",0);   // default off 
    demod->linear.pll = config2_getboolean(Modetable,Configtable,mode,sname,"pll",0);        // default off. On also enables squelch!
    demod->linear.square = config2_getboolean(Modetable,Configtable,mode,sname,"square",0);  // default off. On implies PLL on
    if(demod->linear.square)
      demod->linear.pll = 1; // Square implies PLL

    demod->filter.isb = config2_getboolean(Modetable,Configtable,mode,sname,"conj",0);       // default off (unimplemented anyway)
    demod->linear.loop_bw = config2_getfloat(Modetable,Configtable,mode,sname,"pll-bw",DEFAULT_PLL_BW);
    demod->linear.agc = config2_getboolean(Modetable,Configtable,mode,sname,"agc",1);        // default ON
    switch(demod->demod_type){
    case LINEAR_DEMOD:
      break;
    case FM_DEMOD:
      {
	float tc = config2_getfloat(Modetable,Configtable,mode,sname,"deemph-tc",DEFAULT_NBFM_TC);
	if(tc != 0.0){
	  demod->deemph.rate = expf(-1.0 / (tc * 1e-6 * demod->output.samprate));
	  demod->deemph.gain = config2_getfloat(Modetable,Configtable,mode,sname,"deemph-gain",4.0); // empirical value, needs work
	}
      }
      break;
    case WFM_DEMOD:
      demod->output.channels = 2;      // always stereo
      demod->output.samprate = 384000; // downconverter samprate forced for FM stereo decoding. Output also forced to 48 kHz
      {
	// Default 75 microseconds for north american FM broadcasting
	float tc = config2_getfloat(Modetable,Configtable,mode,sname,"deemph-tc",DEFAULT_WFM_TC);
	if(tc != 0){
	  demod->deemph.rate = expf(-1.0 / (tc * 1e-6 * 48000)); // hardwired output sample rate -- needs cleanup
	  demod->deemph.gain = config2_getfloat(Modetable,Configtable,mode,sname,"deemph-gain",4.0);  // empirical value, needs work
	}
      }
    }
    char const * const status = config_getstring(Configtable,sname,"status",NULL);
    if(status)
      fprintf(stdout,"note: 'status =' now set in [global] section only\n");

    char const * data = config_getstring(Configtable,global,"data",NULL);
    data = config_getstring(Configtable,sname,"data",data);
    if(data == NULL){
      fprintf(stdout,"'data =' missing and not set in [%s]\n",global);
      free_demod(&demod);
      continue;
    }
    strlcpy(demod->output.data_dest_string,data,sizeof(demod->output.data_dest_string));
    // There can be multiple senders to an output stream, so let avahi suppress the duplicate addresses
    char service_name[1024];
    snprintf(service_name,sizeof(service_name),"%s radio (%s)",sname,data);
    char description[1024];
    snprintf(description,sizeof(description),"pcm-source=%s",formatsock(&Frontend.input.data_dest_address));
    avahi_start(service_name,"_rtp._udp",5004,demod->output.data_dest_string,ElfHashString(demod->output.data_dest_string),description);
    base_address += 16;
    char iface[1024];
    resolve_mcast(demod->output.data_dest_string,&demod->output.data_dest_address,DEFAULT_RTP_PORT,iface,sizeof(iface));
    demod->output.data_fd = connect_mcast(&demod->output.data_dest_address,iface,Mcast_ttl,IP_tos);
    if(demod->output.data_fd < 3){
      fprintf(stdout,"can't set up PCM output to %s\n",demod->output.data_dest_string);
      continue;
    } else {
      socklen_t len = sizeof(demod->output.data_source_address);
      getsockname(demod->output.data_fd,(struct sockaddr *)&demod->output.data_source_address,&len);
    }
    
    if(SAP_enable){
      // Highly experimental, off by default
      char sap_dest[] = "224.2.127.254:9875"; // sap.mcast.net
      demod->output.sap_fd = setup_mcast(sap_dest,NULL,1,Mcast_ttl,IP_tos,0);
      if(demod->output.sap_fd < 3)
	fprintf(stdout,"Can't set up SAP output to %s\n",sap_dest);
      else
	pthread_create(&demod->sap_thread,NULL,sap_send,demod);
    }
     // RTCP Real Time Control Protocol daemon is optional
    if(RTCP_enable){
      demod->output.rtcp_fd = setup_mcast(demod->output.data_dest_string,NULL,1,Mcast_ttl,IP_tos,1); // RTP port number + 1
      if(demod->output.rtcp_fd < 3)
	fprintf(stdout,"can't set up RTCP output to %s\n",demod->output.data_dest_string);
      else
	pthread_create(&demod->rtcp_thread,NULL,rtcp_send,demod);
    }
    // Process frequency/frequencies
    // To work around iniparser's limited line length, we look for multiple keywords
    // "freq", "freq0", "freq1", etc, up to "freq9"
    int nfreq = 0;
    for(int ff = -1; ff < 10; ff++){
      char fname[10];
      if(ff == -1)
	snprintf(fname,sizeof(fname),"freq");
      else
	snprintf(fname,sizeof(fname),"freq%d",ff);

      char const * const frequencies = config_getstring(Configtable,sname,fname,NULL);
      if(frequencies == NULL)
	break; // no more

      char *freq_list = strdup(frequencies); // Need writeable copy for strtok
      char *saveptr = NULL;
      for(char *tok = strtok_r(freq_list," \t",&saveptr);
	  tok != NULL;
	  tok = strtok_r(NULL," \t",&saveptr)){
	
	double const f = parse_frequency(tok);
	if(f < 0){
	  fprintf(stdout,"can't parse frequency %s\n",tok);
	  continue;
	}
	demod->tune.freq = f;

	// If not explicitly specified, generate SSRC in decimal using frequency in Hz
	if(demod->output.rtp.ssrc == 0){
	  if(f == 0){
	    if(Dynamic_demod){
	      free_demod(&Dynamic_demod);
	    }
	    // Template for dynamically created demods
	    Dynamic_demod = demod;
	    fprintf(stdout,"dynamic demod template created\n");
	  } else {
	    for(char const *cp = tok ; cp != NULL && *cp != '\0' ; cp++){
	      if(isdigit(*cp)){
		demod->output.rtp.ssrc *= 10;
		demod->output.rtp.ssrc += *cp - '0';
	      }
	    }
	  }
	}
	// Initialization all done, start it up
	set_freq(demod,demod->tune.freq);
	start_demod(demod);
	nfreq++;
	ndemods++;
	if(Verbose)
	  fprintf(stdout,"started %'.3lf Hz\n",demod->tune.freq);

	// Set up for next demod
	struct demod *ndemod = alloc_demod();
	if(ndemod == NULL){
	  fprintf(stdout,"alloc_demod() failed, quitting\n");
	  break;
	}
	// Copy everything to next demod except filter, demod thread ID, freq and ssrc
	memcpy(ndemod,demod,sizeof(*ndemod));
	ndemod->filter.out = NULL;
	ndemod->demod_thread = (pthread_t)0;
	ndemod->tune.freq = 0;
	ndemod->output.rtp.ssrc = 0;
	demod = ndemod;
	ndemod = NULL;
      }
      free(freq_list);
      freq_list = NULL;
    }
    free_demod(&demod); // last one wasn't needed
    fprintf(stdout,"%d demodulators started\n",nfreq);
  }
  // Start the status thread after all the receivers have been created so it doesn't contend for the demod list lock
  if(Ctl_fd >= 3 && Status_fd >= 3){
    pthread_create(&Status_thread,NULL,radio_status,NULL);
    pthread_create(&Demod_reaper_thread,NULL,demod_reaper,NULL);
  }
  iniparser_freedict(Configtable);
  Configtable = NULL;
  return ndemods;
}

// RTP control protocol sender task
void *rtcp_send(void *arg){
  struct demod const *demod = (struct demod *)arg;
  if(demod == NULL)
    pthread_exit(NULL);

  {
    char name[100];
    snprintf(name,sizeof(name),"rtcp %u",demod->output.rtp.ssrc);
    pthread_setname(name);
  }

  while(1){

    if(demod->output.rtp.ssrc == 0) // Wait until it's set by output RTP subsystem
      goto done;
    unsigned char buffer[4096]; // much larger than necessary
    memset(buffer,0,sizeof(buffer));
    
    // Construct sender report
    struct rtcp_sr sr;
    memset(&sr,0,sizeof(sr));
    sr.ssrc = demod->output.rtp.ssrc;

    // Construct NTP timestamp
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME,&ts);
    double runtime = (ts.tv_sec - Starttime.tv_sec) + (ts.tv_nsec - Starttime.tv_nsec)/1000000000.;

    long long now_time = ((long long)ts.tv_sec + NTP_EPOCH)<< 32;
    now_time += ((long long)ts.tv_nsec << 32) / 1000000000;

    sr.ntp_timestamp = now_time;
    // The zero is to remind me that I start timestamps at zero, but they could start anywhere
    sr.rtp_timestamp = 0 + runtime * demod->output.samprate;
    sr.packet_count = demod->output.rtp.seq;
    sr.byte_count = demod->output.rtp.bytes;
    
    unsigned char *dp = gen_sr(buffer,sizeof(buffer),&sr,NULL,0);

    // Construct SDES
    struct rtcp_sdes sdes[4];
    
    // CNAME
    char hostname[1024];
    gethostname(hostname,sizeof(hostname));
    char *string = NULL;
    int sl = asprintf(&string,"radio@%s",hostname);
    if(sl > 0 && sl <= 255){
      sdes[0].type = CNAME;
      strlcpy(sdes[0].message,string,sizeof(sdes[0].message));
      sdes[0].mlen = strlen(sdes[0].message);
    }
    if(string){
      free(string); string = NULL;
    }

    sdes[1].type = NAME;
    strlcpy(sdes[1].message,"KA9Q Radio Program",sizeof(sdes[1].message));
    sdes[1].mlen = strlen(sdes[1].message);
    
    sdes[2].type = EMAIL;
    strlcpy(sdes[2].message,"karn@ka9q.net",sizeof(sdes[2].message));
    sdes[2].mlen = strlen(sdes[2].message);

    sdes[3].type = TOOL;
    strlcpy(sdes[3].message,"KA9Q Radio Program",sizeof(sdes[3].message));
    sdes[3].mlen = strlen(sdes[3].message);
    
    dp = gen_sdes(dp,sizeof(buffer) - (dp-buffer),demod->output.rtp.ssrc,sdes,4);


    send(demod->output.rtcp_fd,buffer,dp-buffer,0);
  done:;
    usleep(1000000);
  }
}
static void closedown(int a){
  fprintf(stdout,"Received signal %d, exiting\n",a);
  int r = fftwf_export_wisdom_to_filename(Wisdom_file);
  fprintf(stdout,"fftwf_export_wisdom_to_filename(%s) %s\n",Wisdom_file,r == 1 ? "succeeded" : "failed");

  if(a == SIGTERM)
    exit(0); // Return success when terminated by systemd
  else
    exit(1);
}

