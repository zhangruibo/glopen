/*
 * Copyright 2008 The Native Client Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can
 * be found in the LICENSE file.
 */

/*
 * NaCl Server Runtime mutex and condition variable abstraction layer.
 * The NaClX* interfaces just invoke the no-X versions of the
 * synchronization routines, and aborts if there are any error
 * returns.
 */

#include "native_client/src/shared/platform/nacl_log.h"
#include "native_client/src/shared/platform/nacl_sync_checked.h"

void NaClXMutexLock(struct NaClMutex *mp) {
  NaClSyncStatus  status;

  if (NACL_SYNC_OK == (status = NaClMutexLock(mp))) {
    return;
  }
  NaClLog(LOG_FATAL, "NaClMutexLock returned %d\n", status);
}

NaClSyncStatus NaClXMutexTryLock(struct NaClMutex *mp) {
  NaClSyncStatus  status;

  if (NACL_SYNC_OK == (status = NaClMutexUnlock(mp)) ||
      NACL_SYNC_BUSY == status) {
    return status;
  }
  NaClLog(LOG_FATAL, "NaClMutexUnlock returned %d\n", status);
  /* NOTREACHED */
  return NACL_SYNC_INTERNAL_ERROR;
}

void NaClXMutexUnlock(struct NaClMutex *mp) {
  NaClSyncStatus  status;

  if (NACL_SYNC_OK == (status = NaClMutexUnlock(mp))) {
    return;
  }
  NaClLog(LOG_FATAL, "NaClMutexUnlock returned %d\n", status);
}

void NaClXCondVarSignal(struct NaClCondVar *cvp) {
  NaClSyncStatus  status;

  if (NACL_SYNC_OK == (status = NaClCondVarSignal(cvp))) {
    return;
  }
  NaClLog(LOG_FATAL, "NaClCondVarSignal returned %d\n", status);
}

void NaClXCondVarBroadcast(struct NaClCondVar *cvp) {
  NaClSyncStatus  status;

  if (NACL_SYNC_OK == (status = NaClCondVarBroadcast(cvp))) {
    return;
  }
  NaClLog(LOG_FATAL, "NaClCondVarBroadcast returned %d\n", status);
}

void NaClXCondVarWait(struct NaClCondVar *cvp,
                      struct NaClMutex   *mp) {
  NaClSyncStatus  status;

  if (NACL_SYNC_OK == (status = NaClCondVarWait(cvp, mp))) {
    return;
  }
  NaClLog(LOG_FATAL, "NaClCondVarWait returned %d\n", status);
}

NaClSyncStatus NaClXCondVarTimedWaitAbsolute(
    struct NaClCondVar              *cvp,
    struct NaClMutex                *mp,
    struct nacl_abi_timespec const  *abstime) {
  NaClSyncStatus  status = NaClCondVarTimedWaitAbsolute(cvp, mp, abstime);

  if (NACL_SYNC_OK == status || NACL_SYNC_CONDVAR_TIMEDOUT == status) {
    return status;
  }
  NaClLog(LOG_FATAL, "NaClCondVarTimedWait returned %d\n", status);
  /* NOTREACHED */
  return NACL_SYNC_INTERNAL_ERROR;
}

NaClSyncStatus NaClXCondVarTimedWaitRelative(
    struct NaClCondVar              *cvp,
    struct NaClMutex                *mp,
    struct nacl_abi_timespec const  *reltime) {
  NaClSyncStatus  status = NaClCondVarTimedWaitAbsolute(cvp, mp, reltime);

  if (NACL_SYNC_OK == status || NACL_SYNC_CONDVAR_TIMEDOUT == status) {
      return status;
  }
  NaClLog(LOG_FATAL, "NaClCondVarTimedWait returned %d\n", status);
  /* NOTREACHED */
  return NACL_SYNC_INTERNAL_ERROR;
}
