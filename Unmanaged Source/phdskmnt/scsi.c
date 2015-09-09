
/// proxy.c
/// SCSI support routines, called from miniport callback functions, normally
/// at DISPATCH_LEVEL. This includes responding to control requests and
/// queueing work items for requests that need to be carried out at
/// PASSIVE_LEVEL.
/// 
/// Copyright (c) 2012-2015, Arsenal Consulting, Inc. (d/b/a Arsenal Recon) <http://www.ArsenalRecon.com>
/// This source code and API are available under the terms of the Affero General Public
/// License v3.
///
/// Please see LICENSE.txt for full license terms, including the availability of
/// proprietary exceptions.
/// Questions, comments, or requests for clarification: http://ArsenalRecon.com/contact/
///

#include "phdskmnt.h"

#include <Ntddcdrm.h>
#include <Ntddmmc.h>

#define TOC_DATA_TRACK                   0x04

#ifdef USE_SCSIPORT
/**************************************************************************************************/
/*                                                                                                */
/**************************************************************************************************/
VOID
ScsiExecuteRaidControllerUnit(
__in pHW_HBA_EXT          pHBAExt,    // Adapter device-object extension from port driver.
__in PSCSI_REQUEST_BLOCK  pSrb,
__in PUCHAR               pResult
)
{
    KdPrint2(("PhDskMnt::ScsiExecuteRaidControllerUnit: pSrb = 0x%p, CDB = 0x%X Path: %x TID: %x Lun: %x\n",
        pSrb, pSrb->Cdb[0], pSrb->PathId, pSrb->TargetId, pSrb->Lun));

    *pResult = ResultDone;

    switch (pSrb->Cdb[0])
    {
    case SCSIOP_TEST_UNIT_READY:
    case SCSIOP_SYNCHRONIZE_CACHE:
    case SCSIOP_START_STOP_UNIT:
    case SCSIOP_VERIFY:
        ScsiSetSuccess(pSrb, 0);
        break;

    case SCSIOP_INQUIRY:
        ScsiOpInquiryRaidControllerUnit(pHBAExt, pSrb);
        break;

    default:
        KdPrint(("PhDskMnt::ScsiExecuteRaidControllerUnit: Unknown opcode=0x%X\n", (int)pSrb->Cdb[0]));
        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
        break;
    }
}

VOID
ScsiOpInquiryRaidControllerUnit(
__in pHW_HBA_EXT          pHBAExt,      // Adapter device-object extension from port driver.
__in PSCSI_REQUEST_BLOCK  pSrb
)
{
    PINQUIRYDATA          pInqData = (PINQUIRYDATA)pSrb->DataBuffer;// Point to Inquiry buffer.
    PCDB                  pCdb;

    KdPrint2(("PhDskMnt::ScsiOpInquiryRaidControllerUnit:  pHBAExt = 0x%p, pSrb=0x%p\n", pHBAExt, pSrb));

    RtlZeroMemory((PUCHAR)pSrb->DataBuffer, pSrb->DataTransferLength);

    pCdb = (PCDB)pSrb->Cdb;

    if (pCdb->CDB6INQUIRY3.EnableVitalProductData == 1)
    {
        KdPrint(("PhDskMnt::ScsiOpInquiry: Received VPD request for page 0x%X\n", pCdb->CDB6INQUIRY.PageCode));

        // Current implementation of ScsiOpVPDRaidControllerUnit seems somewhat dangerous and could cause buffer
        // overruns. For now, just skip Vital Product Data requests.
#if 1
        ScsiOpVPDRaidControllerUnit(pHBAExt, pSrb);
#else
        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
#endif

        goto done;
    }

    pInqData->DeviceType = ARRAY_CONTROLLER_DEVICE;

    pInqData->CommandQueue = TRUE;

    RtlMoveMemory(pInqData->VendorId, pHBAExt->VendorId, 8);
    RtlMoveMemory(pInqData->ProductId, pHBAExt->ProductId, 16);
    RtlMoveMemory(pInqData->ProductRevisionLevel, pHBAExt->ProductRevision, 4);

    ScsiSetSuccess(pSrb, sizeof(INQUIRYDATA));

done:
    KdPrint2(("PhDskMnt::ScsiOpInquiry: End: status=0x%X\n", (int)pSrb->SrbStatus));

    return;
}                                                     // End ScsiOpInquiry.
#endif

/**************************************************************************************************/
/*                                                                                                */
/**************************************************************************************************/
VOID
ScsiExecute(
__in pHW_HBA_EXT          pHBAExt,    // Adapter device-object extension from port driver.
__in PSCSI_REQUEST_BLOCK  pSrb,
__in PUCHAR               pResult,
__in PKIRQL               LowestAssumedIrql
)
{
    pHW_LU_EXTENSION  pLUExt = NULL;
    UCHAR             status;

    KdPrint2(("PhDskMnt::ScsiExecute: pSrb = 0x%p, CDB = 0x%X Path: %x TID: %x Lun: %x\n",
        pSrb, pSrb->Cdb[0], pSrb->PathId, pSrb->TargetId, pSrb->Lun));

#ifdef USE_SCSIPORT
    // In case of SCSIPORT (Win XP), we need to show at least one working device connected to this
    // adapter. Otherwise, SCSIPORT may not even let control requests through, which could cause
    // a scenario where even new devices cannot be added. So, for device path 0:0:0, always answer
    // 'success' (without actual response data) to some basic requests.
    if ((pSrb->PathId | pSrb->TargetId | pSrb->Lun) == 0)
    {
        ScsiExecuteRaidControllerUnit(pHBAExt, pSrb, pResult);
        goto Done;
    }
#endif

    *pResult = ResultDone;

    // Get the LU extension from port driver.
    status = ScsiGetLUExtension(pHBAExt, &pLUExt, pSrb->PathId,
        pSrb->TargetId, pSrb->Lun, LowestAssumedIrql);

    if (status != SRB_STATUS_SUCCESS)
    {
        ScsiSetError(pSrb, status);

        KdPrint(("PhDskMnt::ScsiExecute: No LUN object yet for device %d:%d:%d\n", pSrb->PathId, pSrb->TargetId, pSrb->Lun));

        goto Done;
    }

    // Set SCSI check conditions if LU is not yet ready
    if (!KeReadStateEvent(&pLUExt->Initialized))
    {
        ScsiSetCheckCondition(
            pSrb,
            SRB_STATUS_BUSY,
            SCSI_SENSE_NOT_READY,
            SCSI_ADSENSE_LUN_NOT_READY,
            SCSI_SENSEQ_BECOMING_READY
            );

        KdPrint(("PhDskMnt::ScsiExecute: Device %d:%d:%d not yet ready.\n", pSrb->PathId, pSrb->TargetId, pSrb->Lun));

        goto Done;
    }
    
    // Handle sufficient opcodes to support a LUN suitable for a file system. Other opcodes are failed.

    switch (pSrb->Cdb[0])
    {
    case SCSIOP_TEST_UNIT_READY:
    case SCSIOP_SYNCHRONIZE_CACHE:
    case SCSIOP_SYNCHRONIZE_CACHE16:
    case SCSIOP_VERIFY:
    case SCSIOP_VERIFY16:
        ScsiSetSuccess(pSrb, 0);
        break;

    case SCSIOP_START_STOP_UNIT:
        ScsiOpStartStopUnit(pHBAExt, pLUExt, pSrb, LowestAssumedIrql);
        break;

    case SCSIOP_INQUIRY:
        ScsiOpInquiry(pHBAExt, pLUExt, pSrb);
        break;

    case SCSIOP_READ_CAPACITY:
    case SCSIOP_READ_CAPACITY16:
        ScsiOpReadCapacity(pHBAExt, pLUExt, pSrb);
        break;

    case SCSIOP_READ:
    case SCSIOP_READ16:
    case SCSIOP_WRITE:
    case SCSIOP_WRITE16:
        ScsiOpReadWrite(pHBAExt, pLUExt, pSrb, pResult, LowestAssumedIrql);
        break;

    case SCSIOP_UNMAP:
        ScsiSetSuccess(pSrb, 0);
        break;

    case SCSIOP_READ_TOC:
        ScsiOpReadTOC(pHBAExt, pLUExt, pSrb);
        break;

    case SCSIOP_GET_CONFIGURATION:
        ScsiOpGetConfiguration(pHBAExt, pLUExt, pSrb);
        break;

    case SCSIOP_READ_DISC_INFORMATION:
        ScsiOpReadDiscInformation(pHBAExt, pLUExt, pSrb);
        break;

    case SCSIOP_READ_TRACK_INFORMATION:
        ScsiOpReadTrackInformation(pHBAExt, pLUExt, pSrb);
        break;

    case SCSIOP_MEDIUM_REMOVAL:
        ScsiOpMediumRemoval(pHBAExt, pLUExt, pSrb);
        break;

    case SCSIOP_MODE_SENSE:
        ScsiOpModeSense(pHBAExt, pLUExt, pSrb);
        break;

    case SCSIOP_MODE_SENSE10:
        ScsiOpModeSense10(pHBAExt, pLUExt, pSrb);
        break;

    default:
        KdPrint(("PhDskMnt::ScsiExecute: Unknown opcode=0x%X\n", (int)pSrb->Cdb[0]));
        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
        break;

    } // switch (pSrb->Cdb[0])

Done:
    KdPrint2(("PhDskMnt::ScsiExecute: End: status=0x%X, *pResult=%i\n", (int)pSrb->SrbStatus, (INT)*pResult));

    return;
}                                                     // End ScsiExecute.

