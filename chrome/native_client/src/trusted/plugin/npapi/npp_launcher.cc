/*
 * Copyright 2008 The Native Client Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can
 * be found in the LICENSE file.
 */


/**
  * @file
  * Defines the basic API for the nacl browser plugin
  *
  * http://www.mozilla.org/projects/plugins/
  * http://www.ssuet.edu.pk/taimoor/books/0-7897-1063-3/ch11.htm

  * NOTE: This is mostly a wrapper for module specific functions defined
  *       elsewhere
  * TODO: give some more details
  */

#include "native_client/src/include/portability.h"
#include "native_client/src/include/portability_io.h"
#include "native_client/src/include/portability_string.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// To define nothrow object to prevent exceptions from new.
#include <new>

#include "native_client/src/include/elf.h"
#include "native_client/src/include/nacl_elf.h"
#include "native_client/src/shared/npruntime/nacl_npapi.h"
#include "native_client/src/trusted/desc/nrd_all_modules.h"
#if NACL_OSX
#include "native_client/src/trusted/plugin/osx/open_mac_file.h"
#endif  // NACL_OSX
#include "native_client/src/trusted/plugin/npapi/plugin_npapi.h"
#include "native_client/src/trusted/service_runtime/expiration.h"

using nacl::DebugPrintf;

#define EXPAND(x) x
#define ENQUOTE(x) #x
#define EMBED(x) EXPAND(ENQUOTE(x))

const char kPluginName[] = "Native Client Plugin";
const char kPluginDescription[] = "Native Client Plugin was built on "
                                  __DATE__ " at " __TIME__
#if defined(EXPIRATION_CHECK)
                                  " and expires on "
                                  EMBED(EXPIRATION_MONTH) "/"
                                  EMBED(EXPIRATION_DAY) "/"
                                  EMBED(EXPIRATION_YEAR)
                                  " (mm/dd/yyyy)"
#endif
                                  "";

char* NPP_GetMIMEDescription() {
  return const_cast<char*>("application/x-nacl-srpc:nexe:NativeClient"
                           " Simple RPC module;");
}

// Invoked from NP_Initialize()
NPError NPP_Initialize() {
  DebugPrintf("NPP_Initialize\n");
  if (NaClHasExpired()) {
    return NPERR_INCOMPATIBLE_VERSION_ERROR;
  }
  NaClNrdAllModulesInit();
  return NPERR_NO_ERROR;
}

// Invoked from NP_Shutdown()
void NPP_Shutdown() {
  NaClNrdAllModulesFini();
  DebugPrintf("NPP_Shutdown\n");
}

NPError NPP_New(NPMIMEType plugin_type,
                NPP instance,
                uint16_t mode,
                int16_t argc,
                char* argn[],
                char* argv[],
                NPSavedData* saved) {
  UNREFERENCED_PARAMETER(mode);
  UNREFERENCED_PARAMETER(saved);
  DebugPrintf("NPP_New '%s'\n", plugin_type);

  for (int i = 0; i < argc; ++i) {
    DebugPrintf("args %u: '%s' '%s'\n", i, argn[i], argv[i]);
  }

  if (NaClHasExpired()) {
    return NPERR_INCOMPATIBLE_VERSION_ERROR;
  }
  if (instance == NULL) {
    return NPERR_INVALID_INSTANCE_ERROR;
  }
  if (strcmp(plugin_type, "application/x-nacl-srpc") == 0) {
    instance->pdata = static_cast<nacl::NPInstance*>(
        plugin::PluginNpapi::New(instance, argc, argn, argv));
    if (instance->pdata == NULL) {
      return NPERR_OUT_OF_MEMORY_ERROR;
    }
  }
  if (instance->pdata == NULL) {
    return NPERR_OUT_OF_MEMORY_ERROR;
  }
#ifndef NACL_STANDALONE
  // NaCl is a windowless plugin in Chrome.
  NPN_SetValue(instance, NPPVpluginWindowBool, false);
#endif
  return NPERR_NO_ERROR;
}

NPError NPP_Destroy(NPP instance, NPSavedData** save) {
  NPError nperr = NPERR_NO_ERROR;

  DebugPrintf("NPP_Destroy\n");
  if (instance == NULL) {
    return NPERR_INVALID_INSTANCE_ERROR;
  }
  nacl::NPInstance* module = static_cast<nacl::NPInstance*>(instance->pdata);
  if (module != NULL) {
    instance->pdata = NULL;
    nperr = module->Destroy(save);  // module must delete itself.
  }
  // TODO(sehr): we are leaking browser_interface, extension, and SRPC_Plugin.
  return nperr;
}

