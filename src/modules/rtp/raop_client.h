#ifndef fooraopclientfoo
#define fooraopclientfoo

/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2008 Colin Guthrie

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <pulse/mainloop-api.h>
#include <pulsecore/iochannel.h>
#include <pulsecore/core.h>

typedef struct pa_raop_client pa_raop_client;

pa_raop_client* pa_raop_client_new(pa_core *core, const char* host);
void pa_raop_client_free(pa_raop_client* c);

pa_memchunk pa_raop_client_encode_sample(pa_raop_client* c, pa_memchunk* raw);

typedef void (*pa_raop_client_cb_t)(int fd, void *userdata);
void pa_raop_client_set_callback(pa_raop_client* c, pa_raop_client_cb_t callback, void *userdata);

typedef void (*pa_raop_client_closed_cb_t)(void *userdata);
void pa_raop_client_set_closed_callback(pa_raop_client* c, pa_raop_client_closed_cb_t callback, void *userdata);

#endif
