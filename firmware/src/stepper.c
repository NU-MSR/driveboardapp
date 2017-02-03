/*
  stepper.c - stepper motor pulse generation
  Processes block from the queue generated by the planer and pulses
  steppers accordingly via a dynamically adapted timer interrupt.
  Part of DriveboardFirmware

  Copyright (c) 2011-2016 Stefan Hechenberger
  Copyright (c) 2009-2011 Simen Svale Skogsrud
  Copyright (c) 2011 Sungeun K. Jeon

  Inspired by the 'RepRap cartesian firmware' by Zack Smith and
  Philipp Tiefenbacher.

  DriveboardFirmware is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  DriveboardFirmware is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  ---

           __________________________
          /|                        |\     _________________         ^
         / |                        | \   /|               |\        |
        /  |                        |  \ / |               | \       s
       /   |                        |   |  |               |  \      p
      /    |                        |   |  |               |   \     e
     +-----+------------------------+---+--+---------------+----+    e
     |               BLOCK 1            |      BLOCK 2          |    d

                             time ----->

  The speed profile starts at block->initial_rate, accelerates by block->rate_delta
  during the first block->accelerate_until step_events_completed, then keeps going at constant speed until
  step_events_completed reaches block->decelerate_after after which it decelerates until final_rate is reached.
  The slope of acceleration is always +/- block->rate_delta and is applied at a constant rate following the midpoint rule.
  Speed adjustments are made ACCELERATION_TICKS_PER_SECOND times per second.
*/

#define __DELAY_BACKWARD_COMPATIBLE__  // _delay_us() make backward compatible see delay.h

#include <math.h>
#include <stdlib.h>
#include <util/delay.h>
#include <avr/interrupt.h>
#include <string.h>
#include "stepper.h"
#include "config.h"
#include "protocol.h"
#include "planner.h"
#include "sense_control.h"
#include "serial.h"  //for debug


#define CYCLES_PER_MINUTE (60*F_CPU)  // 960000000
#define CYCLES_PER_MICROSECOND (F_CPU/1000000)  //16000000/1000000 = 16
#define CYCLES_PER_ACCELERATION_TICK (F_CPU/ACCELERATION_TICKS_PER_SECOND)  // 16MHz/100 = 160000


static int32_t stepper_position[3];  // real-time position in absolute steps
static block_t *current_block;  // A pointer to the block currently being traced

// Variables used by The Stepper Driver Interrupt
static uint8_t out_bits;       // The next stepping-bits to be output
static int32_t counter_x,       // Counter variables for the bresenham line tracer
               counter_y,
               counter_z;
static uint32_t step_events_completed; // The number of step events executed in the current block
static volatile bool busy;  // true when stepper ISR is in already running

// Variables used by the trapezoid generation
static uint32_t cycles_per_step_event;        // The number of machine cycles between each step event
static uint32_t acceleration_tick_counter;    // The cycles since last acceleration_tick.
                                              // Used to generate ticks at a steady pace without allocating a separate timer.
static uint32_t adjusted_rate;                // The current rate of step_events according to the speed profile
static volatile bool processing_flag;         // indicates if blocks are being processed
static volatile bool stop_requested;          // when set to true stepper interrupt will go idle on next entry
static volatile uint8_t stop_status;          // yields the reason for a stop request

#ifndef STATIC_PWM_FREQ
  static volatile uint8_t pwm_counter = 1;
#endif

// prototypes for static functions (non-accesible from other files)
static bool acceleration_tick();
static void adjust_speed( uint32_t steps_per_minute );
static void adjust_beam_dynamics( uint32_t steps_per_minute );
static uint32_t config_step_timer(uint32_t cycles);