NPError NPP_SetWindow(NPP instance, NPWindow* window) {
  DebugPrintf("NPP_SetWindow\n");
  if (instance == NULL) {
    return NPERR_INVALID_INSTANCE_ERROR;
  }
  if (window == NULL) {
    return NPERR_GENERIC_ERROR;
  }
  nacl::NPInstance* module = static_cast<nacl::NPInstance*>(instance->pdata);
  if (module != NULL) {
    return module->SetWindow(window);
  }
  return NPERR_GENERIC_ERROR;
}

NPError NPP_GetValue(NPP instance, NPPVariable variable, void *value) {
  DebugPrintf("NPP_GetValue\n");
  if (variable == NPPVpluginNameString) {
    *static_cast<const char**>(value) = kPluginName;
    return NPERR_NO_ERROR;
  }
  if (variable == NPPVpluginDescriptionString) {
    *static_cast<const char**>(value) = kPluginDescription;
    return NPERR_NO_ERROR;
  }
  if (instance == NULL) {
    return NPERR_INVALID_INSTANCE_ERROR;
  }
  nacl::NPInstance* module = static_cast<nacl::NPInstance*>(instance->pdata);
  if (module != NULL) {
    return module->GetValue(variable, value);
  }
  return NPERR_GENERIC_ERROR;
}

int16_t NPP_HandleEvent(NPP instance, void* event) {
  DebugPrintf("NPP_HandleEvent\n");
  if (instance == NULL) {
    return NPERR_INVALID_INSTANCE_ERROR;
  }
  nacl::NPInstance* module = static_cast<nacl::NPInstance*>(instance->pdata);
  if (module != NULL) {
    return module->HandleEvent(event);
  }
  return 0;
}

NPError NPP_NewStream(NPP instance, NPMIMEType type,
                      NPStream* stream, NPBool seekable,
                      uint16_t* stype) {
  DebugPrintf("NPP_NewStream\n");
  if (instance == NULL) {
    return NPERR_INVALID_INSTANCE_ERROR;
  }
  nacl::NPInstance* module = static_cast<nacl::NPInstance*>(instance->pdata);
  if (module != NULL) {
    return module->NewStream(type, stream, seekable, stype);
  }
  return NPERR_GENERIC_ERROR;
}

int32_t NPP_WriteReady(NPP instance, NPStream* stream) {
  DebugPrintf("NPP_WriteReady \n");
  if ((NULL == instance) || (NULL == instance->pdata)) {
    return 0;
  }
  nacl::NPInstance* module = static_cast<nacl::NPInstance*>(instance->pdata);
  return module->WriteReady(stream);
}

int32_t NPP_Write(NPP instance, NPStream* stream, int32_t offset, int32_t len,
                  void* buffer) {
  DebugPrintf("NPP_Write: offset %d, len %d\n", offset, len);
  if ((NULL == instance) || (NULL == instance->pdata)) {
    return -1;
  }
  nacl::NPInstance* module = static_cast<nacl::NPInstance*>(instance->pdata);
  return module->Write(stream, offset, len, buffer);
}

void NPP_StreamAsFile(NPP instance, NPStream* stream, const char* filename) {
  DebugPrintf("NPP_StreamAsFile: %s\n", filename);
  if (instance == NULL) {
    return;
  }
  nacl::NPInstance* module = static_cast<nacl::NPInstance*>(instance->pdata);
  if (module != NULL) {
#if NACL_OSX
    OpenMacFile(stream, filename, module);
#else
    module->StreamAsFile(stream, filename);
#endif  // NACL_OSX
  }
}

// Note Safari running on OS X removes the loaded file at this point.
// To keep the file, it must be opened in NPP_StreamAsFile().
NPError NPP_DestroyStream(NPP instance, NPStream *stream, NPError reason) {
  DebugPrintf("NPP_DestroyStream %d\n", reason);
  if (instance == NULL) {
    return NPERR_INVALID_INSTANCE_ERROR;
  }
  nacl::NPInstance* module = static_cast<nacl::NPInstance*>(instance->pdata);
  if (module != NULL) {
    return module->DestroyStream(stream, reason);
  }
  return NPERR_GENERIC_ERROR;
}

void NPP_URLNotify(NPP instance, const char* url, NPReason reason,
                   void* notify_data) {
  DebugPrintf("NPP_URLNotify(%s)\n", url);
  if (instance == NULL) {
    return;
  }
  nacl::NPInstance* module = static_cast<nacl::NPInstance*>(instance->pdata);
  if (module != NULL) {
    module->URLNotify(url, reason, notify_data);
  }
}