VOID
ScsiOpStartStopUnit(
__in pHW_HBA_EXT          pHBAExt,      // Adapter device-object extension from port driver.
__in pHW_LU_EXTENSION     device_extension,       // LUN device-object extension from port driver.
__in PSCSI_REQUEST_BLOCK  pSrb,
__inout __deref PKIRQL         LowestAssumedIrql
)
{
    PCDB                     pCdb = (PCDB)pSrb->Cdb;
    NTSTATUS		     status;
    SRB_IMSCSI_REMOVE_DEVICE srb_io_data = { 0 };

    UNREFERENCED_PARAMETER(pHBAExt);

    KdPrint(("PhDskMnt::ScsiOpStartStopUnit for device %i:%i:%i.\n",
        (int)device_extension->DeviceNumber.PathId,
        (int)device_extension->DeviceNumber.TargetId,
        (int)device_extension->DeviceNumber.Lun));

    if ((pCdb->START_STOP.OperationCode != SCSIOP_START_STOP_UNIT) |
        (pCdb->START_STOP.LoadEject != 1) |
        (pCdb->START_STOP.Start != 0))
    {
        KdPrint(("PhDskMnt::ScsiOpStartStopUnit (unknown op) for device %i:%i:%i.\n",
            (int)device_extension->DeviceNumber.PathId,
            (int)device_extension->DeviceNumber.TargetId,
            (int)device_extension->DeviceNumber.Lun));

        ScsiSetSuccess(pSrb, 0);
        return;
    }

    //if (!device_extension->RemovableMedia)
    //{
    //    KdPrint(("PhDskMnt::ScsiOpStartStopUnit (eject) invalid for device %i:%i:%i.\n",
    //        (int)device_extension->DeviceNumber.PathId,
    //        (int)device_extension->DeviceNumber.TargetId,
    //        (int)device_extension->DeviceNumber.Lun));

    //    ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
    //    return;
    //}

    KdPrint(("PhDskMnt::ScsiOpStartStopUnit (eject) received for device %i:%i:%i.\n",
        (int)device_extension->DeviceNumber.PathId,
        (int)device_extension->DeviceNumber.TargetId,
        (int)device_extension->DeviceNumber.Lun));

    srb_io_data.DeviceNumber = device_extension->DeviceNumber;

    status = ImScsiRemoveDevice(pHBAExt, &srb_io_data.DeviceNumber, LowestAssumedIrql);

    KdPrint(("PhDskMnt::ScsiOpStartStopUnit (eject) result for device %i:%i:%i was %#x.\n",
        (int)device_extension->DeviceNumber.PathId,
        (int)device_extension->DeviceNumber.TargetId,
        (int)device_extension->DeviceNumber.Lun,
        status));

    if (!NT_SUCCESS(status))
    {
        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
    }
    else
    {
        ScsiSetSuccess(pSrb, pSrb->DataTransferLength);
    }
}

VOID
ScsiOpMediumRemoval(__in pHW_HBA_EXT          pHBAExt,      // Adapter device-object extension from port driver.
__in pHW_LU_EXTENSION     device_extension,       // LUN device-object extension from port driver.
__in PSCSI_REQUEST_BLOCK  pSrb
)
{
    UNREFERENCED_PARAMETER(pHBAExt);
    UNREFERENCED_PARAMETER(device_extension);

    KdPrint(("PhDskMnt::ScsiOpMediumRemoval for device %i:%i:%i.\n",
        (int)device_extension->DeviceNumber.PathId,
        (int)device_extension->DeviceNumber.TargetId,
        (int)device_extension->DeviceNumber.Lun));

    if (!device_extension->RemovableMedia)
    {
        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
        return;
    }

    RtlZeroMemory(pSrb->DataBuffer, pSrb->DataTransferLength);

    ScsiSetSuccess(pSrb, pSrb->DataTransferLength);
}

VOID
ScsiOpGetConfiguration(__in pHW_HBA_EXT          pHBAExt,      // Adapter device-object extension from port driver.
__in pHW_LU_EXTENSION     device_extension,       // LUN device-object extension from port driver.
__in PSCSI_REQUEST_BLOCK  pSrb
)
{
    PGET_CONFIGURATION_HEADER output_buffer = (PGET_CONFIGURATION_HEADER)pSrb->DataBuffer;
    // PCDB   cdb = (PCDB)pSrb->Cdb;

    UNREFERENCED_PARAMETER(pHBAExt);

    KdPrint(("PhDskMnt::ScsiOpGetConfiguration for device %i:%i:%i.\n",
        (int)device_extension->DeviceNumber.PathId,
        (int)device_extension->DeviceNumber.TargetId,
        (int)device_extension->DeviceNumber.Lun));

    if ((device_extension->DeviceType != READ_ONLY_DIRECT_ACCESS_DEVICE) ||
        (pSrb->DataTransferLength < sizeof(GET_CONFIGURATION_HEADER)))
    {
        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
        return;
    }

    RtlZeroMemory(output_buffer, pSrb->DataTransferLength);

    // ToDo: Support GET_CONFIGURATION SCSI requests for CD/DVD.

    *(PUSHORT)output_buffer->CurrentProfile = RtlUshortByteSwap(ProfileDvdRom);

    ScsiSetSuccess(pSrb, sizeof(GET_CONFIGURATION_HEADER));
    return;
}

