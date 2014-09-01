/* 
** File:
**   $Id: $
**
** Purpose: Manage the table (Packet Table) that defines which packets will be sent from the
**          software bus to a socket.
**
** Notes
**   1. 
**
** References:
**   1. Core Flight Executive Application Developers Guide.
**   2. The GN&C FSW Framework Programmer's Guide
**
**
** $Date: $
** $Revision: $
** $Log: $
*/

/*
** Include Files:
*/

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "app_config.h"
#include "pktmgr.h"
#include "network_includes.h"

/*
** Global File Data
*/

static PKTMGR_Class*    PktMgr = NULL;
static PKTMGR_TblEntry  UnusedTblEntry = {PKTMGR_UNUSED_MSG_ID, {0, 0}, 0};

/*
** Local Function Prototypes
*/

static void DestructorCallback(void);
static void FlushTlmPipe(void);
static int  StreamIdEntry(uint16 StreamId);
static int  UnusedTableEntry(void);

/******************************************************************************
** Function: PKTMGR_Constructor
**
*/
void PKTMGR_Constructor(PKTMGR_Class*  PktMgrPtr, char* PipeName, uint16 PipeDepth)
{
   int i;

   PktMgr = PktMgrPtr;

   PktMgr->DownlinkOn   = FALSE;
   PktMgr->SuppressSend = TRUE;
   PktMgr->TlmSockId    = 0;
   strncpy(PktMgr->TlmDestIp, "000.000.000.000", PKTMGR_IP_STR_LEN);

   CFE_PSP_MemSet((void*)&(PktMgr->Table), 0, sizeof(PKTMGR_Table));
   for (i=0; i < PKTMGR_TBL_MAX_ENTRY_ID; i++) {

      PktMgr->Table.Entry[i].StreamId = PKTMGR_UNUSED_MSG_ID;

   } /* end pkt loop */


   CFE_SB_CreatePipe(&(PktMgr->TlmPipe), PipeDepth, PipeName);

   OS_TaskInstallDeleteHandler((void *)(&DestructorCallback)); /* Call when application terminates */

} /* End PKTMGR_Constructor() */


/******************************************************************************
** Function: PKTMGR_GetTblPtr
**
*/
const PKTMGR_Table* PKTMGR_GetTblPtr()
{

   return &(PktMgr->Table);

} /* End PKTMGR_GetTblPtr() */


/******************************************************************************
** Function:  PKTMGR_ResetStatus
**
*/
void PKTMGR_ResetStatus()
{

   /* Nothing to do */

} /* End PKTMGR_ResetStatus() */


/******************************************************************************
** Function: PKTMGR_OutputTelemetry
**
*/
void PKTMGR_OutputTelemetry(void)
{

    static struct sockaddr_in SocketAddr;
    int                       SocketStatus;
    int32                     SbStatus;
    uint16                    Size;
    CFE_SB_Msg_t              *PktPtr;

    CFE_PSP_MemSet((void*)&SocketAddr, 0, sizeof(SocketAddr));
    SocketAddr.sin_family      = AF_INET;
    SocketAddr.sin_addr.s_addr = inet_addr(PktMgr->TlmDestIp);
    SocketAddr.sin_port        = htons(LABTLM_TLM_PORT);
    SocketStatus = 0;

    /*
    ** CFE_SB_RcvMsg returns CFE_SUCCESS when it gets a packet, otherwise no packet was received
    */
    do
    {
       SbStatus = CFE_SB_RcvMsg(&PktPtr, PktMgr->TlmPipe, CFE_SB_POLL);

       if ( (SbStatus == CFE_SUCCESS) && (PktMgr->SuppressSend == FALSE) )
       {
          Size = CFE_SB_GetTotalMsgLength(PktPtr);

          if(PktMgr->DownlinkOn)
          {

             SocketStatus = sendto(PktMgr->TlmSockId, (char *)PktPtr, Size, 0,
                                  (struct sockaddr *) &SocketAddr, sizeof(SocketAddr) );

          }
          if (SocketStatus < 0)
          {
             CFE_EVS_SendEvent(PKTMGR_SOCKET_SEND_ERR_EID,CFE_EVS_ERROR,
                               "Error sending packet on socket %s, port %d, errno %d. Tlm output supressed\n",
                               PktMgr->TlmDestIp, LABTLM_TLM_PORT, errno);
             PktMgr->SuppressSend = TRUE;
          }

       } /* End if SB received msg and output enabled */

    } while(SbStatus == CFE_SUCCESS);

} /* End of PKTMGR_OutputTelemetry() */


