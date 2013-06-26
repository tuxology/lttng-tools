/*
 * Copyright (C) 2013 - Suchakra Sharma <suchakrapani.sharma@polymtl.ca>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef _LTT_MUTATOR_UST_H
#define _LTT_MUTATOR_UST_H


#include <config.h>
#include <limits.h>
#include <urcu/list.h>

#include <lttng/lttng.h>
//#include <common/hashtable/hashtable.h>
//#include <common/defaults.h>

//#include "consumer.h"
//#include "ust-ctl.h"

/*Types of dynamic tracepoints*/
enum dynamic_tracepoint_type{
  DYN_TP_INT        = 0,
  DYN_TP_STRING     = 1,
  DYN_TP_FLOAT      = 2,
};

int insert_dynamic_tp(struct lttng_event *);
int swap_binary(void);


#endif /* _LTT_MUTATOR_UST_H */