VOID
ScsiOpReadTrackInformation(__in pHW_HBA_EXT          pHBAExt,      // Adapter device-object extension from port driver.
__in pHW_LU_EXTENSION     device_extension,       // LUN device-object extension from port driver.
__in PSCSI_REQUEST_BLOCK  pSrb
)
{
    PUCHAR output_buffer = (PUCHAR)pSrb->DataBuffer;
    PCDB   cdb = (PCDB)pSrb->Cdb;

    UNREFERENCED_PARAMETER(pHBAExt);

    if ((device_extension->DeviceType != READ_ONLY_DIRECT_ACCESS_DEVICE) ||
        (pSrb->DataTransferLength < sizeof(SCSIOP_READ_DISC_INFORMATION)))
    {
        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
        return;
    }

    KdPrint(("PhDskMnt::ScsiOpReadTrackInformation for device %i:%i:%i. Data type req = %#x\n",
        (int)device_extension->DeviceNumber.PathId,
        (int)device_extension->DeviceNumber.TargetId,
        (int)device_extension->DeviceNumber.Lun,
        (int)cdb->READ_TRACK_INFORMATION.Lun));

    RtlZeroMemory(output_buffer, pSrb->DataTransferLength);

    cdb;

    //switch (cdb->READ_TRACK_INFORMATION.Lun)
    //{
        //   case 0x00:
        //if (pSrb->DataTransferLength < 34)
        //{
        //    ScsiSetError(pSrb, SRB_STATUS_DATA_OVERRUN);
        //    return;
        //}
        //output_buffer[2] = 0x0A;
        //output_buffer[7] = 0x20;

        //ScsiSetSuccess(pSrb, 34);
        //return;

    //default:
        ScsiSetError(pSrb, SRB_STATUS_ERROR);
        return;
    //}
}

VOID
ScsiOpReadDiscInformation(__in pHW_HBA_EXT          pHBAExt,      // Adapter device-object extension from port driver.
__in pHW_LU_EXTENSION     device_extension,       // LUN device-object extension from port driver.
__in PSCSI_REQUEST_BLOCK  pSrb
)
{
    PUCHAR output_buffer = (PUCHAR)pSrb->DataBuffer;
    PCDB   cdb = (PCDB)pSrb->Cdb;

    UNREFERENCED_PARAMETER(pHBAExt);

    if ((device_extension->DeviceType != READ_ONLY_DIRECT_ACCESS_DEVICE) ||
        (pSrb->DataTransferLength < sizeof(SCSIOP_READ_DISC_INFORMATION)))
    {
        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
        return;
    }

    KdPrint(("PhDskMnt::ScsiOpReadDiscInformation for device %i:%i:%i. Data type req = %#x\n",
        (int)device_extension->DeviceNumber.PathId,
        (int)device_extension->DeviceNumber.TargetId,
        (int)device_extension->DeviceNumber.Lun,
        (int)cdb->READ_DISC_INFORMATION.Lun));

    RtlZeroMemory(output_buffer, pSrb->DataTransferLength);

    switch (cdb->READ_DISC_INFORMATION.Lun)
    {
    case 0x00:
        if (pSrb->DataTransferLength < 34)
        {
            ScsiSetError(pSrb, SRB_STATUS_DATA_OVERRUN);
            return;
        }
        output_buffer[2] = 0x0A;
        output_buffer[7] = 0x20;

        ScsiSetSuccess(pSrb, 34);
        return;

    default:
        ScsiSetError(pSrb, SRB_STATUS_ERROR);
        return;
    }
}

VOID
ScsiOpReadTOC(__in pHW_HBA_EXT          pHBAExt,      // Adapter device-object extension from port driver.
__in pHW_LU_EXTENSION     device_extension,       // LUN device-object extension from port driver.
__in PSCSI_REQUEST_BLOCK  pSrb
)
{
    PCDROM_TOC cdrom_toc = (PCDROM_TOC)pSrb->DataBuffer;
    PCDB       cdb = (PCDB)pSrb->Cdb;

    UNREFERENCED_PARAMETER(pHBAExt);

    KdPrint(("PhDskMnt::ScsiOpReadTOC for device %i:%i:%i.\n",
        (int)device_extension->DeviceNumber.PathId,
        (int)device_extension->DeviceNumber.TargetId,
        (int)device_extension->DeviceNumber.Lun));

    if (device_extension->DeviceType != READ_ONLY_DIRECT_ACCESS_DEVICE)
    {
        KdPrint(("PhDskMnt::ScsiOpReadTOC not supported for device %i:%i:%i.\n",
            (int)device_extension->DeviceNumber.PathId,
            (int)device_extension->DeviceNumber.TargetId,
            (int)device_extension->DeviceNumber.Lun));

        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
        return;
    }

    /*
    #define READ_TOC_FORMAT_TOC         0x00
    #define READ_TOC_FORMAT_SESSION     0x01
    #define READ_TOC_FORMAT_FULL_TOC    0x02
    #define READ_TOC_FORMAT_PMA         0x03
    #define READ_TOC_FORMAT_ATIP        0x04
    */
    
    KdPrint2(("PhDskMnt::ScsiOpReadTOC: Msf = %d\n", cdb->READ_TOC.Msf));
    KdPrint2(("PhDskMnt::ScsiOpReadTOC: LogicalUnitNumber = %d\n", cdb->READ_TOC.LogicalUnitNumber));
    KdPrint2(("PhDskMnt::ScsiOpReadTOC: Format2 = %d\n", cdb->READ_TOC.Format2));
    KdPrint2(("PhDskMnt::ScsiOpReadTOC: StartingTrack = %d\n", cdb->READ_TOC.StartingTrack));
    KdPrint2(("PhDskMnt::ScsiOpReadTOC: AllocationLength = %d\n", (cdb->READ_TOC.AllocationLength[0] << 8) | cdb->READ_TOC.AllocationLength[1]));
    KdPrint2(("PhDskMnt::ScsiOpReadTOC: Control = %d\n", cdb->READ_TOC.Control));
    KdPrint2(("PhDskMnt::ScsiOpReadTOC: Format = %d\n", cdb->READ_TOC.Format));

    switch (cdb->READ_TOC.Format2)
    {
    case READ_TOC_FORMAT_TOC:
    case READ_TOC_FORMAT_SESSION:
    case READ_TOC_FORMAT_FULL_TOC:
    {
        USHORT size = FIELD_OFFSET(CDROM_TOC, TrackData) + sizeof(TRACK_DATA);

        if (pSrb->DataTransferLength < size)
        {
            ScsiSetError(pSrb, SRB_STATUS_DATA_OVERRUN);
            break;
        }

        RtlZeroMemory(cdrom_toc, size);

        *(PUSHORT)cdrom_toc->Length = RtlUshortByteSwap(size);
        cdrom_toc->FirstTrack = 1;
        cdrom_toc->LastTrack = 1;
        cdrom_toc->TrackData[0].Adr = 1;
        cdrom_toc->TrackData[0].Control = TOC_DATA_TRACK;

        ScsiSetSuccess(pSrb, size);

        //data_buffer[0] = 0; // length MSB
        //data_buffer[1] = 10; // length LSB
        //data_buffer[2] = 1; // First Track
        //data_buffer[3] = 1; // Last Track
        //data_buffer[4] = 0; // Reserved
        //data_buffer[5] = 0x14; // current position data + uninterrupted data
        //data_buffer[6] = 1; // last complete track
        //data_buffer[7] = 0; // reserved
        //data_buffer[8] = 0; // MSB Block
        //data_buffer[9] = 0;
        //data_buffer[10] = 0;
        //data_buffer[11] = 0; // LSB Block

        //ScsiSetSuccess(pSrb, 12);

        break;
    }

    case READ_TOC_FORMAT_PMA:
    case READ_TOC_FORMAT_ATIP:
        ScsiSetError(pSrb, SRB_STATUS_ERROR);
        break;

    default:
        ScsiSetError(pSrb, SRB_STATUS_ERROR);
        break;
    }
}