/******************************************************************************
** Function: PKTMGR_LoadTable
**
*/
boolean PKTMGR_LoadTable(PKTMGR_Table* NewTable)
{

   int      i, EntryCnt = 0, FailedSubscription = 0;
   int32    Status;
   boolean  RetStatus = TRUE;

   CFE_SB_MsgPtr_t MsgPtr = NULL;

   PKTMGR_RemoveAllPktsCmd(MsgPtr);  /* Parameter is unused so OK to be NULL */

   CFE_PSP_MemCpy(&(PktMgr->Table), NewTable, sizeof(PKTMGR_Table));

   for (i=0; i < PKTMGR_TBL_MAX_ENTRY_ID; i++)
   {

      if(PktMgr->Table.Entry[i].StreamId != PKTMGR_UNUSED_MSG_ID)
      {
         EntryCnt++;
         Status = CFE_SB_SubscribeEx(PktMgr->Table.Entry[i].StreamId,
                                     PktMgr->TlmPipe,PktMgr->Table.Entry[i].Qos,
                                     PktMgr->Table.Entry[i].BuffLim);
         if(Status != CFE_SUCCESS)
         {
            FailedSubscription++;
            CFE_EVS_SendEvent(PKTMGR_LOAD_TBL_SUBSCRIBE_ERR_EID,CFE_EVS_ERROR,
                              "Error subscribing to stream 0x%4X, BuffLim %d, Status %i",
                              PktMgr->Table.Entry[i].StreamId, PktMgr->Table.Entry[i].BuffLim, Status);
         }
      }

   } /* End pkt loop */

   if (FailedSubscription == 0)
   {
      CFE_EVS_SendEvent(PKTMGR_LOAD_TBL_INFO_EID, CFE_EVS_INFORMATION,
                        "Loaded new table with %d entries", EntryCnt);
   }
   else
   {
      RetStatus = FALSE;
      CFE_EVS_SendEvent(PKTMGR_LOAD_TBL_ERR_EID, CFE_EVS_INFORMATION,
                        "Attempted to load new table with %d entries. Failed %d subscriptions",
                        EntryCnt, FailedSubscription);
   }

   return RetStatus;

} /* End PKTMGR_LoadTable() */


/******************************************************************************
** Function: PKTMGR_LoadTableEntry
**
*/
boolean PKTMGR_LoadTableEntry(uint16 EntryId, PKTMGR_TblEntry* NewEntry)
{

   int32    Status;
   boolean  RetStatus = TRUE;

   CFE_PSP_MemCpy(&(PktMgr->Table.Entry[EntryId]),NewEntry,sizeof(PKTMGR_TblEntry));

   Status = CFE_SB_SubscribeEx(NewEntry->StreamId, PktMgr->TlmPipe, NewEntry->Qos, NewEntry->BuffLim);

   if(Status == CFE_SUCCESS)
   {
      CFE_EVS_SendEvent(PKTMGR_LOAD_TBL_ENTRY_INFO_EID, CFE_EVS_INFORMATION,
                        "Loaded table entry %d with stream 0x%4X, BuffLim %d",
                        EntryId, NewEntry->StreamId, NewEntry->BuffLim);
   }
   else
   {
      RetStatus = FALSE;
      CFE_EVS_SendEvent(PKTMGR_LOAD_TBL_ENTRY_SUBSCRIBE_ERR_EID,CFE_EVS_ERROR,
                        "Error subscribing to stream 0x%4X, BuffLim %d, Status %i",
                        NewEntry->StreamId, NewEntry->BuffLim, Status);
   }

   return RetStatus;

} /* End PKTMGR_LoadTableEntry() */