// Initialize and start the stepper motor subsystem
void stepper_init() {
  // Configure directions of interface pins
  STEPPING_DDR |= (STEPPING_MASK | DIRECTION_MASK);
  STEPPING_PORT = (STEPPING_PORT & ~(STEPPING_MASK | DIRECTION_MASK)) | INVERT_MASK;

  // waveform generation = 0100 = CTC
  TCCR1B &= ~(1<<WGM13);
  TCCR1B |= (1<<WGM12);
  TCCR1A &= ~(1<<WGM11);
  TCCR1A &= ~(1<<WGM10);

  // output mode = 00 (disconnected)
  TCCR1A &= ~(3<<COM1A0);
  TCCR1A &= ~(3<<COM1B0);

  // Configure Timer 2
  TCCR2A = 0; // Normal operation
  TCCR2B = 0; // Disable timer until needed.
  TIMSK2 |= (1<<TOIE2); // Enable Timer2 interrupt flag

  adjust_speed(MINIMUM_STEPS_PER_MINUTE);
  control_laser_intensity(0);
  clear_vector(stepper_position);
  stepper_set_position( CONFIG_X_ORIGIN_OFFSET,
                        CONFIG_Y_ORIGIN_OFFSET,
                        CONFIG_Z_ORIGIN_OFFSET );
  acceleration_tick_counter = 0;
  current_block = NULL;
  stop_requested = false;
  stop_status = STOPERROR_OK;
  busy = false;

  // start in the idle state
  // The stepper interrupt gets started when blocks are being added.
  stepper_stop_processing();
}


// start processing command blocks
void stepper_start_processing() {
  if (!processing_flag) {
    processing_flag = true;
    // Initialize stepper output bits
    out_bits = INVERT_MASK;
    // Enable stepper driver interrupt
    TIMSK1 |= (1<<OCIE1A);
  }
}

// stop processing command blocks
void stepper_stop_processing() {
  processing_flag = false;
  current_block = NULL;
  // Disable stepper driver interrupt
  TIMSK1 &= ~(1<<OCIE1A);
  control_laser_intensity(0);
}

// is the stepper interrupt processing
bool stepper_processing() {
  return processing_flag;
}

// stop event handling
void stepper_request_stop(uint8_t status) {
  if (!stop_requested) {  // prevent retriggering
    stop_status = status;
    stop_requested = true;
    serial_stop();
  }
}

uint8_t stepper_stop_status() {
  return stop_status;
}

bool stepper_stop_requested() {
  return stop_requested;
}

void stepper_stop_resume() {
  stop_status = STOPERROR_OK;
  stop_requested = false;
}




double stepper_get_position_x() {
  return stepper_position[X_AXIS]/CONFIG_X_STEPS_PER_MM;
}
double stepper_get_position_y() {
  return stepper_position[Y_AXIS]/CONFIG_Y_STEPS_PER_MM;
}
double stepper_get_position_z() {
  return stepper_position[Z_AXIS]/CONFIG_Z_STEPS_PER_MM;
}
void stepper_set_position(double x, double y, double z) {
  stepper_position[X_AXIS] = lround(x*CONFIG_X_STEPS_PER_MM);
  stepper_position[Y_AXIS] = lround(y*CONFIG_Y_STEPS_PER_MM);
  stepper_position[Z_AXIS] = lround(z*CONFIG_Z_STEPS_PER_MM);
}



// The PWM Reset ISR
// TIMER0 overflow interrupt service routine
// called whenever TCNT0 overflows
ISR(TIMER0_OVF_vect) {
  ASSIST_PORT &= ~(1 << LASER_PWM_BIT); // off
  TCCR0B = 0;  // disable
}


// The Stepper Reset ISR
// It resets the motor port after a short period completing one step cycle.
// TODO: It is possible for the serial interrupts to delay this interrupt by a few microseconds, if
// they execute right before this interrupt. Not a big deal, but could use some TLC at some point.
ISR(TIMER2_OVF_vect) {
  // reset step pins
  STEPPING_PORT = (STEPPING_PORT & ~STEPPING_MASK) | (INVERT_MASK & STEPPING_MASK);
  TCCR2B = 0; // Disable Timer2 to prevent re-entering this interrupt when it's not needed.
}