/**************************************************************************************************/
/*                                                                                                */
/**************************************************************************************************/
VOID
ScsiOpInquiry(
__in pHW_HBA_EXT          pHBAExt,      // Adapter device-object extension from port driver.
__in pHW_LU_EXTENSION     pLUExt,       // LUN device-object extension from port driver.
__in PSCSI_REQUEST_BLOCK  pSrb
)
{
    PINQUIRYDATA          pInqData = (PINQUIRYDATA)pSrb->DataBuffer;// Point to Inquiry buffer.
    PCDB                  pCdb;

    KdPrint2(("PhDskMnt::ScsiOpInquiry:  pHBAExt = 0x%p, pLUExt=0x%p, pSrb=0x%p\n", pHBAExt, pLUExt, pSrb));

    RtlZeroMemory((PUCHAR)pSrb->DataBuffer, pSrb->DataTransferLength);

    pCdb = (PCDB)pSrb->Cdb;

    if (pCdb->CDB6INQUIRY3.EnableVitalProductData == 1)
    {
        KdPrint(("PhDskMnt::ScsiOpInquiry: Received VPD request for page 0x%X\n", pCdb->CDB6INQUIRY.PageCode));

        // Current implementation of ScsiOpVPDRaidControllerUnit seems somewhat dangerous and could cause buffer
        // overruns. For now, just skip Vital Product Data requests.
#if 0
        ScsiOpVPDRaidControllerUnit(pHBAExt, pLUExt, pSrb);
#else
        ScsiSetCheckCondition(
            pSrb,
            SRB_STATUS_ERROR,
            SCSI_SENSE_ILLEGAL_REQUEST,
            SCSI_ADSENSE_INVALID_CDB,
            0);
#endif

        goto done;
    }

    if (!KeReadStateEvent(&pLUExt->Initialized))
    {
        KdPrint(("PhDskMnt::ScsiOpInquiry: Rejected. Device not initialized.\n"));

        ScsiSetCheckCondition(
            pSrb,
            SRB_STATUS_BUSY,
            SCSI_SENSE_NOT_READY,
            SCSI_ADSENSE_LUN_NOT_READY,
            SCSI_SENSEQ_BECOMING_READY);

        goto done;
    }

    pInqData->DeviceType = pLUExt->DeviceType;
    pInqData->RemovableMedia = pLUExt->RemovableMedia;

    pInqData->CommandQueue = TRUE;

    RtlMoveMemory(pInqData->VendorId, pHBAExt->VendorId, 8);
    RtlMoveMemory(pInqData->ProductId, pHBAExt->ProductId, 16);
    RtlMoveMemory(pInqData->ProductRevisionLevel, pHBAExt->ProductRevision, 4);

    ScsiSetSuccess(pSrb, sizeof(INQUIRYDATA));

done:
    KdPrint2(("PhDskMnt::ScsiOpInquiry: End: status=0x%X\n", (int)pSrb->SrbStatus));

    return;
}                                                     // End ScsiOpInquiry.

/**************************************************************************************************/
/*                                                                                                */
/**************************************************************************************************/
UCHAR
ScsiGetLUExtension(
__in pHW_HBA_EXT								pHBAExt,      // Adapter device-object extension from port driver.
pHW_LU_EXTENSION * ppLUExt,
__in UCHAR									PathId,
__in UCHAR									TargetId,
__in UCHAR									Lun,
__in PKIRQL                                 LowestAssumedIrql
)
{
    PLIST_ENTRY           list_ptr;
    pHW_LU_EXTENSION *    pPortLunExt = NULL;
    UCHAR                 status;
    KLOCK_QUEUE_HANDLE    LockHandle;

    *ppLUExt = NULL;

    KdPrint2(("PhDskMnt::ScsiGetLUExtension: %d:%d:%d\n", PathId, TargetId, Lun));

    ImScsiAcquireLock(                   // Serialize the linked list of LUN extensions.              
        &pHBAExt->LUListLock, &LockHandle, *LowestAssumedIrql);

    pPortLunExt = (pHW_LU_EXTENSION*)StoragePortGetLogicalUnit(pHBAExt, PathId, TargetId, Lun);

    if (pPortLunExt == NULL)
    {
        KdPrint(("PhDskMnt::ScsiGetLUExtension: StoragePortGetLogicalUnit failed for %d:%d:%d\n",
            PathId,
            TargetId,
            Lun));

        status = SRB_STATUS_NO_DEVICE;
        goto done;
    }

    if (*pPortLunExt != NULL)
    {
        pHW_LU_EXTENSION pLUExt = *pPortLunExt;

        if ((pLUExt->DeviceNumber.PathId != PathId) |
            (pLUExt->DeviceNumber.TargetId != TargetId) |
            (pLUExt->DeviceNumber.Lun != Lun))
        {
            DbgPrint("PhDskMnt::ScsiGetLUExtension: LUExt %p for device %i:%i:%i returned for device %i:%i:%i!\n",
                pLUExt,
                (int)pLUExt->DeviceNumber.PathId,
                (int)pLUExt->DeviceNumber.TargetId,
                (int)pLUExt->DeviceNumber.Lun,
                (int)PathId,
                (int)TargetId,
                (int)Lun);
        }
        else if (KeReadStateEvent(&pLUExt->StopThread))
        {
            DbgPrint("PhDskMnt::ScsiGetLUExtension: Device %i:%i:%i is stopping. MP reports missing to port driver.\n",
                (int)pLUExt->DeviceNumber.PathId,
                (int)pLUExt->DeviceNumber.TargetId,
                (int)pLUExt->DeviceNumber.Lun);

            *pPortLunExt = NULL;

            status = SRB_STATUS_NO_DEVICE;

            goto done;
        }
        else
        {
            if (!KeReadStateEvent(&pLUExt->Initialized))
            {
                DbgPrint("PhDskMnt::ScsiGetLUExtension: Warning: Device is not yet initialized!\n");
            }

            *ppLUExt = pLUExt;

            KdPrint2(("PhDskMnt::ScsiGetLUExtension: Device %d:%d:%d has pLUExt=0x%p\n",
                PathId, TargetId, Lun, *ppLUExt));

            status = SRB_STATUS_SUCCESS;

            goto done;
        }
    }

    for (list_ptr = pHBAExt->LUList.Flink;
        list_ptr != &pHBAExt->LUList;
        list_ptr = list_ptr->Flink
        )
    {
        pHW_LU_EXTENSION object;
        object = CONTAINING_RECORD(list_ptr, HW_LU_EXTENSION, List);

        if ((object->DeviceNumber.PathId == PathId) &&
            (object->DeviceNumber.TargetId == TargetId) &&
            (object->DeviceNumber.Lun == Lun) &&
            !KeReadStateEvent(&object->StopThread))
        {
            *ppLUExt = object;
            break;
        }
    }

    if (*ppLUExt == NULL)
    {
        KdPrint2(("PhDskMnt::ScsiGetLUExtension: No saved data for Lun.\n"));

        status = SRB_STATUS_NO_DEVICE;

        goto done;
    }

    *pPortLunExt = *ppLUExt;

    status = SRB_STATUS_SUCCESS;

    KdPrint(("PhDskMnt::ScsiGetLUExtension: Device %d:%d:%d get pLUExt=0x%p\n",
        PathId, TargetId, Lun, *ppLUExt));

done:

    ImScsiReleaseLock(&LockHandle, LowestAssumedIrql);

    KdPrint2(("PhDskMnt::ScsiGetLUExtension: End: status=0x%X\n", (int)status));

    return status;
}                                                     // End ScsiOpInquiry.