/******************************************************************************
** Function: PKTMGR_EnableOutputCmd
**
*/
boolean PKTMGR_EnableOutputCmd(const CFE_SB_MsgPtr_t MsgPtr)
{
   const PKTMGR_EnableOutputCmdParam *EnableOutputCmd = (const PKTMGR_EnableOutputCmdParam *) MsgPtr;
   boolean  RetStatus = TRUE;

   strncpy(PktMgr->TlmDestIp, EnableOutputCmd->DestIp, PKTMGR_IP_STR_LEN);

   PktMgr->SuppressSend = FALSE;
   CFE_EVS_SendEvent(PKTMGR_TLM_OUTPUT_ENA_INFO_EID, CFE_EVS_INFORMATION,
                     "Telemetry output enabled for IP %s", PktMgr->TlmDestIp);

   /*
   ** If disabled then create the socket and turn it on. If already
   ** enabled then destination address is changed in the existing socket
   */
   if(PktMgr->DownlinkOn == FALSE)
   {

      if ( (PktMgr->TlmSockId = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
      {
         RetStatus = FALSE;
         CFE_EVS_SendEvent(PKTMGR_TLM_OUTPUT_ENA_SOCKET_ERR_EID, CFE_EVS_ERROR,
                           "Telemetry output enable socket error. errno %d", errno);
      }
      else
      {
         PktMgr->DownlinkOn = TRUE;
      }

   } /* End if downlink disabled */

   return RetStatus;

} /* End PKTMGR_EnableOutputCmd() */


/******************************************************************************
** Function: PKTMGR_AddPktCmd
**
*/
boolean PKTMGR_AddPktCmd(const CFE_SB_MsgPtr_t MsgPtr)
{

   const  PKTMGR_AddPktCmdParam *CmdParam = (const PKTMGR_AddPktCmdParam *) MsgPtr;
   int              EntryId;
   PKTMGR_TblEntry  NewEntry;
   boolean          RetStatus = TRUE;

   if ( (EntryId = StreamIdEntry(CmdParam->StreamId)) < 0)
   {

      if ( (EntryId = UnusedTableEntry()) >= 0 )
      {

         NewEntry.StreamId = CmdParam->StreamId;
         NewEntry.Qos      = CmdParam->Qos;
         NewEntry.BuffLim  = CmdParam->BuffLim;
         PKTMGR_LoadTableEntry(EntryId, &NewEntry);
         CFE_EVS_SendEvent(PKTMGR_ADD_PKT_INFO_EID, CFE_EVS_INFORMATION,
                           "Added packet 0x%04X, QoS (%d,%d), BuffLim %d",
                           NewEntry.StreamId, NewEntry.Qos.Priority, NewEntry.Qos.Reliability, NewEntry.BuffLim);

      } /* end if found unused entry */
      else
      {

         RetStatus = FALSE;
         CFE_EVS_SendEvent(PKTMGR_ADD_PKT_NO_FREE_ENTRY_ERR_EID, CFE_EVS_ERROR,
                           "Error adding packet 0x%04X, packet table is full. Unused StreamId = 0x%04X",
                           CmdParam->StreamId,PKTMGR_UNUSED_MSG_ID);

      }

   } /* End if didn't find existing entry */
   else
   {
      CFE_EVS_SendEvent(PKTMGR_ADD_PKT_DUPLICATE_ENTRY_EID, CFE_EVS_ERROR,
                        "Error adding packet, stream id 0x%4X exists at entry %d",
                        CmdParam->StreamId, EntryId);

   } /* End if found existing entry */

   return RetStatus;

} /* End of PKTMGR_AddPktCmd() */

/*******************************************************************
** Function: PKTMGR_RemovePktCmd
**
*/
boolean PKTMGR_RemovePktCmd(const CFE_SB_MsgPtr_t MsgPtr)
{

   const PKTMGR_RemovePktCmdParam *RemovePktCmd = (const PKTMGR_RemovePktCmdParam *) MsgPtr;
   int      EntryId = -1;
   int32    Status;
   boolean  RetStatus = TRUE;

   if ( (EntryId = StreamIdEntry(RemovePktCmd->StreamId)) >= 0)
   {

      CFE_PSP_MemCpy(&(PktMgr->Table.Entry[EntryId]), &UnusedTblEntry, sizeof(PKTMGR_TblEntry));
      Status = CFE_SB_Unsubscribe(RemovePktCmd->StreamId, PktMgr->TlmPipe);

      if(Status == CFE_SUCCESS)
      {
         CFE_EVS_SendEvent(PKTMGR_REMOVE_PKT_INFO_EID, CFE_EVS_INFORMATION,
                           "Removed stream id 0x%4X at table entry %d",
                           RemovePktCmd->StreamId, EntryId);
      }
      else
      {
         RetStatus = FALSE;
         CFE_EVS_SendEvent(PKTMGR_REMOVE_PKT_SB_ERR_EID, CFE_EVS_ERROR,
                           "Error removing stream id 0x%4X at table entry %d. Unsubscribe status 0x%8x",
                           RemovePktCmd->StreamId, EntryId, Status);
      }

   } /* End if found stream ID in table */
   else
   {

      CFE_EVS_SendEvent(PKTMGR_REMOVE_PKT_NOT_FOUND_ERR_EID, CFE_EVS_ERROR,
                        "Error removing stream id 0x%4X. Packet not found",
                        RemovePktCmd->StreamId);

   } /* End if didn't find stream ID in table */

   return RetStatus;

} /* End of PKTMGR_RemovePktCmd() */


/******************************************************************************
** Function: PKTMGR_RemoveAllPktsCmd
**
** TODO - The original code removed the inputs as well. My thought is this
**        command was used so the tolab app could be removed.
*/
boolean PKTMGR_RemoveAllPktsCmd(const CFE_SB_MsgPtr_t MsgPtr)
{

   int      i, EntryCnt = 0, FailedUnsubscribe = 0;
   int32    Status;
   boolean  RetStatus = TRUE;

   for (i=0; i < PKTMGR_TBL_MAX_ENTRY_ID; i++)
   {
       if (PktMgr->Table.Entry[i].StreamId != PKTMGR_UNUSED_MSG_ID )
       {

          EntryCnt++;
          CFE_PSP_MemCpy(&(PktMgr->Table.Entry[i]), &UnusedTblEntry, sizeof(PKTMGR_TblEntry));
          Status = CFE_SB_Unsubscribe(PktMgr->Table.Entry[i].StreamId, PktMgr->TlmPipe);

          if(Status != CFE_SUCCESS)
          {
             FailedUnsubscribe++;
             CFE_EVS_SendEvent(PKTMGR_REMOVE_ALL_PKTS_UNSUBSCRIBE_ERR_EID,CFE_EVS_ERROR,
                               "Error removing stream id 0x%4X at table entry %d. Unsubscribe status 0x%8X",
                               PktMgr->Table.Entry[i].StreamId, i, Status);
          }

       } /* End if entry in use */

   } /* End pkt loop */

   printf ("PKTMGR_RemoveAllPktsCmd() - About to flush pipe\n");
   FlushTlmPipe();
   printf ("PKTMGR_RemoveAllPktsCmd() - Completed pipe flush\n");

   if (FailedUnsubscribe == 0)
   {
      CFE_EVS_SendEvent(PKTMGR_REMOVE_ALL_PKTS_INFO_EID, CFE_EVS_INFORMATION,
                        "Removed %d table entries", EntryCnt);
   }
   else
   {
      RetStatus = FALSE;
      CFE_EVS_SendEvent(PKTMGR_REMOVE_ALL_PKTS_ERR_EID, CFE_EVS_INFORMATION,
                        "Attempted to remove %d entries. Failed %d unsubscribes",
                        EntryCnt, FailedUnsubscribe);
   }

   return RetStatus;

} /* End of PKTMGR_RemoveAllPktsCmd() */


/******************************************************************************
** Function: DestructorCallback
**
** This function is called when the to-lab is killed. This should
** never occur but if it does this will close the network socket.
*/
static void DestructorCallback(void)
{

   OS_printf("TO delete callback -- Closing TO Network socket.\n");
   if ( PktMgr->DownlinkOn)
   {
       close(PktMgr->TlmSockId);
   }

} /* End DestructorCallback() */

/******************************************************************************
** Function: FlushTlmPipe
**
** Remove all of the packets from the inptu pipe.
**
*/
static void FlushTlmPipe(void)
{

   int32 Status;
   CFE_SB_MsgPtr_t MsgPtr = NULL;

   do
   {
      Status = CFE_SB_RcvMsg(&MsgPtr, PktMgr->TlmPipe, CFE_SB_POLL);

   } while(Status == CFE_SUCCESS);

} /* End FlushTlmPipe() */

/******************************************************************************
** Function: StreamIdTableEntry
**
** Look for a StreamId in the packet table. Return the index of the
** table entry if it is found and return -1 if it is not found.
**
*/
static int StreamIdEntry(uint16 StreamId)
{

   int i;
   int Entry = -1;

   for (i=0; i < PKTMGR_TBL_MAX_ENTRY_ID; i++)
   {

      if (PktMgr->Table.Entry[i].StreamId == StreamId)
      {
         Entry = i;
         break;
      }

   } /* end pkt loop */

   return Entry;

} /* End StreamIdEntry() */

/******************************************************************************
** Function: UnusedTableEntry
**
** Look for an unused packet table entry. Return the index of the
** unused table entry if one is found and return -1 if one is not
** found.
**
*/
static int UnusedTableEntry(void)
{

   int i;
   int Entry = -1;

   for (i=0; i < PKTMGR_TBL_MAX_ENTRY_ID; i++)
   {

      if (PktMgr->Table.Entry[i].StreamId == PKTMGR_UNUSED_MSG_ID)
      {
         Entry = i;
         break;
      }

   } /* end pkt loop */

   return Entry;

} /* End UnusedTableEntry() */

/* end of file */
