// General purpose linear demodulator for ka9q-radio
// Handles USB/IQ/CW/etc, all modes but FM
// Copyright May 2022-2023 Phil Karn, KA9Q

#define DEFAULT_SHIFT (0.0)          // Post detection frequency shift, Hz
#define DEFAULT_HEADROOM (-10.0)     // Target average output level, dBFS
#define DEFAULT_HANGTIME (1.1)       // AGC gain hang time, sec
#define DEFAULT_RECOVERY_RATE (20.0)  // AGC recovery rate after hang expiration, dB/s
#define DEFAULT_GAIN (0.)           // Linear gain, dB
#define DEFAULT_THRESHOLD (-15.0)     // AGC threshold, dB (noise will be at HEADROOM + THRESHOLD)
#define DEFAULT_PLL_DAMPING (M_SQRT1_2); // PLL loop damping factor; 1/sqrt(2) is "critical" damping
#define DEFAULT_PLL_LOCKTIME (.05);  // time, sec PLL stays above/below threshold SNR to lock/unlock

#define _GNU_SOURCE 1
#include <assert.h>
#include <complex.h>
#include <math.h>
#include <fftw3.h>
#include <pthread.h>
#include <string.h>
#include <stdbool.h>

#include "misc.h"
#include "filter.h"
#include "radio.h"