/**************************************************************************************************/
/*                                                                                                */
/**************************************************************************************************/
#ifdef USE_SCSIPORT
VOID
ScsiOpVPDRaidControllerUnit(
__in pHW_HBA_EXT          pHBAExt,          // Adapter device-object extension from port driver.
__in PSCSI_REQUEST_BLOCK  pSrb
)
{
    struct _CDB6INQUIRY3 * pVpdInquiry = (struct _CDB6INQUIRY3 *)&pSrb->Cdb;

    UNREFERENCED_PARAMETER(pHBAExt);

    ASSERT(pSrb->DataTransferLength>0);

    KdPrint(("PhDskMnt::ScsiOpVPDRaidControllerUnit:  pHBAExt = 0x%p, pSrb=0x%p\n", pHBAExt, pSrb));

    if (pSrb->DataTransferLength == 0)
    {
        DbgPrint("PhDskMnt::ScsiOpVPDRaidControllerUnit: pSrb->DataTransferLength = 0\n");

        ScsiSetError(pSrb, SRB_STATUS_DATA_OVERRUN);
        return;
    }

    RtlZeroMemory((PUCHAR)pSrb->DataBuffer,           // Clear output buffer.
        pSrb->DataTransferLength);

    switch (pVpdInquiry->PageCode)
    {
    case VPD_SUPPORTED_PAGES:
    { // Inquiry for supported pages?
        PVPD_SUPPORTED_PAGES_PAGE pSupportedPages;
        ULONG len;

        len = sizeof(VPD_SUPPORTED_PAGES_PAGE) + 8;

        if (pSrb->DataTransferLength < len)
        {
            ScsiSetError(pSrb, SRB_STATUS_DATA_OVERRUN);
            return;
        }

        pSupportedPages = (PVPD_SUPPORTED_PAGES_PAGE)pSrb->DataBuffer;             // Point to output buffer.

        pSupportedPages->DeviceType = ARRAY_CONTROLLER_DEVICE;
        pSupportedPages->DeviceTypeQualifier = 0;
        pSupportedPages->PageCode = VPD_SERIAL_NUMBER;
        pSupportedPages->PageLength = 8;                // Enough space for 4 VPD values.
        pSupportedPages->SupportedPageList[0] =         // Show page 0x80 supported.
            VPD_SERIAL_NUMBER;
        pSupportedPages->SupportedPageList[1] =         // Show page 0x83 supported.
            VPD_DEVICE_IDENTIFIERS;

        ScsiSetSuccess(pSrb, len);
    }
    break;

    case VPD_SERIAL_NUMBER:
    {   // Inquiry for serial number?
        PVPD_SERIAL_NUMBER_PAGE pVpd;
        ULONG len;

        len = sizeof(VPD_SERIAL_NUMBER_PAGE) + 8 + 32;
        if (pSrb->DataTransferLength < len)
        {
            ScsiSetError(pSrb, SRB_STATUS_DATA_OVERRUN);
            return;
        }

        pVpd = (PVPD_SERIAL_NUMBER_PAGE)pSrb->DataBuffer;                        // Point to output buffer.

        pVpd->DeviceType = ARRAY_CONTROLLER_DEVICE;
        pVpd->DeviceTypeQualifier = 0;
        pVpd->PageCode = VPD_SERIAL_NUMBER;
        pVpd->PageLength = 8 + 32;

        /* Generate a changing serial number. */
        //sprintf((char *)pVpd->SerialNumber, "%03d%02d%02d%03d0123456789abcdefghijABCDEFGH\n", 
        //    pMPDrvInfoGlobal->DrvInfoNbrMPHBAObj, pLUExt->DeviceNumber.PathId, pLUExt->DeviceNumber.TargetId, pLUExt->DeviceNumber.Lun);

        KdPrint(("PhDskMnt::ScsiOpVPDRaidControllerUnit:  VPD Page: %X Serial No.: %s",
            (int)pVpd->PageCode, (const char *)pVpd->SerialNumber));

        ScsiSetSuccess(pSrb, len);
    }
    break;

    case VPD_DEVICE_IDENTIFIERS:
    { // Inquiry for device ids?
        PVPD_IDENTIFICATION_PAGE pVpid;
        PVPD_IDENTIFICATION_DESCRIPTOR pVpidDesc;
        ULONG len;

#define VPIDNameSize 32
#define VPIDName     "PSSLUNxxx"

        len = sizeof(VPD_IDENTIFICATION_PAGE) + sizeof(VPD_IDENTIFICATION_DESCRIPTOR) + VPIDNameSize;

        if (pSrb->DataTransferLength < len)
        {
            ScsiSetError(pSrb, SRB_STATUS_DATA_OVERRUN);
            return;
        }

        pVpid = (PVPD_IDENTIFICATION_PAGE)pSrb->DataBuffer;                     // Point to output buffer.

        pVpid->PageCode = VPD_DEVICE_IDENTIFIERS;

        pVpidDesc =                                   // Point to first (and only) descriptor.
            (PVPD_IDENTIFICATION_DESCRIPTOR)pVpid->Descriptors;

        pVpidDesc->CodeSet = VpdCodeSetAscii;         // Identifier contains ASCII.
        pVpidDesc->IdentifierType =                   // 
            VpdIdentifierTypeFCPHName;

        /* Generate a changing serial number. */
        _snprintf((char *)pVpidDesc->Identifier, pVpidDesc->IdentifierLength,
            "%03d%02d%02d%03d0123456789abcdefgh\n", pMPDrvInfoGlobal->DrvInfoNbrMPHBAObj,
            pSrb->PathId, pSrb->TargetId, pSrb->Lun);

        pVpidDesc->IdentifierLength =                 // Size of Identifier.
            (UCHAR)strlen((const char *)pVpidDesc->Identifier) - 1;
        pVpid->PageLength =                           // Show length of remainder.
            (UCHAR)(FIELD_OFFSET(VPD_IDENTIFICATION_PAGE, Descriptors) +
            FIELD_OFFSET(VPD_IDENTIFICATION_DESCRIPTOR, Identifier) +
            pVpidDesc->IdentifierLength);

        KdPrint(("PhDskMnt::ScsiOpVPDRaidControllerUnit:  VPD Page 0x83. Identifier=%.*s\n",
            pVpidDesc->IdentifierLength, pVpidDesc->Identifier));

        ScsiSetSuccess(pSrb, len);
    }
    break;

    default:
        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ADSENSE_INVALID_CDB, 0);
    }

    KdPrint2(("PhDskMnt::ScsiOpVPDRaidControllerUnit:  End: status=0x%X\n", (int)pSrb->SrbStatus));

    return;
}                                                     // End ScsiOpVPDRaidControllerUnit().
#endif