// The Stepper ISR
// This is the workhorse of DriveboardFirmware. It is executed at the rate set with
// config_step_timer. It pops blocks from the block_buffer and executes them by pulsing the stepper pins appropriately.
// The bresenham line tracer algorithm controls all three stepper outputs simultaneously.
ISR(TIMER1_COMPA_vect) {
  if (busy) { return; } // The busy-flag is used to avoid reentering this interrupt
  busy = true;
  if (stop_requested) {
    // go idle
    stepper_stop_processing();
    // absorb blocks
    // Make sure to do this again from the protocol loop
    // because it could still be adding blocks after this call.
    planner_reset_block_buffer();
    busy = false;
    return;
  }

  #ifdef ENABLE_LASER_INTERLOCKS
    // honor interlocks
    // (for unlikely edge case the protocol loop stops)
    if (SENSE_DOOR_OPEN || SENSE_CHILLER_OFF) {
      control_laser_intensity(0);
    }
    // stop program when any limit is hit
    if (SENSE_X1_LIMIT) {
      stepper_request_stop(STOPERROR_LIMIT_HIT_X1);
      busy = false;
      return;
    } else if (SENSE_X2_LIMIT) {
      stepper_request_stop(STOPERROR_LIMIT_HIT_X2);
      busy = false;
      return;
    } else if (SENSE_Y1_LIMIT) {
      stepper_request_stop(STOPERROR_LIMIT_HIT_Y1);
      busy = false;
      return;
    }else if (SENSE_Y2_LIMIT) {
      stepper_request_stop(STOPERROR_LIMIT_HIT_Y2);
      busy = false;
      return;
    }
    #ifdef ENABLE_3AXES
      else if (SENSE_Z1_LIMIT && ENABLE_3AXES) {
        stepper_request_stop(STOPERROR_LIMIT_HIT_Z1);
        busy = false;
        return;
      } else if (SENSE_Z2_LIMIT && ENABLE_3AXES) {
        stepper_request_stop(STOPERROR_LIMIT_HIT_Z2);
        busy = false;
        return;
      }
    #endif
  #endif

  #ifndef STATIC_PWM_FREQ
    // pulse laser
    uint8_t duty = control_get_intensity();
    if (pwm_counter < CONFIG_BEAMDYNAMICS_EVERY) {
      pwm_counter += 1;
    } else {
      // generate pulse
      if (duty == 0) {
        ASSIST_PORT &= ~(1 << LASER_PWM_BIT); // off
      } else {
        TCCR0B = 0;
        ASSIST_PORT |= (1 << LASER_PWM_BIT);  // on
        // set timer0 for reset
        // maximum is 0.01632s (261120 cycles)
        // may limit pulse duration on very slow moves
        if (duty < 242) {  // TODO: osci-test again for higher values, for now just leave at 100%/full duty cycle
          uint32_t cycles = CONFIG_BEAMDYNAMICS_EVERY*duty*(cycles_per_step_event >> 8);
          uint8_t prescaler = 0;
          if(cycles < 256) { prescaler |= _BV(CS00); }                            // no prescale, full xtal
          else if((cycles >>= 3) < 256) { prescaler |= _BV(CS01); }               // prescale by /8
          else if((cycles >>= 3) < 256) { prescaler |= _BV(CS01) | _BV(CS00); }   // prescale by /64
          else if((cycles >>= 2) < 256) { prescaler |= _BV(CS02); }               // prescale by /256
          else if((cycles >>= 2) < 256) { prescaler |= _BV(CS02) | _BV(CS00); }   // prescale by /1024
          else { cycles = 255, prescaler |= _BV(CS02) | _BV(CS00); }              // over 261120 cycles, set as maximum
          TCNT0 = 256-cycles;  // isr is triggered when overflowing
          TCCR0B = prescaler;
        }
      }
      pwm_counter = 1;
    }
  #endif

  // pulse steppers
  STEPPING_PORT = (STEPPING_PORT & ~DIRECTION_MASK) | (out_bits & DIRECTION_MASK);
  STEPPING_PORT = (STEPPING_PORT & ~STEPPING_MASK) | out_bits;
  // prime for reset pulse in CONFIG_PULSE_MICROSECONDS
  TCNT2 = -(((CONFIG_PULSE_MICROSECONDS-2)*CYCLES_PER_MICROSECOND) >> 3); // Reload timer counter
  TCCR2B = (1<<CS21); // Begin timer2. Full speed, 1/8 prescaler

  // Enable nested interrupts.
  // By default nested interrupts are disabled but can be enabled with sei()
  // This allows the reset interrupt and serial ISRs to jump in.
  // See: http://avr-libc.nongnu.org/user-manual/group__avr__interrupts.html
  sei();

  // If there is no current block, attempt to pop one from the buffer
  if (current_block == NULL) {
    // Anything in the buffer?
    current_block = planner_get_current_block();
    // if still no block command, go idle, disable interrupt
    if (current_block == NULL) {
      stepper_stop_processing();
      busy = false;
      return;
    }
    if (current_block->type == TYPE_LINE || current_block->type == TYPE_RASTER_LINE) {
      // starting on new line block
      adjusted_rate = current_block->initial_rate;
      acceleration_tick_counter = CYCLES_PER_ACCELERATION_TICK/2; // start halfway, midpoint rule.
      adjust_speed( adjusted_rate ); // initialize cycles_per_step_event
      if (current_block->type == TYPE_RASTER_LINE) {
        control_laser_intensity(0);  // set only through raster data
      } else {
        adjust_beam_dynamics(adjusted_rate);
      }
      counter_x = -(current_block->step_event_count >> 1);
      counter_y = counter_x;
      counter_z = counter_x;
      step_events_completed = 0;
    }
  }

  // process current block, populate out_bits (or handle other commands)
  switch (current_block->type) {
    case TYPE_LINE: case TYPE_RASTER_LINE:
      ////// Execute step displacement profile by bresenham line algorithm
      out_bits = current_block->direction_bits;
      counter_x += current_block->steps_x;
      if (counter_x > 0) {
        out_bits |= (1<<X_STEP_BIT);
        counter_x -= current_block->step_event_count;
        // also keep track of absolute position
        if ((out_bits >> X_DIRECTION_BIT) & 1 ) {
          stepper_position[X_AXIS] -= 1;
        } else {
          stepper_position[X_AXIS] += 1;
        }
      }
      counter_y += current_block->steps_y;
      if (counter_y > 0) {
        out_bits |= (1<<Y_STEP_BIT);
        counter_y -= current_block->step_event_count;
        // also keep track of absolute position
        if ((out_bits >> Y_DIRECTION_BIT) & 1 ) {
          stepper_position[Y_AXIS] -= 1;
        } else {
          stepper_position[Y_AXIS] += 1;
        }
      }
      counter_z += current_block->steps_z;
      if (counter_z > 0) {
        out_bits |= (1<<Z_STEP_BIT);
        counter_z -= current_block->step_event_count;
        // also keep track of absolute position
        if ((out_bits >> Z_DIRECTION_BIT) & 1 ) {
          stepper_position[Z_AXIS] -= 1;
        } else {
          stepper_position[Z_AXIS] += 1;
        }
      }
      //////

      step_events_completed++;  // increment step count

      // apply stepper invert mask
      out_bits ^= INVERT_MASK;

      ////////// SPEED ADJUSTMENT
      if (step_events_completed < current_block->step_event_count) {  // block not finished

        // accelerating
        if (step_events_completed < current_block->accelerate_until) {
          if ( acceleration_tick() ) {  // scheduled speed change
            adjusted_rate += current_block->rate_delta;
            if (adjusted_rate > current_block->nominal_rate) {  // overshot
              adjusted_rate = current_block->nominal_rate;
            }
            adjust_speed( adjusted_rate );
            if (current_block->type == TYPE_RASTER_LINE) {
              control_laser_intensity(0);  // set only through raster data
            } else {
              adjust_beam_dynamics(adjusted_rate);
            }
          }

        // deceleration start
        } else if (step_events_completed == current_block->decelerate_after) {
            // reset counter, midpoint rule
            // makes sure deceleration is performed the same every time
            acceleration_tick_counter = CYCLES_PER_ACCELERATION_TICK/2;

        // decelerating
        } else if (step_events_completed >= current_block->decelerate_after) {
          if ( acceleration_tick() ) {  // scheduled speed change
            if (adjusted_rate > current_block->rate_delta) {
              adjusted_rate -= current_block->rate_delta;
            } else {
              adjusted_rate = 0; // unsigned
            }
            if (adjusted_rate < current_block->final_rate) {  // overshot
              adjusted_rate = current_block->final_rate;
            }
            adjust_speed( adjusted_rate );
            if (current_block->type == TYPE_RASTER_LINE) {
              control_laser_intensity(0);  // set only through raster data
            } else {
              adjust_beam_dynamics(adjusted_rate);
            }
          }

        // cruising
        } else {
          // No accelerations. Make sure we cruise exactly at the nominal rate.
          if (adjusted_rate != current_block->nominal_rate) {
            adjusted_rate = current_block->nominal_rate;
            adjust_speed( adjusted_rate );
            if (current_block->type == TYPE_RASTER_LINE) {
              control_laser_intensity(0);  // set only through raster data
            } else {
              adjust_beam_dynamics(adjusted_rate);
            }
          }
          // Special case raster line.
          // Adjust intensity according raster buffer.
          if (current_block->type == TYPE_RASTER_LINE) {
            if ((step_events_completed % current_block->pixel_steps) == 0) {
              // for every pixel width get the next raster value
              // disable nested interrupts
              // this is to prevent race conditions with the serial interrupt
              // over the rx_buffer variables.
              cli();
              uint8_t chr = serial_raster_read();
              sei();
              // map [128,255] -> [0, nominal_laser_intensity]
              // (chr-128)*2 * (current_block->nominal_laser_intensity/255)
              control_laser_intensity( (chr-128)*2*current_block->nominal_laser_intensity/255 );
            }
          }
        }
      } else {  // block finished
        if (current_block->type == TYPE_RASTER_LINE) {
          // make sure all raster data is consumed
          serial_consume_data();
        }
        current_block = NULL;
        planner_discard_current_block();
      }
      ////////// END OF SPEED ADJUSTMENT

      break;

    case TYPE_AIR_ASSIST_ENABLE:
      control_air_assist(true);
      current_block = NULL;
      planner_discard_current_block();
      break;

    case TYPE_AIR_ASSIST_DISABLE:
      control_air_assist(false);
      current_block = NULL;
      planner_discard_current_block();
      break;

#ifndef DRIVEBOARD_USB
    case TYPE_AUX1_ASSIST_ENABLE:
      control_aux1_assist(true);
      current_block = NULL;
      planner_discard_current_block();
      break;

    case TYPE_AUX1_ASSIST_DISABLE:
      control_aux1_assist(false);
      current_block = NULL;
      planner_discard_current_block();
      break;

    case TYPE_AUX2_ASSIST_ENABLE:
      control_aux2_assist(true);
      current_block = NULL;
      planner_discard_current_block();
      break;

    case TYPE_AUX2_ASSIST_DISABLE:
      control_aux2_assist(false);
      current_block = NULL;
      planner_discard_current_block();
      break;
#endif
  }

  busy = false;
}




