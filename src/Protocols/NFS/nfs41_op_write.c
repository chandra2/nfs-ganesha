/*
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2008)
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * ---------------------------------------
 */

/**
 * \file    nfs41_op_write.c
 * \author  $Author: deniel $
 * \date    $Date: 20heck
 * \version $Revision: 1.15 $
 * \brief   Routines used for managing the NFS4 COMPOUND functions.
 *
 * nfs41_op_write.c : Routines used for managing the NFS4 COMPOUND functions.
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef _SOLARIS
#include "solaris_port.h"
#endif

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "HashData.h"
#include "HashTable.h"
#include "rpc.h"
#include "log_macros.h"
#include "stuff_alloc.h"
#include "nfs4.h"
#include "nfs_core.h"
#include "sal_functions.h"
#include "cache_content_policy.h"
#include "nfs_proto_functions.h"
#include "nfs_proto_tools.h"

/**
 * nfs41_op_write: The NFS4_OP_WRITE operation
 *
 * This functions handles the NFS4_OP_WRITE operation in NFSv4. This function can be called only from nfs4_Compound.
 *
 * @param op    [IN]    pointer to nfs41_op arguments
 * @param data  [INOUT] Pointer to the compound request's data
 * @param resp  [IN]    Pointer to nfs41_op results
 *
 * @return NFS4_OK if successfull, other values show an error.
 *
 */

extern verifier4 NFS4_write_verifier;   /* NFS V4 write verifier from nfs_Main.c     */

#define arg_WRITE4 op->nfs_argop4_u.opwrite
#define res_WRITE4 resp->nfs_resop4_u.opwrite