/**************************************************************************************************/
/*                                                                                                */
/**************************************************************************************************/
VOID
ScsiOpReadCapacity(
__in pHW_HBA_EXT          pHBAExt, // Adapter device-object extension from port driver.
__in pHW_LU_EXTENSION     pLUExt,  // LUN device-object extension from port driver.
__in __deref PSCSI_REQUEST_BLOCK  pSrb
)
{
    PREAD_CAPACITY_DATA     readCapacity = (PREAD_CAPACITY_DATA)pSrb->DataBuffer;
    PREAD_CAPACITY_DATA_EX  readCapacity16 = (PREAD_CAPACITY_DATA_EX)pSrb->DataBuffer;
    ULARGE_INTEGER          maxBlocks;
    ULONG                   blockSize;

    UNREFERENCED_PARAMETER(pHBAExt);

    KdPrint2(("PhDskMnt::ScsiOpReadCapacity:  pHBAExt = 0x%p, pLUExt=0x%p, pSrb=0x%p, Action=0x%X\n", pHBAExt, pLUExt, pSrb, (int)pSrb->Cdb[0]));

    RtlZeroMemory((PUCHAR)pSrb->DataBuffer, pSrb->DataTransferLength);

    //if ((pLUExt == NULL) ? TRUE : ((pLUExt->DiskSize.QuadPart == 0) | (!pLUExt->Initialized)))
    //{
    //    KdPrint(("PhDskMnt::ScsiOpReadWrite: Rejected. Device not initialized.\n"));

    //    ScsiSetCheckCondition(
    //        pSrb,
    //        SRB_STATUS_BUSY,
    //        SCSI_SENSE_NOT_READY,
    //        SCSI_ADSENSE_LUN_NOT_READY,
    //        SCSI_SENSEQ_BECOMING_READY);

    //    return;
    //}

    blockSize = 1UL << pLUExt->BlockPower;

    KdPrint2(("PhDskMnt::ScsiOpReadCapacity: Block Size: 0x%X\n", blockSize));

    if (pLUExt->DiskSize.QuadPart > 0)
        maxBlocks.QuadPart = (pLUExt->DiskSize.QuadPart >> pLUExt->BlockPower) - 1;
    else
        maxBlocks.QuadPart = 0;

    if (pSrb->Cdb[0] == SCSIOP_READ_CAPACITY)
        if (maxBlocks.QuadPart > MAXULONG)
            maxBlocks.QuadPart = MAXULONG;

    KdPrint2(("PhDskMnt::ScsiOpReadCapacity: Max Blocks: 0x%I64X\n", maxBlocks));

    if (pSrb->Cdb[0] == SCSIOP_READ_CAPACITY)
    {
        REVERSE_BYTES(&readCapacity->BytesPerBlock, &blockSize);
        REVERSE_BYTES(&readCapacity->LogicalBlockAddress, &maxBlocks.LowPart);
    }
    else if (pSrb->Cdb[0] == SCSIOP_READ_CAPACITY16)
    {
        REVERSE_BYTES(&readCapacity16->BytesPerBlock, &blockSize);
        REVERSE_BYTES_QUAD(&readCapacity16->LogicalBlockAddress, &maxBlocks);
    }

    KdPrint2(("PhDskMnt::ScsiOpReadCapacity:  End.\n"));

    ScsiSetSuccess(pSrb, pSrb->DataTransferLength);
    return;
}                                                     // End ScsiOpReadCapacity.

/******************************************************************************************************/
/*                                                                                                    */
/* This routine does the setup for reading or writing. Thread ImScsiDispatchWork is going to be the   */
/* place to do the work since it gets control at PASSIVE_LEVEL and so could do real I/O, could        */
/* wait, etc, etc.                                                                                    */
/*                                                                                                    */
/******************************************************************************************************/
VOID
ScsiOpReadWrite(
__in pHW_HBA_EXT          pHBAExt, // Adapter device-object extension from port driver.
__in pHW_LU_EXTENSION     pLUExt,  // LUN device-object extension from port driver.        
__in PSCSI_REQUEST_BLOCK  pSrb,
__in PUCHAR               pResult,
__in PKIRQL               LowestAssumedIrql
)
{
    PCDB                         pCdb = (PCDB)pSrb->Cdb;
    LONGLONG                     startingSector;
    LONGLONG                     startingOffset;
    ULONG                        numBlocks;
    pMP_WorkRtnParms             pWkRtnParms;
    KLOCK_QUEUE_HANDLE           lock_handle;

    KdPrint2(("PhDskMnt::ScsiOpReadWrite:  pHBAExt = 0x%p, pLUExt=0x%p, pSrb=0x%p\n", pHBAExt, pLUExt, pSrb));

    *pResult = ResultDone;                            // Assume no queuing.

    if ((pCdb->AsByte[0] == SCSIOP_READ16) |
        (pCdb->AsByte[0] == SCSIOP_WRITE16))
    {
        REVERSE_BYTES_QUAD(&startingSector, pCdb->CDB16.LogicalBlock);
    }
    else
    {
        startingSector = 0;
        REVERSE_BYTES(&startingSector, &pCdb->CDB10.LogicalBlockByte0);
    }

    if (startingSector & ~(MAXLONGLONG >> pLUExt->BlockPower))
    {      // Check if startingSector << blockPower fits within a LONGLONG.
        KdPrint(("PhDskMnt::ScsiOpReadWrite: Too large sector number: %I64X\n", startingSector));

        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_HARDWARE_ERROR, SCSI_ADSENSE_ILLEGAL_BLOCK, 0);

        return;
    }

    startingOffset = startingSector << pLUExt->BlockPower;

    numBlocks = pSrb->DataTransferLength >> pLUExt->BlockPower;

    KdPrint2(("PhDskMnt::ScsiOpReadWrite action: 0x%X, starting sector: 0x%I64X, number of blocks: 0x%X\n", (int)pSrb->Cdb[0], startingSector, numBlocks));
    KdPrint2(("PhDskMnt::ScsiOpReadWrite pSrb: 0x%p, pSrb->DataBuffer: 0x%p\n", pSrb, pSrb->DataBuffer));

    if (!KeReadStateEvent(&pLUExt->Initialized))
    {
        KdPrint(("PhDskMnt::ScsiOpReadWrite: Busy. Device not initialized.\n"));

        ScsiSetCheckCondition(
            pSrb,
            SRB_STATUS_BUSY,
            SCSI_SENSE_NOT_READY,
            SCSI_ADSENSE_LUN_NOT_READY,
            SCSI_SENSEQ_BECOMING_READY);

        return;
    }

    // Check device shutdown condition
    if (KeReadStateEvent(&pLUExt->StopThread))
    {
        KdPrint(("PhDskMnt::ScsiOpReadWrite: Rejected. Device shutting down.\n"));

        ScsiSetError(pSrb, SRB_STATUS_NO_DEVICE);

        return;
    }

    // Check write protection
    if (((pSrb->Cdb[0] == SCSIOP_WRITE) |
        (pSrb->Cdb[0] == SCSIOP_WRITE16)) &&
        pLUExt->ReadOnly)
    {
        KdPrint(("PhDskMnt::ScsiOpReadWrite: Rejected. Write attempt on read-only device.\n"));

        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_DATA_PROTECT, SCSI_ADSENSE_WRITE_PROTECT, 0);

        return;
    }

    // Check disk bounds
    if ((startingSector + numBlocks) > (pLUExt->DiskSize.QuadPart >> pLUExt->BlockPower))
    {      // Starting sector beyond the bounds?
        KdPrint(("PhDskMnt::ScsiOpReadWrite: Out of bounds: sector: %I64X, blocks: %d\n", startingSector, numBlocks));

        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_HARDWARE_ERROR, SCSI_ADSENSE_ILLEGAL_BLOCK, 0);

        return;
    }

    // Intermediate non-paged cache
    if (pLUExt->FileObject == NULL)
    {
        ImScsiAcquireLock(&pLUExt->LastIoLock, &lock_handle, *LowestAssumedIrql);

        if (((pSrb->Cdb[0] == SCSIOP_READ) |
            (pSrb->Cdb[0] == SCSIOP_READ16)) &
            (pLUExt->LastIoBuffer != NULL) &
            (pLUExt->LastIoStartSector <= startingSector) &
            ((startingOffset - (pLUExt->LastIoStartSector << pLUExt->BlockPower) + pSrb->DataTransferLength) <= pLUExt->LastIoLength))
        {
            PVOID sysaddress = NULL;
            ULONG storage_status;

            storage_status = StoragePortGetSystemAddress(pHBAExt, pSrb, &sysaddress);
            if ((storage_status != STORAGE_STATUS_SUCCESS) | (sysaddress == NULL))
            {
                ImScsiReleaseLock(&lock_handle, LowestAssumedIrql);

                DbgPrint("PhDskMnt::ScsiOpReadWrite: StorPortGetSystemAddress failed: status=0x%X address=0x%p translated=0x%p\n",
                    storage_status,
                    pSrb->DataBuffer,
                    sysaddress);

                ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_HARDWARE_ERROR, SCSI_ADSENSE_NO_SENSE, 0);
            }
            else
            {
                KdPrint(("PhDskMnt::ScsiOpReadWrite: Intermediate cache hit.\n"));

                RtlMoveMemory(
                    sysaddress,
                    (PUCHAR)pLUExt->LastIoBuffer + startingOffset - (pLUExt->LastIoStartSector << pLUExt->BlockPower),
                    pSrb->DataTransferLength
                    );

                ImScsiReleaseLock(&lock_handle, LowestAssumedIrql);

                ScsiSetSuccess(pSrb, pSrb->DataTransferLength);
            }

            return;
        }

        ImScsiReleaseLock(&lock_handle, LowestAssumedIrql);
    }

    pWkRtnParms =                                     // Allocate parm area for work routine.
        (pMP_WorkRtnParms)ExAllocatePoolWithTag(NonPagedPool, sizeof(MP_WorkRtnParms), MP_TAG_GENERAL);

    if (pWkRtnParms == NULL)
    {
        DbgPrint("PhDskMnt::ScsiOpReadWrite Failed to allocate work parm structure\n");

        ScsiSetCheckCondition(pSrb, SRB_STATUS_ERROR, SCSI_SENSE_HARDWARE_ERROR, SCSI_ADSENSE_NO_SENSE, 0);
        return;
    }

    RtlZeroMemory(pWkRtnParms, sizeof(MP_WorkRtnParms));

    pWkRtnParms->pHBAExt = pHBAExt;
    pWkRtnParms->pLUExt = pLUExt;
    pWkRtnParms->pSrb = pSrb;

    if (pLUExt->FileObject != NULL)
    {
        // Service work item directly in calling thread context.

        ImScsiParallelReadWriteImage(pWkRtnParms, pResult, LowestAssumedIrql);
    }
    else
    {
        // Queue work item, which will run in the System process.

        KdPrint2(("PhDskMnt::ScsiOpReadWrite: Queueing work=0x%p\n", pWkRtnParms));

        ImScsiAcquireLock(&pLUExt->RequestListLock, &lock_handle, *LowestAssumedIrql);

        InsertTailList(&pLUExt->RequestList, &pWkRtnParms->RequestListEntry);

        ImScsiReleaseLock(&lock_handle, LowestAssumedIrql);

        KeSetEvent(&pLUExt->RequestEvent, (KPRIORITY)0, FALSE);

        *pResult = ResultQueued;                          // Indicate queuing.
    }

    KdPrint2(("PhDskMnt::ScsiOpReadWrite:  End. *Result=%i\n", (INT)*pResult));
}                                                     // End ScsiReadWriteSetup.

