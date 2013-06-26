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

#include <dyninst/BPatch.h>
#include <dyninst/BPatch_point.h>
#include <dyninst/BPatch_function.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
extern "C"{
#include "mutator-ust.h"
}

extern "C"{
int insert_dynamic_tp(struct lttng_event *event){
  char* bin = event->attr.dyn.bin_name;
  char* func = event->attr.dyn.func_name;
  char* var = event->attr.dyn.var_name;
  printf("Starting Instrumentation.. %s, %s, %s \n", bin, func, var);

  putenv("DYNINSTAPI_RT_LIB=/usr/lib64/dyninst/libdyninstAPI_RT.so");

  BPatch bpatch;
  BPatch_addressSpace *appAddrSpace;
  BPatch_binaryEdit *proc = bpatch.openBinary(bin);

  // Loads lib from system path so make sure this is there
  BPatch_object *ipa = proc->loadLibrary("dyntp.so");
  BPatch_image *image = proc->getImage();

  std::vector<BPatch_function *> fns, ipa_fns;
  image->findFunction(func, fns);
  image->findFunction("tpint", ipa_fns);

  //get variable from the mutator
  BPatch_variableExpr *v_ex;
  v_ex=image->findVariable(var);

  std::vector<BPatch_snippet*> tpintArgs;
  BPatch_snippet *s_var = v_ex;
  tpintArgs.push_back(s_var);
  BPatch_funcCallExpr call_ipb(*ipa_fns[0], tpintArgs);
  proc->insertSnippet(call_ipb, (fns[0]->findPoint(BPatch_entry))[0]);

  //for testing
  proc->writeFile("/tmp/instrumentedBin");

  return 1;
}
}
