// Commands for controlling GPIO pins
//
// Copyright (C) 2016  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <stddef.h> // offsetof
#include "basecmd.h" // alloc_oid
#include "board/gpio.h" // struct gpio
#include "board/irq.h" // irq_save
#include "command.h" // DECL_COMMAND
#include "sched.h" // DECL_TASK


/****************************************************************
 * Digital out pins
 ****************************************************************/

struct digital_out_s {
    struct timer timer;
    struct gpio_out pin;
    uint32_t max_duration;
    uint8_t value, default_value;
};

static uint8_t
digital_end_event(struct timer *timer)
{
    shutdown("Missed scheduling of next pin event");
}

static uint8_t
digital_out_event(struct timer *timer)
{
    struct digital_out_s *d = container_of(timer, struct digital_out_s, timer);
    gpio_out_write(d->pin, d->value);
    if (d->value == d->default_value || !d->max_duration)
        return SF_DONE;
    d->timer.waketime += d->max_duration;
    d->timer.func = digital_end_event;
    return SF_RESCHEDULE;
}

void
command_config_digital_out(uint32_t *args)
{
    struct digital_out_s *d = alloc_oid(args[0], command_config_digital_out
                                        , sizeof(*d));
    d->default_value = args[2];
    d->pin = gpio_out_setup(args[1], d->default_value);
    d->max_duration = args[3];
}
DECL_COMMAND(command_config_digital_out,
             "config_digital_out oid=%c pin=%u default_value=%c"
             " max_duration=%u");

void
command_schedule_digital_out(uint32_t *args)
{
    struct digital_out_s *d = lookup_oid(args[0], command_config_digital_out);
    sched_del_timer(&d->timer);
    d->timer.func = digital_out_event;
    d->timer.waketime = args[1];
    d->value = args[2];
    sched_timer(&d->timer);
}
DECL_COMMAND(command_schedule_digital_out,
             "schedule_digital_out oid=%c clock=%u value=%c");

static void
digital_out_shutdown(void)
{
    uint8_t i;
    struct digital_out_s *d;
    foreach_oid(i, d, command_config_digital_out) {
        gpio_out_write(d->pin, d->default_value);
    }
}
DECL_SHUTDOWN(digital_out_shutdown);

void
command_set_digital_out(uint32_t *args)
{
    gpio_out_setup(args[0], args[1]);
}
DECL_COMMAND(command_set_digital_out, "set_digital_out pin=%u value=%c");


/****************************************************************
 * Hardware PWM pins
 ****************************************************************/

struct pwm_out_s {
    struct timer timer;
    struct gpio_pwm pin;
    uint32_t max_duration;
    uint8_t value, default_value;
};

static uint8_t
pwm_event(struct timer *timer)
{
    struct pwm_out_s *p = container_of(timer, struct pwm_out_s, timer);
    gpio_pwm_write(p->pin, p->value);
    if (p->value == p->default_value || !p->max_duration)
        return SF_DONE;
    p->timer.waketime += p->max_duration;
    p->timer.func = digital_end_event;
    return SF_RESCHEDULE;
}

void
command_config_pwm_out(uint32_t *args)
{
    struct pwm_out_s *p = alloc_oid(args[0], command_config_pwm_out, sizeof(*p));
    p->default_value = args[3];
    p->pin = gpio_pwm_setup(args[1], args[2], p->default_value);
    p->max_duration = args[4];
}
DECL_COMMAND(command_config_pwm_out,
             "config_pwm_out oid=%c pin=%u cycle_ticks=%u default_value=%c"
             " max_duration=%u");

void
command_schedule_pwm_out(uint32_t *args)
{
    struct pwm_out_s *p = lookup_oid(args[0], command_config_pwm_out);
    sched_del_timer(&p->timer);
    p->timer.func = pwm_event;
    p->timer.waketime = args[1];
    p->value = args[2];
    sched_timer(&p->timer);
}
DECL_COMMAND(command_schedule_pwm_out,
             "schedule_pwm_out oid=%c clock=%u value=%c");

static void
pwm_shutdown(void)
{
    uint8_t i;
    struct pwm_out_s *p;
    foreach_oid(i, p, command_config_pwm_out) {
        gpio_pwm_write(p->pin, p->default_value);
    }
}
DECL_SHUTDOWN(pwm_shutdown);

void
command_set_pwm_out(uint32_t *args)
{
    gpio_pwm_setup(args[0], args[1], args[2]);
}
DECL_COMMAND(command_set_pwm_out, "set_pwm_out pin=%u cycle_ticks=%u value=%c");


/****************************************************************
 * Soft PWM output pins
 ****************************************************************/

struct soft_pwm_s {
    struct timer timer;
    uint32_t on_duration, off_duration, end_time;
    uint32_t next_on_duration, next_off_duration;
    uint32_t max_duration, cycle_time, pulse_time;
    struct gpio_out pin;
    uint8_t default_value, flags;
};