/**************************************************************************************************/
/*                                                                                                */
/**************************************************************************************************/
VOID
ScsiOpModeSense(
__in pHW_HBA_EXT          pHBAExt,    // Adapter device-object extension from port driver.
__in pHW_LU_EXTENSION     pLUExt,     // LUN device-object extension from port driver.
__in __deref PSCSI_REQUEST_BLOCK  pSrb
)
{
    PMODE_PARAMETER_HEADER mph = (PMODE_PARAMETER_HEADER)pSrb->DataBuffer;

    UNREFERENCED_PARAMETER(pHBAExt);

    KdPrint2(("PhDskMnt::ScsiOpModeSense:  pHBAExt = 0x%p, pLUExt=0x%p, pSrb=0x%p\n", pHBAExt, pLUExt, pSrb));

    RtlZeroMemory((PUCHAR)pSrb->DataBuffer, pSrb->DataTransferLength);

    if (pSrb->DataTransferLength < sizeof(MODE_PARAMETER_HEADER))
    {
        KdPrint(("PhDskMnt::ScsiOpModeSense:  Invalid request length.\n"));
        ScsiSetError(pSrb, SRB_STATUS_DATA_OVERRUN);
        return;
    }

    mph->ModeDataLength = sizeof(MODE_PARAMETER_HEADER);
    if (pLUExt != NULL ? pLUExt->ReadOnly : FALSE)
        mph->DeviceSpecificParameter = MODE_DSP_WRITE_PROTECT;

    if (pLUExt != NULL ? pLUExt->RemovableMedia : FALSE)
        mph->MediumType = RemovableMedia;

    ScsiSetSuccess(pSrb, pSrb->DataTransferLength);
}

/**************************************************************************************************/
/*                                                                                                */
/**************************************************************************************************/
VOID
ScsiOpModeSense10(
__in pHW_HBA_EXT          pHBAExt,    // Adapter device-object extension from port driver.
__in pHW_LU_EXTENSION     pLUExt,     // LUN device-object extension from port driver.
__in __deref PSCSI_REQUEST_BLOCK  pSrb
)
{
    PMODE_PARAMETER_HEADER10 mph = (PMODE_PARAMETER_HEADER10)pSrb->DataBuffer;

    UNREFERENCED_PARAMETER(pHBAExt);
    UNREFERENCED_PARAMETER(pLUExt);

    KdPrint(("PhDskMnt::ScsiOpModeSense10:  pHBAExt = 0x%p, pLUExt=0x%p, pSrb=0x%p\n", pHBAExt, pLUExt, pSrb));

    RtlZeroMemory((PUCHAR)pSrb->DataBuffer, pSrb->DataTransferLength);

    if (pSrb->DataTransferLength < sizeof(MODE_PARAMETER_HEADER10))
    {
        KdPrint(("PhDskMnt::ScsiOpModeSense10:  Invalid request length.\n"));
        ScsiSetError(pSrb, SRB_STATUS_DATA_OVERRUN);
        return;
    }

    mph->ModeDataLength[1] = sizeof(MODE_PARAMETER_HEADER10);
    if (pLUExt != NULL ? pLUExt->ReadOnly : FALSE)
        mph->DeviceSpecificParameter = MODE_DSP_WRITE_PROTECT;

    if (pLUExt != NULL ? pLUExt->RemovableMedia : FALSE)
        mph->MediumType = RemovableMedia;

    ScsiSetSuccess(pSrb, pSrb->DataTransferLength);
}

/**************************************************************************************************/
/*                                                                                                */
/**************************************************************************************************/
VOID
ScsiOpReportLuns(
__inout         pHW_HBA_EXT         pHBAExt,   // Adapter device-object extension from port driver.
__in    __deref PSCSI_REQUEST_BLOCK pSrb,
__inout __deref PKIRQL              LowestAssumedIrql
)
{
    UCHAR                 count;
    PLIST_ENTRY           list_ptr;
    PLUN_LIST             pLunList = (PLUN_LIST)pSrb->DataBuffer; // Point to LUN list.
    KLOCK_QUEUE_HANDLE    LockHandle;

    KdPrint(("PhDskMnt::ScsiOpReportLuns:  pHBAExt = 0x%p, pSrb=0x%p\n", pHBAExt, pSrb));

    // This opcode will be one of the earliest I/O requests for a new HBA (and may be received later, too).
    pHBAExt->bReportAdapterDone = TRUE;

    RtlZeroMemory((PUCHAR)pSrb->DataBuffer, pSrb->DataTransferLength);

    ImScsiAcquireLock(                   // Serialize the linked list of LUN extensions.              
        &pHBAExt->LUListLock, &LockHandle, *LowestAssumedIrql);

    for (count = 0, list_ptr = pHBAExt->LUList.Flink;
        (count < MAX_LUNS) & (list_ptr != &pHBAExt->LUList);
        list_ptr = list_ptr->Flink
        )
    {
        pHW_LU_EXTENSION object;
        object = CONTAINING_RECORD(list_ptr, HW_LU_EXTENSION, List);

        if ((object->DeviceNumber.PathId == pSrb->PathId) &
            (object->DeviceNumber.TargetId == pSrb->TargetId))
            if (pSrb->DataTransferLength >= FIELD_OFFSET(LUN_LIST, Lun) + (sizeof(pLunList->Lun[0])*count))
                pLunList->Lun[count++][1] = object->DeviceNumber.Lun;
            else
                break;
    }

    ImScsiReleaseLock(&LockHandle, LowestAssumedIrql);

    KdPrint(("PhDskMnt::ScsiOpReportLuns:  Reported %i LUNs\n", (int)count));

    pLunList->LunListLength[3] =                  // Set length needed for LUNs.
        (UCHAR)(8 * count);

    // Set the LUN numbers if there is enough room, and set only those LUNs to be reported.

    ScsiSetSuccess(pSrb, pSrb->DataTransferLength);

    KdPrint2(("PhDskMnt::ScsiOpReportLuns:  End: status=0x%X\n", (int)pSrb->SrbStatus));
}                                                     // End ScsiOpReportLuns.