// This function determines an acceleration velocity change every CYCLES_PER_ACCELERATION_TICK by
// keeping track of the number of elapsed cycles during a de/ac-celeration. The code assumes that
// step_events occur significantly more often than the acceleration velocity iterations.
inline bool acceleration_tick() {
  acceleration_tick_counter += cycles_per_step_event;
  if(acceleration_tick_counter > CYCLES_PER_ACCELERATION_TICK) {
    acceleration_tick_counter -= CYCLES_PER_ACCELERATION_TICK;
    return true;
  } else {
    return false;
  }
}


// Configures the prescaler and ceiling of timer 1 to produce the given rate as accurately as possible.
// Returns the actual number of cycles per interrupt
inline uint32_t config_step_timer(uint32_t cycles) {
  uint16_t ceiling;
  uint16_t prescaler;
  uint32_t actual_cycles;
  if (cycles <= 0xffffL) {
    ceiling = cycles;
    prescaler = 0; // prescaler: 0
    actual_cycles = ceiling;
  } else if (cycles <= 0x7ffffL) {
    ceiling = cycles >> 3;
    prescaler = 1; // prescaler: 8
    actual_cycles = ceiling * 8L;
  } else if (cycles <= 0x3fffffL) {
    ceiling = cycles >> 6;
    prescaler = 2; // prescaler: 64
    actual_cycles = ceiling * 64L;
  } else if (cycles <= 0xffffffL) {
    ceiling = (cycles >> 8);
    prescaler = 3; // prescaler: 256
    actual_cycles = ceiling * 256L;
  } else if (cycles <= 0x3ffffffL) {
    ceiling = (cycles >> 10);
    prescaler = 4; // prescaler: 1024
    actual_cycles = ceiling * 1024L;
  } else {
    // Okay, that was slower than we actually go. Just set the slowest speed
    ceiling = 0xffff;
    prescaler = 4;
    actual_cycles = 0xffff * 1024;
  }
  // Set prescaler
  TCCR1B = (TCCR1B & ~(0x07<<CS10)) | ((prescaler+1)<<CS10);
  // Set ceiling
  OCR1A = ceiling;
  return(actual_cycles);
}