void *demod_linear(void *arg){
  assert(arg != NULL);
  struct channel * const chan = arg;

  {
    char name[100];
    snprintf(name,sizeof(name),"lin %u",chan->output.rtp.ssrc);
    pthread_setname(name);
  }
  pthread_mutex_init(&chan->status.lock,NULL);
  pthread_mutex_lock(&chan->status.lock);
  FREE(chan->status.command);
  FREE(chan->filter.energies);
  FREE(chan->spectrum.bin_data);
  if(chan->output.opus != NULL){
    opus_encoder_destroy(chan->output.opus);
    chan->output.opus = NULL;
  }

  int const blocksize = chan->output.samprate * Blocktime / 1000;
  delete_filter_output(&chan->filter.out);
  create_filter_output(&chan->filter.out,&Frontend.in,NULL,blocksize,COMPLEX);
  pthread_mutex_unlock(&chan->status.lock);

  set_filter(&chan->filter.out,
	     chan->filter.min_IF/chan->output.samprate,
	     chan->filter.max_IF/chan->output.samprate,
	     chan->filter.kaiser_beta);
  
  // Coherent mode parameters
  float const damping = DEFAULT_PLL_DAMPING;
  float const lock_time = DEFAULT_PLL_LOCKTIME;

  int const lock_limit = lock_time * chan->output.samprate;
  init_pll(&chan->pll.pll,(float)chan->output.samprate);

  realtime();

  while(downconvert(chan) == 0){
    int const N = chan->filter.out.olen; // Number of raw samples in filter output buffer

    // First pass over sample block.
    // Run the PLL (if enabled)
    // Apply post-downconversion shift (if enabled, e.g. for CW)
    // Measure energy
    // Apply PLL & frequency shift, measure energy
    complex float * const buffer = chan->filter.out.output.c; // Working buffer
    float signal = 0; // PLL only
    float noise = 0;  // PLL only

    if(chan->linear.pll){
      // Update PLL state, if active
      if(!chan->pll.was_on){
	// Just turned on, reset stuff
	chan->linear.rotations = 0;
	chan->pll.pll.integrator = 0; // reset oscillator when coming back on
	chan->pll.was_on = true;
      }
      set_pll_params(&chan->pll.pll,chan->linear.loop_bw,damping);
      for(int n=0; n<N; n++){
	complex float const s = buffer[n] *= conjf(pll_phasor(&chan->pll.pll));
	float phase;
	if(chan->linear.square){
	  phase = cargf(s*s);
	} else {
	  phase = cargf(s);
	}
	run_pll(&chan->pll.pll,phase);
	signal += crealf(s) * crealf(s); // signal in phase with VCO is signal + noise power
	noise += cimagf(s) * cimagf(s);  // signal in quadrature with VCO is assumed to be noise power
      }
      if(noise != 0){
	chan->sig.snr = (signal / noise) - 1; // S/N as power ratio; meaningful only in coherent modes
	if(chan->sig.snr < 0)
	  chan->sig.snr = 0; // Clamp to 0 so it'll show as -Inf dB
      } else
	chan->sig.snr = NAN;

      // Loop lock detector with hysteresis
      // If there's more I signal than Q signal, declare it locked
      // The squelch settings are really for FM, not for us
      if(chan->sig.snr < chan->fm.squelch_close){
	chan->pll.lock_count -= N;
	if(chan->pll.lock_count <= -lock_limit){
	  chan->pll.lock_count = -lock_limit;
	  chan->linear.pll_lock = false;
	}
      } else if(chan->sig.snr > chan->fm.squelch_open){
	chan->pll.lock_count += N;
	if(chan->pll.lock_count >= lock_limit){
	  chan->pll.lock_count = lock_limit;
	  chan->linear.pll_lock = true;
	}
      }
      chan->linear.lock_timer = chan->pll.lock_count;
      double phase = carg(pll_phasor(&chan->pll.pll));

      double phase_diff = phase - chan->linear.cphase;
      chan->linear.cphase = phase;
      if(phase_diff > M_PI){
	chan->linear.rotations--;
      } else if(phase_diff < -M_PI){
	chan->linear.rotations++;
      }
      chan->sig.foffset = pll_freq(&chan->pll.pll);
    } else { // if PLL
      chan->pll.was_on = false;
    }
    // Apply frequency shift
    // Must be done after PLL, which operates only on DC
    set_osc(&chan->shift,chan->tune.shift/chan->output.samprate,0);
    if(chan->shift.freq != 0){
      for(int n=0; n < N; n++){
	buffer[n] *= step_osc(&chan->shift);
      }
    }
 
    // Run AGC on a block basis to do some forward averaging
    // Lots of people seem to have strong opinions on how AGCs should work
    // so there's probably a lot of work to do here
    float gain_change = 1; // default to constant gain
    if(chan->linear.agc){
      float const bw = fabsf(chan->filter.min_IF - chan->filter.max_IF);
      float const bn = sqrtf(bw * chan->sig.n0); // Noise amplitude
      float const ampl = sqrtf(chan->sig.bb_power);

      // per-sample gain change is required to avoid sudden gain changes at block boundaries that can
      // cause clicks and pops when a strong signal straddles a block boundary
      // the new gain setting is applied exponentially over the block
      // gain_change is per sample and close to 1, so be careful with numerical precision!
      if(ampl * chan->output.gain > chan->output.headroom){
	// Strong signal, reduce gain
	// Don't do it instantly, but by the end of this block
	float const newgain = chan->output.headroom / ampl;
	// N-th root of newgain / gain
	// Should this be in double precision to avoid imprecision when gain = - epsilon dB?
	if(newgain > 0)
	  gain_change = powf(newgain/chan->output.gain, 1.0F/N);
	assert(gain_change != 0);
	chan->hangcount = chan->linear.hangtime;
      } else if(bn * chan->output.gain > chan->linear.threshold * chan->output.headroom){
	// Reduce gain to keep noise < threshold, same as for strong signal
	float const newgain = chan->linear.threshold * chan->output.headroom / bn;
	if(newgain > 0)
	  gain_change = powf(newgain/chan->output.gain, 1.0F/N);
	assert(gain_change != 0);
      } else if(chan->hangcount > 0){
	// Waiting for AGC hang time to expire before increasing gain
	gain_change = 1; // Constant gain
	chan->hangcount--;
      } else {
	// Allow gain to increase at configured rate, e.g. 20 dB/s
	gain_change = powf(chan->linear.recovery_rate, 1.0F/N);
	assert(gain_change != 0);
      }
    }
    // Accumulate sum of square gains, for averaging in status
    float start_gain = chan->output.gain;

    // Final pass over signal block
    // Demodulate, apply gain changes, compute output energy
    float output_power = 0;
    if(chan->output.channels == 1){
      // Complex input buffer is I0 Q0 I1 Q1 ...
      // Real output will be R0 R1 R2 R3 ...
      // Help cache use by overlaying output on input; ok as long as we index it from low to high
      float *samples = (float *)buffer;
      if(chan->linear.env){
	// AM envelope detection
	for(int n=0; n < N; n++){
	  samples[n] = cabsf(buffer[n]) * chan->output.gain;
	  output_power += samples[n] * samples[n];
	  chan->output.gain *= gain_change;
	}
      } else {
	// I channel only (SSB, CW, etc)
	for(int n=0; n < N; n++){
	  samples[n] = crealf(buffer[n]) * chan->output.gain;
	  output_power += samples[n] * samples[n];
	  chan->output.gain *= gain_change;
	}
      }
    } else {
      // Complex input buffer is I0 Q0 I1 Q1 ...
      // Real output will be L0 R0 L1 R1  ...
      // Overlay input with output
      if(chan->linear.env){
	// I on left, envelope/AM on right (for experiments in fine SSB tuning)
	for(int n=0; n < N; n++){      
	  __imag__ buffer[n] = cabsf(buffer[n]) * 2; // empirical +6dB for AM to match SSB
	  buffer[n] *= chan->output.gain;
	  output_power += cnrmf(buffer[n]);
	  chan->output.gain *= gain_change;
	}
      } else {
	// Simplest case: I/Q output with I on left, Q on right
	for(int n=0; n < N; n++){      
	  buffer[n] *= chan->output.gain;
	  output_power += cnrmf(buffer[n]);
	  chan->output.gain *= gain_change;
	}
      }
    }
    output_power /= N; // Per sample
    if(chan->output.channels == 1)
      output_power *= 2; // +3 dB for mono since 0 dBFS = 1 unit peak, not RMS
    chan->output.energy += output_power;
    // Mute if no signal (e.g., outside front end coverage)
    // or if no PLL lock (AM squelch)
    // or if zero frequency
    bool mute = (output_power == 0) || (chan->linear.pll && !chan->linear.pll_lock) || (chan->tune.freq == 0);

    // send_output() knows if the buffer is mono or stereo
    if(send_output(chan,(float *)buffer,N,mute) == -1)
      break; // No output stream!

    // When the gain is allowed to vary, the average gain won't be exactly consistent with the
    // average baseband (input) and output powers. But I still try to make it meaningful.
    chan->output.sum_gain_sq += start_gain * chan->output.gain; // accumulate square of approx average gain
  }
  return NULL;
}