VOID
ScsiSetCheckCondition(
__in __deref PSCSI_REQUEST_BLOCK pSrb,
__in UCHAR               SrbStatus,
__in UCHAR               SenseKey,
__in UCHAR               AdditionalSenseCode,
__in UCHAR               AdditionalSenseCodeQualifier OPTIONAL
)
{
    PSENSE_DATA mph = (PSENSE_DATA)pSrb->SenseInfoBuffer;

    pSrb->SrbStatus = SrbStatus | SRB_STATUS_AUTOSENSE_VALID;
    pSrb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
    pSrb->DataTransferLength = 0;

    RtlZeroMemory((PUCHAR)pSrb->SenseInfoBuffer, pSrb->SenseInfoBufferLength);

    if (pSrb->SenseInfoBufferLength < sizeof(SENSE_DATA))
    {
        DbgPrint("PhDskMnt::ScsiSetCheckCondition:  Insufficient sense data buffer.\n");
        return;
    }

    mph->SenseKey = SenseKey;
    mph->AdditionalSenseCode = AdditionalSenseCode;
    mph->AdditionalSenseCodeQualifier = AdditionalSenseCodeQualifier;
}

UCHAR
ScsiResetLun(
__in PVOID               pHBAExt,
__in PSCSI_REQUEST_BLOCK pSrb
)
{
    UNREFERENCED_PARAMETER(pHBAExt);

    ScsiSetSuccess(pSrb, 0);
    return SRB_STATUS_SUCCESS;
}

/**************************************************************************************************/
/*                                                                                                */
/**************************************************************************************************/
VOID
ScsiPnPRemoveDevice(
__in pHW_HBA_EXT             pHBAExt,// Adapter device-object extension from port driver.
__in PSCSI_PNP_REQUEST_BLOCK pSrb,
__inout __deref PKIRQL            LowestAssumedIrql
)
{
    DEVICE_NUMBER remove_data = { 0 };

    KdPrint(("PhDskMnt::ScsiPnPRemoveDevice:  pHBAExt = 0x%p, pSrb = 0x%p\n", pHBAExt, pSrb));

    remove_data.LongNumber = IMSCSI_ALL_DEVICES;

    ImScsiRemoveDevice(pHBAExt, &remove_data, LowestAssumedIrql);

    pSrb->SrbStatus = SRB_STATUS_SUCCESS;

    return;
}                                                     // End ScsiPnPRemoveDevice().

/**************************************************************************************************/
/*                                                                                                */
/**************************************************************************************************/
VOID
ScsiPnPQueryCapabilities(
__in pHW_HBA_EXT             pHBAExt,// Adapter device-object extension from port driver.
__in PSCSI_PNP_REQUEST_BLOCK pSrb,
__inout __deref PKIRQL               LowestAssumedIrql
)
{
    pHW_LU_EXTENSION          pLUExt;
    PSTOR_DEVICE_CAPABILITIES pStorageCapabilities = (PSTOR_DEVICE_CAPABILITIES)pSrb->DataBuffer;

    UNREFERENCED_PARAMETER(pHBAExt);

    KdPrint(("PhDskMnt::ScsiPnPQueryCapabilities:  pHBAExt = 0x%p, pSrb = 0x%p\n", pHBAExt, pSrb));

    // Get the LU extension from port driver.
    pSrb->SrbStatus = ScsiGetLUExtension(pHBAExt, &pLUExt, pSrb->PathId,
        pSrb->TargetId, pSrb->Lun, LowestAssumedIrql);

    if (pSrb->SrbStatus != SRB_STATUS_SUCCESS)
    {
        pSrb->DataTransferLength = 0;

        KdPrint(("PhDskMnt::ScsiPnP: No LUN object yet for device %d:%d:%d\n", pSrb->PathId, pSrb->TargetId, pSrb->Lun));

        return;
    }

    // Set SCSI check conditions if LU is not yet ready
    if (!KeReadStateEvent(&pLUExt->Initialized))
    {
        pSrb->DataTransferLength = 0;
        pSrb->SrbStatus = SRB_STATUS_BUSY;

        KdPrint(("PhDskMnt::ScsiPnP: Device %d:%d:%d not yet ready.\n", pSrb->PathId, pSrb->TargetId, pSrb->Lun));

        return;
    }

    RtlZeroMemory(pStorageCapabilities, pSrb->DataTransferLength);

    pStorageCapabilities->EjectSupported = TRUE;
    pStorageCapabilities->SilentInstall = TRUE;
    pStorageCapabilities->Removable = pLUExt->RemovableMedia;
    pStorageCapabilities->SurpriseRemovalOK = FALSE;

    pSrb->SrbStatus = SRB_STATUS_SUCCESS;

    return;
}                                                     // End ScsiPnPQueryCapabilities().

/**************************************************************************************************/
/*                                                                                                */
/**************************************************************************************************/
VOID
ScsiPnP(
__in pHW_HBA_EXT              pHBAExt,  // Adapter device-object extension from port driver.
__in PSCSI_PNP_REQUEST_BLOCK  pSrb,
__inout __deref PKIRQL             LowestAssumedIrql
)
{
    KdPrint(("PhDskMnt::ScsiPnP for device %d:%d:%d:  pHBAExt = 0x%p, PnPAction = %#x, pSrb = 0x%p\n",
        pSrb->PathId, pSrb->TargetId, pSrb->Lun, pHBAExt, pSrb->PnPAction, pSrb));

    // Handle sufficient opcodes to support a LUN suitable for a file system. Other opcodes are just completed.

    switch (pSrb->PnPAction)
    {

    case StorRemoveDevice:
        ScsiPnPRemoveDevice(pHBAExt, pSrb, LowestAssumedIrql);
        break;

    case StorQueryCapabilities:
        ScsiPnPQueryCapabilities(pHBAExt, pSrb, LowestAssumedIrql);
        break;

    default:
        pSrb->SrbStatus = SRB_STATUS_SUCCESS;         // Do nothing.
    }

    KdPrint2(("PhDskMnt::ScsiPnP:  status = 0x%X\n", status));

    return;
}                                                     // End ScsiPnP().

UCHAR
ScsiResetDevice(
__in PVOID               pHBAExt,
__in PSCSI_REQUEST_BLOCK pSrb
)
{
    UNREFERENCED_PARAMETER(pHBAExt);

    ScsiSetSuccess(pSrb, 0);
    return SRB_STATUS_SUCCESS;
}