inline void adjust_speed( uint32_t steps_per_minute ) {
  // steps_per_minute is typicaly just adjusted_rate
  if (steps_per_minute < MINIMUM_STEPS_PER_MINUTE) { steps_per_minute = MINIMUM_STEPS_PER_MINUTE; }
  cycles_per_step_event = config_step_timer(CYCLES_PER_MINUTE/steps_per_minute);
}


inline void adjust_beam_dynamics( uint32_t steps_per_minute ) {
  // Adjust intensity with speed.
  // Laser pulses are triggered along with motion steps (freq linked to speed).
  // Additional progressive dimming with increasing intensity is added here.
  // map intensity [0,255] -> [CONFIG_PWM_DIMM_OFFSET, 1.0]
  float dimm = CONFIG_BEAMDYNAMICS_START+(((1.0-CONFIG_BEAMDYNAMICS_START) *
                 (float)current_block->nominal_laser_intensity)/255.0);
  // actual dimming function, (1-d) + (d * slowdown_factor)
  uint8_t adjusted_intensity = current_block->nominal_laser_intensity *
                 ((1.0-dimm) + dimm*(((float)steps_per_minute/
                 (float)current_block->nominal_rate)));
  // adjusted_intensity = max(adjusted_intensity, 0);
  control_laser_intensity(adjusted_intensity);
}