enum {
    SPF_ON=1<<0, SPF_TOGGLING=1<<1, SPF_CHECK_END=1<<2, SPF_HAVE_NEXT=1<<3,
    SPF_NEXT_ON=1<<4, SPF_NEXT_TOGGLING=1<<5, SPF_NEXT_CHECK_END=1<<6,
};

static uint8_t soft_pwm_load_event(struct timer *timer);

// Normal pulse change event
static uint8_t
soft_pwm_toggle_event(struct timer *timer)
{
    struct soft_pwm_s *s = container_of(timer, struct soft_pwm_s, timer);
    gpio_out_toggle(s->pin);
    s->flags ^= SPF_ON;
    uint32_t waketime = s->timer.waketime;
    if (s->flags & SPF_ON)
        waketime += s->on_duration;
    else
        waketime += s->off_duration;
    if (s->flags & SPF_CHECK_END && !sched_is_before(waketime, s->end_time)) {
        // End of normal pulsing - next event loads new pwm settings
        s->timer.func = soft_pwm_load_event;
        waketime = s->end_time;
    }
    s->timer.waketime = waketime;
    return SF_RESCHEDULE;
}

// Load next pwm settings
static uint8_t
soft_pwm_load_event(struct timer *timer)
{
    struct soft_pwm_s *s = container_of(timer, struct soft_pwm_s, timer);
    if (!(s->flags & SPF_HAVE_NEXT))
        shutdown("Missed scheduling of next pwm event");
    uint8_t flags = s->flags >> 4;
    s->flags = flags;
    gpio_out_write(s->pin, flags & SPF_ON);
    if (!(flags & SPF_TOGGLING)) {
        // Pin is in an always on (value=255) or always off (value=0) state
        if (!(flags & SPF_CHECK_END))
            return SF_DONE;
        s->timer.waketime = s->end_time = s->end_time + s->max_duration;
        return SF_RESCHEDULE;
    }
    // Schedule normal pin toggle timer events
    s->timer.func = soft_pwm_toggle_event;
    s->off_duration = s->next_off_duration;
    s->on_duration = s->next_on_duration;
    s->timer.waketime = s->end_time + s->on_duration;
    s->end_time += s->max_duration;
    return SF_RESCHEDULE;
}

void
command_config_soft_pwm_out(uint32_t *args)
{
    struct soft_pwm_s *s = alloc_oid(args[0], command_config_soft_pwm_out
                                     , sizeof(*s));
    s->cycle_time = args[2];
    s->pulse_time = s->cycle_time / 255;
    s->default_value = !!args[3];
    s->max_duration = args[4];
    s->flags = s->default_value ? SPF_ON : 0;
    s->pin = gpio_out_setup(args[1], s->default_value);
}
DECL_COMMAND(command_config_soft_pwm_out,
             "config_soft_pwm_out oid=%c pin=%u cycle_ticks=%u default_value=%c"
             " max_duration=%u");

void
command_schedule_soft_pwm_out(uint32_t *args)
{
    struct soft_pwm_s *s = lookup_oid(args[0], command_config_soft_pwm_out);
    uint32_t time = args[1];
    uint8_t value = args[2];
    uint8_t next_flags = SPF_CHECK_END | SPF_HAVE_NEXT;
    uint32_t next_on_duration, next_off_duration;
    if (value == 0 || value == 255) {
        next_on_duration = next_off_duration = 0;
        next_flags |= value ? SPF_NEXT_ON : 0;
        if (!!value != s->default_value && s->max_duration)
            next_flags |= SPF_NEXT_CHECK_END;
    } else {
        next_on_duration = s->pulse_time * value;
        next_off_duration = s->cycle_time - next_on_duration;
        next_flags |= SPF_NEXT_ON | SPF_NEXT_TOGGLING;
        if (s->max_duration)
            next_flags |= SPF_NEXT_CHECK_END;
    }
    uint8_t flag = irq_save();
    if (s->flags & SPF_CHECK_END && sched_is_before(s->end_time, time))
        shutdown("next soft pwm extends existing pwm");
    s->end_time = time;
    s->next_on_duration = next_on_duration;
    s->next_off_duration = next_off_duration;
    s->flags |= next_flags;
    if (s->flags & SPF_TOGGLING && sched_is_before(s->timer.waketime, time)) {
        // soft_pwm_toggle_event() will schedule a load event when ready
    } else {
        // Schedule the loading of the pwm parameters at the requested time
        sched_del_timer(&s->timer);
        s->timer.waketime = time;
        s->timer.func = soft_pwm_load_event;
        sched_timer(&s->timer);
    }
    irq_restore(flag);
}
DECL_COMMAND(command_schedule_soft_pwm_out,
             "schedule_soft_pwm_out oid=%c clock=%u value=%c");

static void
soft_pwm_shutdown(void)
{
    uint8_t i;
    struct soft_pwm_s *s;
    foreach_oid(i, s, command_config_soft_pwm_out) {
        gpio_out_write(s->pin, s->default_value);
        s->flags = s->default_value ? SPF_ON : 0;
    }
}
DECL_SHUTDOWN(soft_pwm_shutdown);