int nfs41_op_write(struct nfs_argop4 *op, compound_data_t * data, struct nfs_resop4 *resp)
{
  char __attribute__ ((__unused__)) funcname[] = "nfs41_op_write";

  fsal_seek_t              seek_descriptor;
  fsal_size_t              size;
  fsal_size_t              written_size;
  fsal_off_t               offset;
  fsal_boolean_t           eof_met;
  bool_t                   stable_flag = TRUE;
  caddr_t                  bufferdata;
  stable_how4              stable_how;
  cache_content_status_t   content_status;
  state_t                * pstate_found = NULL;
  cache_inode_status_t     cache_status;
  state_status_t           state_status;
  fsal_attrib_list_t       attr;
  cache_entry_t          * entry = NULL;
  state_t                * pstate_iterate = NULL;
  state_t                * pstate_previous_iterate = NULL;

  cache_content_policy_data_t datapol;

  datapol.UseMaxCacheSize = FALSE;

  /* Lock are not supported */
  resp->resop = NFS4_OP_WRITE;
  res_WRITE4.status = NFS4_OK;

  /* If there is no FH */
  if(nfs4_Is_Fh_Empty(&(data->currentFH)))
    {
      res_WRITE4.status = NFS4ERR_NOFILEHANDLE;
      return res_WRITE4.status;
    }

  /* If the filehandle is invalid */
  if(nfs4_Is_Fh_Invalid(&(data->currentFH)))
    {
      res_WRITE4.status = NFS4ERR_BADHANDLE;
      return res_WRITE4.status;
    }

  /* Tests if the Filehandle is expired (for volatile filehandle) */
  if(nfs4_Is_Fh_Expired(&(data->currentFH)))
    {
      res_WRITE4.status = NFS4ERR_FHEXPIRED;
      return res_WRITE4.status;
    }

  /* If Filehandle points to a xattr object, manage it via the xattrs specific functions */
  if(nfs4_Is_Fh_Xattr(&(data->currentFH)))
    return nfs4_op_write_xattr(op, data, resp);

  /* Manage access type MDONLY */
  if(data->pexport->access_type == ACCESSTYPE_MDONLY)
    {
      res_WRITE4.status = NFS4ERR_DQUOT;
      return res_WRITE4.status;
    }

  /* vnode to manage is the current one */
  entry = data->current_entry;

  /* Check for special stateid */
  if(!memcmp((char *)all_zero, arg_WRITE4.stateid.other, OTHERSIZE) &&
     arg_WRITE4.stateid.seqid == 0)
    {
      /* "All 0 stateid special case", see RFC3530 page 220-221 for details
       * This will be treated as a client that held no lock at all,
       * I set pstate_found to NULL to remember this situation later */
      pstate_found = NULL;
    }
  else if(!memcmp((char *)all_one, arg_WRITE4.stateid.other, OTHERSIZE) &&
          arg_WRITE4.stateid.seqid == 0xFFFFFFFF)
    {
      /* "All 1 stateid special case", see RFC3530 page 220-221 for details
       * This will be treated as a client that held no lock at all,
       * I set pstate_found to NULL to remember this situation later */
      pstate_found = NULL;
    }

  /* NB: After this points, if pstate_found == NULL, then the stateid is all-0 or all-1 */

  /* Iterate through file's state to look for conflicts */
  pstate_iterate = NULL;
  pstate_previous_iterate = NULL;
  do
    {
      state_iterate(data->current_entry,
                    &pstate_iterate,
                    pstate_previous_iterate,
                    data->pclient, data->pcontext, &state_status);

      if(state_status == STATE_STATE_ERROR)
        break;                  /* Get out of the loop */

      if(state_status == STATE_INVALID_ARGUMENT)
        {
          res_WRITE4.status = NFS4ERR_INVAL;
          return res_WRITE4.status;
        }

      cache_status = CACHE_INODE_SUCCESS;

      if(pstate_iterate != NULL)
        {
          if(pstate_iterate->state_type == STATE_TYPE_SHARE)
            {
              if(pstate_found != pstate_iterate)
                {
                  if(pstate_iterate->state_data.share.share_deny & OPEN4_SHARE_DENY_WRITE)
                    {
                      /* Writing to this file if prohibited, file is write-denied */
                      res_WRITE4.status = NFS4ERR_LOCKED;
                      return res_WRITE4.status;
                    }
                }
            }
        }
      pstate_previous_iterate = pstate_iterate;
    }
  while(pstate_iterate != NULL);

  /* Only files can be written */
  if(data->current_filetype != REGULAR_FILE)
    {
      /* If the destination is no file, return EISDIR if it is a directory and EINAVL otherwise */
      if(data->current_filetype == DIR_BEGINNING
         || data->current_filetype == DIR_CONTINUE)
        res_WRITE4.status = NFS4ERR_ISDIR;
      else
        res_WRITE4.status = NFS4ERR_INVAL;

      return res_WRITE4.status;
    }

  /* Get the characteristics of the I/O to be made */
  offset = arg_WRITE4.offset;
  size = arg_WRITE4.data.data_len;
  stable_how = arg_WRITE4.stable;
  LogFullDebug(COMPONENT_NFS_V4,
               "NFS4_OP_WRITE: offset = %llu  length = %llu  stable = %d",
               (unsigned long long)offset, size, stable_how);

  if((data->pexport->options & EXPORT_OPTION_MAXOFFSETWRITE) ==
     EXPORT_OPTION_MAXOFFSETWRITE)
    if((fsal_off_t) (offset + size) > data->pexport->MaxOffsetWrite)
      {
        res_WRITE4.status = NFS4ERR_DQUOT;
        return res_WRITE4.status;
      }

  /* The size to be written should not be greater than FATTR4_MAXWRITESIZE because this value is asked
   * by the client at mount time, but we check this by security */
  if(((data->pexport->options & EXPORT_OPTION_MAXWRITE) == EXPORT_OPTION_MAXWRITE) &&
     size > data->pexport->MaxWrite)
    {
      /*
       * The client asked for too much data, we
       * must restrict him
       */
      size = data->pexport->MaxWrite;
    }

  /* Where are the data ? */
  bufferdata = arg_WRITE4.data.data_val;

  LogFullDebug(COMPONENT_NFS_V4,
               "NFS4_OP_WRITE: offset = %llu  length = %llu",
               (unsigned long long)offset, size);

  /* if size == 0 , no I/O) are actually made and everything is alright */
  if(size == 0)
    {
      res_WRITE4.WRITE4res_u.resok4.count = 0;
      res_WRITE4.WRITE4res_u.resok4.committed = FILE_SYNC4;

      memcpy(res_WRITE4.WRITE4res_u.resok4.writeverf, NFS4_write_verifier,
             sizeof(verifier4));

      res_WRITE4.status = NFS4_OK;
      return res_WRITE4.status;
    }

  if((data->pexport->options & EXPORT_OPTION_USE_DATACACHE) &&
     (cache_content_cache_behaviour(entry,
                                    &datapol,
                                    (cache_content_client_t *) (data->pclient->
                                                                pcontent_client),
                                    &content_status) == CACHE_CONTENT_FULLY_CACHED)
     && (entry->object.file.pentry_content == NULL))
    {
      /* Entry is not in datacache, but should be in, cache it .
       * Several threads may call this function at the first time and a race condition can occur here
       * in order to avoid this, cache_inode_add_data_cache is "mutex protected"
       * The first call will create the file content cache entry, the further will return
       * with error CACHE_INODE_CACHE_CONTENT_EXISTS which is not a pathological thing here */

      datapol.UseMaxCacheSize = data->pexport->options & EXPORT_OPTION_MAXCACHESIZE;
      datapol.MaxCacheSize = data->pexport->MaxCacheSize;

      /* Status is set in last argument */
      cache_inode_add_data_cache(entry, data->ht, data->pclient, data->pcontext,
                                 &cache_status);

      if((cache_status != CACHE_INODE_SUCCESS) &&
         (cache_status != CACHE_INODE_CACHE_CONTENT_EXISTS))
        {
          res_WRITE4.status = NFS4ERR_SERVERFAULT;
          return res_WRITE4.status;
        }

    }

  if((nfs_param.core_param.use_nfs_commit == TRUE) && (arg_WRITE4.stable == UNSTABLE4))
    {
      stable_flag = FALSE;
    }
  else
    {
      stable_flag = TRUE;
    }

  /* An actual write is to be made, prepare it */
  /* only FILE_SYNC mode is supported */
  /* Set up uio to define the transfer */
  seek_descriptor.whence = FSAL_SEEK_SET;
  seek_descriptor.offset = offset;

  if(cache_inode_rdwr(entry,
                      CACHE_CONTENT_WRITE,
                      &seek_descriptor,
                      size,
                      &written_size,
                      &attr,
                      bufferdata,
                      &eof_met,
                      data->ht,
                      data->pclient,
                      data->pcontext, stable_flag, &cache_status) != CACHE_INODE_SUCCESS)
    {
      res_WRITE4.status = nfs4_Errno(cache_status);
      return res_WRITE4.status;
    }

  /* Set the returned value */
  if(stable_flag == TRUE)
    res_WRITE4.WRITE4res_u.resok4.committed = FILE_SYNC4;
  else
    res_WRITE4.WRITE4res_u.resok4.committed = UNSTABLE4;

  res_WRITE4.WRITE4res_u.resok4.count = written_size;
  memcpy(res_WRITE4.WRITE4res_u.resok4.writeverf, NFS4_write_verifier, sizeof(verifier4));

  res_WRITE4.status = NFS4_OK;

  return res_WRITE4.status;
}                               /* nfs41_op_write */

/**
 * nfs41_op_write_Free: frees what was allocared to handle nfs41_op_write.
 *
 * Frees what was allocared to handle nfs41_op_write.
 *
 * @param resp  [INOUT]    Pointer to nfs41_op results
 *
 * @return nothing (void function )
 *
 */
void nfs41_op_write_Free(WRITE4res * resp)
{
  /* Nothing to be done */
  return;
}                               /* nfs41_op_write_Free */