inline static void homing_cycle(bool x_axis, bool y_axis, bool z_axis, bool reverse_direction, uint32_t microseconds_per_pulse) {

  uint32_t step_delay = microseconds_per_pulse - CONFIG_PULSE_MICROSECONDS;
  uint8_t out_bits = DIRECTION_MASK;
  uint8_t limit_bits;
  uint8_t x_overshoot_count = 6;
  uint8_t y_overshoot_count = 6;
  uint8_t z_overshoot_count = 6;

  if (x_axis) { out_bits |= (1<<X_STEP_BIT); }
  if (y_axis) { out_bits |= (1<<Y_STEP_BIT); }
  if (z_axis) { out_bits |= (1<<Z_STEP_BIT); }

  // Invert direction bits if this is a reverse homing_cycle
  if (reverse_direction) {
    out_bits ^= DIRECTION_MASK;
  }

  // Apply the global invert mask
  out_bits ^= INVERT_MASK;

  // Set direction pins
  STEPPING_PORT = (STEPPING_PORT & ~DIRECTION_MASK) | (out_bits & DIRECTION_MASK);

  for(;;) {
    limit_bits = LIMIT_PIN;
    if (reverse_direction) {
      // Invert limit_bits if this is a reverse homing_cycle
      limit_bits ^= LIMIT_MASK;
    }

    #ifdef DRIVEBOARD_USB
      bool sense_x1_limit = (limit_bits & (1<<X1_LIMIT_BIT));
      bool sense_y1_limit = (limit_bits & (1<<Y1_LIMIT_BIT));
      bool sense_z1_limit = (limit_bits & (1<<Z1_LIMIT_BIT));
    #else
      bool sense_x1_limit = !(limit_bits & (1<<X1_LIMIT_BIT));
      bool sense_y1_limit = !(limit_bits & (1<<Y1_LIMIT_BIT));
      bool sense_z1_limit = !(limit_bits & (1<<Z1_LIMIT_BIT));
    #endif

    if (x_axis && sense_x1_limit) {
      if(x_overshoot_count == 0) {
        x_axis = false;
        out_bits ^= (1<<X_STEP_BIT);
      } else {
        x_overshoot_count--;
      }
    }
    if (y_axis && sense_y1_limit) {
      if(y_overshoot_count == 0) {
        y_axis = false;
        out_bits ^= (1<<Y_STEP_BIT);
      } else {
        y_overshoot_count--;
      }
    }
    #ifdef ENABLE_3AXES
    if (z_axis && sense_z1_limit) {
      if(z_overshoot_count == 0) {
        z_axis = false;
        out_bits ^= (1<<Z_STEP_BIT);
      } else {
        z_overshoot_count--;
      }
    }
    #endif
    if(x_axis || y_axis || z_axis) {
        // step all axes still in out_bits
        STEPPING_PORT |= out_bits & STEPPING_MASK;
        _delay_us(CONFIG_PULSE_MICROSECONDS);
        STEPPING_PORT ^= out_bits & STEPPING_MASK;
        _delay_us(step_delay);
    } else {
        break;
    }
  }
  clear_vector(stepper_position);
  return;
}


inline void stepper_homing_cycle() {
  // home the x and y axis
  #ifdef ENABLE_3AXES
  // approach limit
  homing_cycle(true, true, true, false, CONFIG_HOMINGRATE);
  // leave limit
  homing_cycle(true, true, true, true, CONFIG_HOMINGRATE);
  #else
  // approach limit
  homing_cycle(true, true, false, false, CONFIG_HOMINGRATE);
  // leave limit
  homing_cycle(true, true, false, true, CONFIG_HOMINGRATE);
  #endif
}
