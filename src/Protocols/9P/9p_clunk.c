/*
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2011)
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
 * \file    9p_clunk.c
 * \brief   9P version
 *
 * 9p_clunk.c : _9P_interpretor, request ATTACH
 *
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
#include "nfs_core.h"
#include "stuff_alloc.h"
#include "log_macros.h"
#include "cache_inode.h"
#include "fsal.h"
#include "9p.h"


int _9p_clunk( _9p_request_data_t * preq9p, 
               void  * pworker_data,
               u32 * plenout, 
               char * preply)
{
  char * cursor = preq9p->_9pmsg + _9P_HDR_SIZE + _9P_TYPE_SIZE ;
  //nfs_worker_data_t * pwkrdata = (nfs_worker_data_t *)pworker_data ;

  u16 * msgtag = NULL ;
  u32 * fid    = NULL ;

  int rc = 0 ;
  u32 err = 0 ;

  _9p_fid_t * pfid = NULL ;

  if ( !preq9p || !pworker_data || !plenout || !preply )
   return -1 ;

  /* Get data */
  _9p_getptr( cursor, msgtag, u16 ) ; 
  _9p_getptr( cursor, fid,    u32 ) ; 

  LogDebug( COMPONENT_9P, "TCLUNK: tag=%u fid=%u", (u32)*msgtag, *fid ) ;

  /* The clunk here does nothing but cleaning info in the fid. We just keep the fid in the table because it probably will be reused 
   * soon by the client with the same fid. It faster and more efficient to Add an HashTable entry (allowing overwrite)
   * scratching a formerly written entry */ 
  if( ( pfid = _9p_hash_fid_get( &preq9p->conn, 
                                 *fid,
                                 &rc ) ) == NULL )
   {
     err = ENOENT ;
     rc = _9p_rerror( preq9p, msgtag, &err, strerror( err ), plenout, preply ) ;
     return rc ;
   }

  /* Clean the fid */
  memset( (char *)pfid, 0, sizeof( _9p_fid_t ) ) ;

  /* Build the reply */
  _9p_setinitptr( cursor, preply, _9P_RCLUNK ) ;
  _9p_setptr( cursor, msgtag, u16 ) ;

  _9p_setendptr( cursor, preply ) ;
  _9p_checkbound( cursor, preply, plenout ) ;

  LogDebug( COMPONENT_9P, "RCLUNK: tag=%u fid=%u", (u32)*msgtag, *fid ) ;

  return 1 ;
}
