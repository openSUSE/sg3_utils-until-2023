/*
 * Copyright (c) 2006-2018 Douglas Gilbert.
 * All rights reserved.
 * Use of this source code is governed by a BSD-style
 * license that can be found in the BSD_LICENSE file.
 */

/* sg_pt_win32 version 1.21 20180203 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#define __STDC_FORMAT_MACROS 1
#include <inttypes.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "sg_lib.h"
#include "sg_unaligned.h"
#include "sg_pt.h"
#include "sg_pt_win32.h"
#include "sg_pt_nvme.h"


#ifndef O_EXCL
// #define O_EXCL 0x80  // cygwin ??
// #define O_EXCL 0x80  // Linux
#define O_EXCL 0x400    // mingw
#warning "O_EXCL not defined"
#endif

#define SCSI_INQUIRY_OPC     0x12
#define SCSI_REPORT_LUNS_OPC 0xa0
#define SCSI_TEST_UNIT_READY_OPC  0x0
#define SCSI_REQUEST_SENSE_OPC  0x3
#define SCSI_SEND_DIAGNOSTIC_OPC  0x1d
#define SCSI_RECEIVE_DIAGNOSTIC_OPC  0x1c
#define SCSI_MAINT_IN_OPC  0xa3
#define SCSI_REP_SUP_OPCS_OPC  0xc
#define SCSI_REP_SUP_TMFS_OPC  0xd

/* Additional Sense Code (ASC) */
#define NO_ADDITIONAL_SENSE 0x0
#define LOGICAL_UNIT_NOT_READY 0x4
#define LOGICAL_UNIT_COMMUNICATION_FAILURE 0x8
#define UNRECOVERED_READ_ERR 0x11
#define PARAMETER_LIST_LENGTH_ERR 0x1a
#define INVALID_OPCODE 0x20
#define LBA_OUT_OF_RANGE 0x21
#define INVALID_FIELD_IN_CDB 0x24
#define INVALID_FIELD_IN_PARAM_LIST 0x26
#define UA_RESET_ASC 0x29
#define UA_CHANGED_ASC 0x2a
#define TARGET_CHANGED_ASC 0x3f
#define LUNS_CHANGED_ASCQ 0x0e
#define INSUFF_RES_ASC 0x55
#define INSUFF_RES_ASCQ 0x3
#define LOW_POWER_COND_ON_ASC  0x5e     /* ASCQ=0 */
#define POWER_ON_RESET_ASCQ 0x0
#define BUS_RESET_ASCQ 0x2      /* scsi bus reset occurred */
#define MODE_CHANGED_ASCQ 0x1   /* mode parameters changed */
#define CAPACITY_CHANGED_ASCQ 0x9
#define SAVING_PARAMS_UNSUP 0x39
#define TRANSPORT_PROBLEM 0x4b
#define THRESHOLD_EXCEEDED 0x5d
#define LOW_POWER_COND_ON 0x5e
#define MISCOMPARE_VERIFY_ASC 0x1d
#define MICROCODE_CHANGED_ASCQ 0x1      /* with TARGET_CHANGED_ASC */
#define MICROCODE_CHANGED_WO_RESET_ASCQ 0x16

/* Use the Microsoft SCSI Pass Through (SPT) interface. It has two
 * variants: "SPT" where data is double buffered; and "SPTD" where data
 * pointers to the user space are passed to the OS. Only Windows
 * 2000 and later (i.e. not 95,98 or ME).
 * There is no ASPI interface which relies on a dll from adaptec.
 * This code uses cygwin facilities and is built in a cygwin
 * shell. It can be run in a normal DOS shell if the cygwin1.dll
 * file is put in an appropriate place.
 * This code can build in a MinGW environment.
 *
 * N.B. MSDN says that the "SPT" interface (i.e. double buffered)
 * should be used for small amounts of data (it says "< 16 KB").
 * The direct variant (i.e. IOCTL_SCSI_PASS_THROUGH_DIRECT) should
 * be used for larger amounts of data but the buffer needs to be
 * "cache aligned". Is that 16 byte alignment or greater?
 *
 * This code will default to indirect (i.e. double buffered) access
 * unless the WIN32_SPT_DIRECT preprocessor constant is defined in
 * config.h . In version 1.12 runtime selection of direct and indirect
 * access was added; the default is still determined by the
 * WIN32_SPT_DIRECT preprocessor constant.
 */

#define DEF_TIMEOUT 60       /* 60 seconds */
#define MAX_OPEN_SIMULT 8
#define WIN32_FDOFFSET 32

union STORAGE_DEVICE_DESCRIPTOR_DATA {
    STORAGE_DEVICE_DESCRIPTOR desc;
    char raw[256];
};

union STORAGE_DEVICE_UID_DATA {
    STORAGE_DEVICE_UNIQUE_IDENTIFIER desc;
    char raw[1060];
};


struct sg_pt_handle {
    bool in_use;
    bool checked_handle;
    bool bus_type_failed;
    bool is_nvme;
    HANDLE fh;
    char adapter[32];
    int bus;
    int target;
    int lun;
    int verbose;        /* tunnel verbose through to scsi_pt_close_device */
    char dname[20];
};

static struct sg_pt_handle handle_arr[MAX_OPEN_SIMULT];

struct sg_pt_win32_scsi {
    bool is_nvme;
    bool nvme_direct;   /* false: our SNTL; true: received NVMe command */
    bool mdxfer_out;    /* direction of metadata xfer, true->data-out */
    bool scsi_dsense;   /* SCSI "descriptor" sense format, active when true */
    bool have_nvme_cmd;
    bool is_read;
    int sense_len;
    int scsi_status;
    int resid;
    int sense_resid;
    int in_err;
    int os_err;                 /* pseudo unix error */
    int transport_err;          /* windows error number */
    int dev_fd;                 /* -1 for no "file descriptor" given */
    uint32_t nvme_nsid;         /* 1 to 0xfffffffe are possibly valid, 0
                                 * implies dev_fd is not a NVMe device
                                 * (is_nvme=false) or has no storage (e.g.
                                 * enclosure rather than disk) */
    uint32_t nvme_result;       /* DW0 from completion queue */
    uint32_t nvme_status;       /* SCT|SC: DW3 27:17 from completion queue,
                                 * note: the DNR+More bit are not there.
                                 * The whole 16 byte completion q entry is
                                 * sent back as sense data */
    uint32_t dxfer_len;
    uint32_t mdxfer_len;
    uint8_t * dxferp;
    uint8_t * mdxferp;    /* NVMe has metadata buffer */
    uint8_t * sensep;
    uint8_t * nvme_id_ctlp;
    uint8_t * free_nvme_id_ctlp;
    uint8_t nvme_cmd[64];
    union {
        SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER swb_d;
        /* Last entry in structure so data buffer can be extended */
        SCSI_PASS_THROUGH_WITH_BUFFERS swb_i;
    };
};

/* embed pointer so can change on fly if (non-direct) data buffer
 * is not big enough */
struct sg_pt_base {
    struct sg_pt_win32_scsi * implp;
};

#ifdef WIN32_SPT_DIRECT
static int spt_direct = 1;
#else
static int spt_direct = 0;
#endif

#if defined(__GNUC__) || defined(__clang__)
static int pr2ws(const char * fmt, ...)
        __attribute__ ((format (printf, 1, 2)));
#else
static int pr2ws(const char * fmt, ...);
#endif

static int do_nvme_pt(struct sg_pt_win32_scsi * psp,
                      struct sg_pt_handle * shp, int time_secs, int vb);


static int
pr2ws(const char * fmt, ...)
{
    va_list args;
    int n;

    va_start(args, fmt);
    n = vfprintf(sg_warnings_strm ? sg_warnings_strm : stderr, fmt, args);
    va_end(args);
    return n;
}

#if (HAVE_NVME && (! IGNORE_NVME))
static inline bool is_aligned(const void * pointer, size_t byte_count)
{
    return ((sg_uintptr_t)pointer % byte_count) == 0;
}
#endif


/* Request SPT direct interface when state_direct is 1, state_direct set
 * to 0 for the SPT indirect interface. */
void
scsi_pt_win32_direct(int state_direct)
{
    spt_direct = state_direct;
}

/* Returns current SPT interface state, 1 for direct, 0 for indirect */
int
scsi_pt_win32_spt_state(void)
{
    return spt_direct;
}

static const char *
get_bus_type_str(int bt)
{
    switch (bt)
    {
    case BusTypeUnknown:
        return "Unknown";
    case BusTypeScsi:
        return "Scsi";
    case BusTypeAtapi:
        return "Atapi";
    case BusTypeAta:
        return "Ata";
    case BusType1394:
        return "1394";
    case BusTypeSsa:
        return "Ssa";
    case BusTypeFibre:
        return "Fibre";
    case BusTypeUsb:
        return "Usb";
    case BusTypeRAID:
        return "RAID";
    case BusTypeiScsi:
        return "iScsi";
    case BusTypeSas:
        return "Sas";
    case BusTypeSata:
        return "Sata";
    case BusTypeSd:
        return "Sd";
    case BusTypeMmc:
        return "Mmc";
    case BusTypeVirtual:
        return "Virt";
    case BusTypeFileBackedVirtual:
        return "FBVir";
#ifdef BusTypeSpaces
    case BusTypeSpaces:
#else
    case 0x10:
#endif
        return "Spaces";
#ifdef BusTypeNvme
    case BusTypeNvme:
#else
    case 0x11:
#endif
        return "NVMe";
#ifdef BusTypeSCM
    case BusTypeSCM:
#else
    case 0x12:
#endif
        return "SCM";
#ifdef BusTypeUfs
    case BusTypeUfs:
#else
    case 0x13:
#endif
        return "Ufs";
    case 0x14:
        return "Max";
    case 0x7f:
        return "Max Reserved";
    default:
        return "_unknown";
    }
}

static char *
get_err_str(DWORD err, int max_b_len, char * b)
{
    LPVOID lpMsgBuf;
    int k, num, ch;

    memset(b, 0, max_b_len);
    FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM,
        NULL,
        err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPTSTR) &lpMsgBuf,
        0, NULL );
    num = lstrlen((LPCTSTR)lpMsgBuf);
    if (num < 1)
        return b;
    num = (num < max_b_len) ? num : (max_b_len - 1);
    for (k = 0; k < num; ++k) {
        ch = *((LPCTSTR)lpMsgBuf + k);
        if ((ch >= 0x0) && (ch < 0x7f))
            b[k] = ch & 0x7f;
        else
            b[k] = '?';
    }
    return b;
}

/* Returns pointer to sg_pt_handle object given Unix like device_fd. If
 * device_fd is invalid or not open returns NULL. If psp is non-NULL and
 * NULL is returned then ENODEV is placed in psp->os_err. */
static struct sg_pt_handle *
get_open_pt_handle(struct sg_pt_win32_scsi * psp, int device_fd, bool vbb)
{
    int index = device_fd - WIN32_FDOFFSET;
    struct sg_pt_handle * shp;

    if ((index < 0) || (index >= WIN32_FDOFFSET)) {
        if (vbb)
            pr2ws("Bad file descriptor\n");
        if (psp)
            psp->os_err = EBADF;
        return NULL;
    }
    shp = handle_arr + index;
    if (! shp->in_use) {
        if (vbb)
            pr2ws("File descriptor closed??\n");
        if (psp)
            psp->os_err = ENODEV;
        return NULL;
    }
    return shp;
}


/* Returns >= 0 if successful. If error in Unix returns negated errno. */
int
scsi_pt_open_device(const char * device_name, bool read_only, int vb)
{
    int oflags = 0 /* O_NONBLOCK*/ ;

    oflags |= (read_only ? 0 : 0);      /* was ... ? O_RDONLY : O_RDWR) */
    return scsi_pt_open_flags(device_name, oflags, vb);
}

/*
 * Similar to scsi_pt_open_device() but takes Unix style open flags OR-ed
 * together. The 'flags' argument is ignored in Windows.
 * Returns >= 0 if successful, otherwise returns negated errno.
 * Optionally accept leading "\\.\". If given something of the form
 * "SCSI<num>:<bus>,<target>,<lun>" where the values in angle brackets
 * are integers, then will attempt to open "\\.\SCSI<num>:" and save the
 * other three values for the DeviceIoControl call. The trailing ".<lun>"
 * is optionally and if not given 0 is assumed. Since "PhysicalDrive"
 * is a lot of keystrokes, "PD" is accepted and converted to the longer
 * form.
 */
int
scsi_pt_open_flags(const char * device_name, int flags, int vb)
{
    bool got_scsi_name = false;
    bool got_pd_name = false;
    int len, k, adapter_num, bus, target, lun, off, index, num, pd_num;
    int share_mode;
    struct sg_pt_handle * shp;
    char buff[8];

    share_mode = (O_EXCL & flags) ? 0 : (FILE_SHARE_READ | FILE_SHARE_WRITE);
    /* lock */
    for (k = 0; k < MAX_OPEN_SIMULT; k++)
        if (! handle_arr[k].in_use)
            break;
    if (k == MAX_OPEN_SIMULT) {
        if (vb)
            pr2ws("too many open handles (%d)\n", MAX_OPEN_SIMULT);
        return -EMFILE;
    } else
        handle_arr[k].in_use = true;
    /* unlock */
    index = k;
    shp = handle_arr + index;
    adapter_num = 0;
    bus = 0;    /* also known as 'PathId' in MS docs */
    target = 0;
    lun = 0;
    len = (int)strlen(device_name);
    k = (int)sizeof(shp->dname);
    if (len < k)
        strcpy(shp->dname, device_name);
    else if (len == k)
        memcpy(shp->dname, device_name, k - 1);
    else        /* trim on left */
        memcpy(shp->dname, device_name + (len - k), k - 1);
    shp->dname[k - 1] = '\0';
    if ((len > 4) && (0 == strncmp("\\\\.\\", device_name, 4)))
        off = 4;
    else
        off = 0;
    if (len > (off + 2)) {
        buff[0] = toupper((int)device_name[off + 0]);
        buff[1] = toupper((int)device_name[off + 1]);
        if (0 == strncmp("PD", buff, 2)) {
            num = sscanf(device_name + off + 2, "%d", &pd_num);
            if (1 == num)
                got_pd_name = true;
        }
        if (! got_pd_name) {
            buff[2] = toupper((int)device_name[off + 2]);
            buff[3] = toupper((int)device_name[off + 3]);
            if (0 == strncmp("SCSI", buff, 4)) {
                num = sscanf(device_name + off + 4, "%d:%d,%d,%d",
                             &adapter_num, &bus, &target, &lun);
                if (num < 3) {
                    if (vb)
                        pr2ws("expected format like: "
                              "'SCSI<port>:<bus>,<target>[,<lun>]'\n");
                    shp->in_use = false;
                    return -EINVAL;
                }
                got_scsi_name = true;
            }
        }
    }
    shp->bus = bus;
    shp->target = target;
    shp->lun = lun;
    shp->verbose = vb;
    memset(shp->adapter, 0, sizeof(shp->adapter));
    strncpy(shp->adapter, "\\\\.\\", 4);
    if (got_pd_name)
        snprintf(shp->adapter + 4, sizeof(shp->adapter) - 5,
                 "PhysicalDrive%d", pd_num);
    else if (got_scsi_name)
        snprintf(shp->adapter + 4, sizeof(shp->adapter) - 5, "SCSI%d:",
                 adapter_num);
    else
        snprintf(shp->adapter + 4, sizeof(shp->adapter) - 5, "%s",
                 device_name + off);
    if (vb > 4)
        pr2ws("%s: CreateFile('%s')\n", __func__, shp->adapter);
#if 1
    shp->fh = CreateFile(shp->adapter, GENERIC_READ | GENERIC_WRITE,
                         share_mode, NULL, OPEN_EXISTING, 0, NULL);
#endif

#if 0
    shp->fh = CreateFileA(shp->adapter, GENERIC_READ|GENERIC_WRITE,
    FILE_SHARE_READ|FILE_SHARE_WRITE,
    (SECURITY_ATTRIBUTES *)0, OPEN_EXISTING, 0, 0);
  // No GENERIC_READ/WRITE access required, works without admin rights (W10)
    shp->fh = CreateFileA(shp->adapter, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      (SECURITY_ATTRIBUTES *)0, OPEN_EXISTING, 0, (HANDLE)0);
#endif
    if (shp->fh == INVALID_HANDLE_VALUE) {
        if (vb) {
            uint32_t err = (uint32_t)GetLastError();
            char b[128];

            pr2ws("%s: CreateFile error: %s [%u]\n", __func__,
                  get_err_str(err, sizeof(b), b), err);
        }
        shp->in_use = false;
        return -ENODEV;
    }
    return index + WIN32_FDOFFSET;
}

/* Returns 0 if successful. If device_id seems wild returns -ENODEV,
 * other errors return 0. If CloseHandle() fails and verbose > 0 then
 * outputs warning with value from GetLastError(). The verbose value
 * defaults to zero and is potentially set from the most recent call
 * to scsi_pt_open_device() or do_scsi_pt(). */
int
scsi_pt_close_device(int device_fd)
{
    struct sg_pt_handle * shp = get_open_pt_handle(NULL, device_fd, false);

    if (NULL == shp)
        return -ENODEV;
    if ((! CloseHandle(shp->fh)) && shp->verbose)
        pr2ws("Windows CloseHandle error=%u\n", (unsigned int)GetLastError());
    shp->bus = 0;
    shp->target = 0;
    shp->lun = 0;
    memset(shp->adapter, 0, sizeof(shp->adapter));
    shp->in_use = false;
    shp->verbose = 0;
    shp->dname[0] = '\0';
    return 0;
}

/* Returns 0 on success, negated errno if error */
static int
get_bus_type(struct sg_pt_handle *shp, const char *dname, STORAGE_BUS_TYPE * btp,
             int vb)
{
    DWORD num_out, err;
    STORAGE_BUS_TYPE bt;
    union STORAGE_DEVICE_DESCRIPTOR_DATA sddd;
    STORAGE_PROPERTY_QUERY query = {StorageDeviceProperty,
                                    PropertyStandardQuery, {0} };
    char b[256];

    memset(&sddd, 0, sizeof(sddd));
    if (! DeviceIoControl(shp->fh, IOCTL_STORAGE_QUERY_PROPERTY,
                          &query, sizeof(query), &sddd, sizeof(sddd),
                          &num_out, NULL)) {
        if (vb > 2) {
            err = GetLastError();
            pr2ws("%s  IOCTL_STORAGE_QUERY_PROPERTY(Devprop) failed, "
                  "Error: %s [%u]\n", dname, get_err_str(err, sizeof(b), b),
                  (uint32_t)err);
        }
        shp->bus_type_failed = true;
        return -EIO;
    }
    bt = sddd.desc.BusType;
    if (vb > 2) {
        pr2ws("%s: Bus type: %s\n", __func__, get_bus_type_str((int)bt));
        if (vb > 3) {
            pr2ws("Storage Device Descriptor Data:\n");
            hex2stderr((const uint8_t *)&sddd, num_out, 0);
        }
    }
    if (shp) {
        shp->checked_handle = true;
        shp->is_nvme = (BusTypeNvme == bt);
    }
    if (btp)
        *btp = bt;
    return 0;
}

/* Assumes dev_fd is an "open" file handle associated with device_name. If
 * the implementation (possibly for one OS) cannot determine from dev_fd if
 * a SCSI or NVMe pass-through is referenced, then it might guess based on
 * device_name. Returns 1 if SCSI generic pass-though device, returns 2 if
 * secondary SCSI pass-through device (in Linux a bsg device); returns 3 is
 * char NVMe device (i.e. no NSID); returns 4 if block NVMe device (includes
 * NSID), or 0 if something else (e.g. ATA block device) or dev_fd < 0.
 * If error, returns negated errno (operating system) value. */
int
check_pt_file_handle(int device_fd, const char * device_name, int vb)
{
    int res;
    STORAGE_BUS_TYPE bt;
    const char * dnp = device_name;
    struct sg_pt_handle * shp;

    if (vb > 3)
        pr2ws("%s: device_name: %s\n", __func__, dnp);
    shp = get_open_pt_handle(NULL, device_fd, vb > 1);
    if (NULL == shp) {
        pr2ws("%s: device_fd (%s) bad or not in_use ??\n", __func__,
              dnp ? dnp : "");
        return -ENODEV;
    }
    if (shp->bus_type_failed) {
        if (vb > 2)
            pr2ws("%s: skip because get_bus_type() has failed\n", __func__);
        return 0;
    }
    dnp = dnp ? dnp : shp->dname;
    res = get_bus_type(shp, dnp, &bt, vb);
    if (res < 0)
        return res;
    return (BusTypeNvme == bt) ? 3 : 1;
    /* NVMe "char" ?? device, could be enclosure: 3 */
    /* SCSI generic pass-though device: 1 */
}

struct sg_pt_base *
construct_scsi_pt_obj_with_fd(int dev_fd, int vb)
{
    int res;
    struct sg_pt_win32_scsi * psp;
    struct sg_pt_base * vp = NULL;
    struct sg_pt_handle * shp = NULL;

    if (dev_fd >= 0) {
        shp = get_open_pt_handle(NULL, dev_fd, vb > 1);
        if (NULL == shp) {
            if (vb)
                pr2ws("%s: dev_fd is not open\n", __func__);
            return NULL;
        }
        if (! (shp->bus_type_failed || shp->checked_handle)) {
            res = get_bus_type(shp, shp->dname, NULL, vb);
            if (res < 0)
                pr2ws("%s: get_bus_type() errno=%d, continue\n", __func__,
                      -res);
        }
    }
    psp = (struct sg_pt_win32_scsi *)calloc(sizeof(struct sg_pt_win32_scsi),
                                            1);
    if (psp) {
        if (shp && shp->is_nvme) {
            psp->is_nvme = true;
        } else if (spt_direct) {
            psp->swb_d.spt.DataIn = SCSI_IOCTL_DATA_UNSPECIFIED;
            psp->swb_d.spt.SenseInfoLength = SCSI_MAX_SENSE_LEN;
            psp->swb_d.spt.SenseInfoOffset =
                offsetof(SCSI_PASS_THROUGH_WITH_BUFFERS, ucSenseBuf);
            psp->swb_d.spt.TimeOutValue = DEF_TIMEOUT;
        } else {
            psp->swb_i.spt.DataIn = SCSI_IOCTL_DATA_UNSPECIFIED;
            psp->swb_i.spt.SenseInfoLength = SCSI_MAX_SENSE_LEN;
            psp->swb_i.spt.SenseInfoOffset =
                offsetof(SCSI_PASS_THROUGH_WITH_BUFFERS, ucSenseBuf);
            psp->swb_i.spt.TimeOutValue = DEF_TIMEOUT;
        }
        psp->dev_fd = (dev_fd < 0) ? -1 : dev_fd;
        vp = malloc(sizeof(struct sg_pt_win32_scsi *)); // yes a pointer
        if (vp)
            vp->implp = psp;
        else
            free(psp);
    }
    if ((NULL == vp) && vb)
        pr2ws("%s: about to return NULL, space problem\n", __func__);
    return vp;
}

struct sg_pt_base *
construct_scsi_pt_obj(void)
{
    return construct_scsi_pt_obj_with_fd(-1, 0);
}

void
destruct_scsi_pt_obj(struct sg_pt_base * vp)
{
    if (vp) {
        struct sg_pt_win32_scsi * psp = vp->implp;

        if (psp) {
            free(psp);
        }
        free(vp);
    }
}

/* Keep state information such as dev_fd and nvme_nsid */
void
clear_scsi_pt_obj(struct sg_pt_base * vp)
{
    bool is_nvme;
    int dev_fd;
    uint32_t nvme_nsid;
    struct sg_pt_win32_scsi * psp = vp->implp;

    if (psp) {
        dev_fd = psp->dev_fd;
        is_nvme = psp->is_nvme;
        nvme_nsid = psp->nvme_nsid;
        memset(psp, 0, sizeof(struct sg_pt_win32_scsi));
        if (spt_direct) {
            psp->swb_d.spt.DataIn = SCSI_IOCTL_DATA_UNSPECIFIED;
            psp->swb_d.spt.SenseInfoLength = SCSI_MAX_SENSE_LEN;
            psp->swb_d.spt.SenseInfoOffset =
                offsetof(SCSI_PASS_THROUGH_WITH_BUFFERS, ucSenseBuf);
            psp->swb_d.spt.TimeOutValue = DEF_TIMEOUT;
        } else {
            psp->swb_i.spt.DataIn = SCSI_IOCTL_DATA_UNSPECIFIED;
            psp->swb_i.spt.SenseInfoLength = SCSI_MAX_SENSE_LEN;
            psp->swb_i.spt.SenseInfoOffset =
                offsetof(SCSI_PASS_THROUGH_WITH_BUFFERS, ucSenseBuf);
            psp->swb_i.spt.TimeOutValue = DEF_TIMEOUT;
        }
        psp->dev_fd = dev_fd;
        psp->is_nvme = is_nvme;
        psp->nvme_nsid = nvme_nsid;
    }
}

void
set_scsi_pt_cdb(struct sg_pt_base * vp, const unsigned char * cdb,
                int cdb_len)
{
    bool scsi_cdb = sg_is_scsi_cdb(cdb, cdb_len);
    struct sg_pt_win32_scsi * psp = vp->implp;

    if (! scsi_cdb) {
        if (psp->have_nvme_cmd)
            ++psp->in_err;
        else
            psp->have_nvme_cmd = true;
        memcpy(psp->nvme_cmd, cdb, cdb_len);
    } else if (spt_direct) {
        if (psp->swb_d.spt.CdbLength > 0)
            ++psp->in_err;
        if (cdb_len > (int)sizeof(psp->swb_d.spt.Cdb)) {
            ++psp->in_err;
            return;
        }
        memcpy(psp->swb_d.spt.Cdb, cdb, cdb_len);
        psp->swb_d.spt.CdbLength = cdb_len;
    } else {
        if (psp->swb_i.spt.CdbLength > 0)
            ++psp->in_err;
        if (cdb_len > (int)sizeof(psp->swb_i.spt.Cdb)) {
            ++psp->in_err;
            return;
        }
        memcpy(psp->swb_i.spt.Cdb, cdb, cdb_len);
        psp->swb_i.spt.CdbLength = cdb_len;
    }
}

void
set_scsi_pt_sense(struct sg_pt_base * vp, unsigned char * sense,
                  int sense_len)
{
    struct sg_pt_win32_scsi * psp = vp->implp;

    if (psp->sensep)
        ++psp->in_err;
    memset(sense, 0, sense_len);
    psp->sensep = sense;
    psp->sense_len = sense_len;
}

/* from device */
void
set_scsi_pt_data_in(struct sg_pt_base * vp, unsigned char * dxferp,
                    int dxfer_len)
{
    struct sg_pt_win32_scsi * psp = vp->implp;

    if (psp->dxferp)
        ++psp->in_err;
    if (dxfer_len > 0) {
        psp->dxferp = dxferp;
        psp->dxfer_len = (uint32_t)dxfer_len;
        psp->is_read = true;
        if (spt_direct)
            psp->swb_d.spt.DataIn = SCSI_IOCTL_DATA_IN;
        else
            psp->swb_i.spt.DataIn = SCSI_IOCTL_DATA_IN;
    }
}

/* to device */
void
set_scsi_pt_data_out(struct sg_pt_base * vp, const unsigned char * dxferp,
                     int dxfer_len)
{
    struct sg_pt_win32_scsi * psp = vp->implp;

    if (psp->dxferp)
        ++psp->in_err;
    if (dxfer_len > 0) {
        psp->dxferp = (unsigned char *)dxferp;
        psp->dxfer_len = (uint32_t)dxfer_len;
        if (spt_direct)
            psp->swb_d.spt.DataIn = SCSI_IOCTL_DATA_OUT;
        else
            psp->swb_i.spt.DataIn = SCSI_IOCTL_DATA_OUT;
    }
}

void
set_pt_metadata_xfer(struct sg_pt_base * vp, unsigned char * mdxferp,
                     uint32_t mdxfer_len, bool out_true)
{
    struct sg_pt_win32_scsi * psp = vp->implp;

    if (psp->mdxferp)
        ++psp->in_err;
    if (mdxfer_len > 0) {
        psp->mdxferp = mdxferp;
        psp->mdxfer_len = mdxfer_len;
        psp->mdxfer_out = out_true;
    }
}

void
set_scsi_pt_packet_id(struct sg_pt_base * vp __attribute__ ((unused)),
                      int pack_id __attribute__ ((unused)))
{
}

void
set_scsi_pt_tag(struct sg_pt_base * vp, uint64_t tag __attribute__ ((unused)))
{
    struct sg_pt_win32_scsi * psp = vp->implp;

    ++psp->in_err;
}

void
set_scsi_pt_task_management(struct sg_pt_base * vp,
                            int tmf_code __attribute__ ((unused)))
{
    struct sg_pt_win32_scsi * psp = vp->implp;

    ++psp->in_err;
}

void
set_scsi_pt_task_attr(struct sg_pt_base * vp,
                      int attrib __attribute__ ((unused)),
                      int priority __attribute__ ((unused)))
{
    struct sg_pt_win32_scsi * psp = vp->implp;

    ++psp->in_err;
}

void
set_scsi_pt_flags(struct sg_pt_base * objp, int flags)
{
    /* do nothing, suppress warnings */
    objp = objp;
    flags = flags;
}

/* Executes SCSI command (or at least forwards it to lower layers)
 * using direct interface. Clears os_err field prior to active call (whose
 * result may set it again). */
static int
do_scsi_pt_direct(struct sg_pt_win32_scsi * psp, struct sg_pt_handle * shp,
                  int time_secs, int vb)
{
    BOOL status;
    DWORD returned;

    psp->os_err = 0;
    if (0 == psp->swb_d.spt.CdbLength) {
        if (vb)
            pr2ws("No command (cdb) given\n");
        return SCSI_PT_DO_BAD_PARAMS;
    }
    psp->swb_d.spt.Length = sizeof (SCSI_PASS_THROUGH_DIRECT);
    psp->swb_d.spt.PathId = shp->bus;
    psp->swb_d.spt.TargetId = shp->target;
    psp->swb_d.spt.Lun = shp->lun;
    psp->swb_d.spt.TimeOutValue = time_secs;
    psp->swb_d.spt.DataTransferLength = psp->dxfer_len;
    if (vb > 4) {
        pr2ws(" spt_direct, adapter: %s  Length=%d ScsiStatus=%d PathId=%d "
              "TargetId=%d Lun=%d\n", shp->adapter,
              (int)psp->swb_d.spt.Length, (int)psp->swb_d.spt.ScsiStatus,
              (int)psp->swb_d.spt.PathId, (int)psp->swb_d.spt.TargetId,
              (int)psp->swb_d.spt.Lun);
        pr2ws("    CdbLength=%d SenseInfoLength=%d DataIn=%d "
              "DataTransferLength=%u\n",
              (int)psp->swb_d.spt.CdbLength,
              (int)psp->swb_d.spt.SenseInfoLength,
              (int)psp->swb_d.spt.DataIn,
              (unsigned int)psp->swb_d.spt.DataTransferLength);
        pr2ws("    TimeOutValue=%u SenseInfoOffset=%u\n",
              (unsigned int)psp->swb_d.spt.TimeOutValue,
              (unsigned int)psp->swb_d.spt.SenseInfoOffset);
    }
    psp->swb_d.spt.DataBuffer = psp->dxferp;
    status = DeviceIoControl(shp->fh, IOCTL_SCSI_PASS_THROUGH_DIRECT,
                            &psp->swb_d,
                            sizeof(psp->swb_d),
                            &psp->swb_d,
                            sizeof(psp->swb_d),
                            &returned,
                            NULL);
    if (! status) {
        unsigned int u;

        u = (unsigned int)GetLastError();
        if (vb) {
            char b[128];

            pr2ws("%s: DeviceIoControl: %s [%u]\n", __func__,
                  get_err_str(u, sizeof(b), b), u);
        }
        psp->transport_err = (int)u;
        psp->os_err = EIO;
        return 0;       /* let app find transport error */
    }

    psp->scsi_status = psp->swb_d.spt.ScsiStatus;
    if ((SAM_STAT_CHECK_CONDITION == psp->scsi_status) ||
        (SAM_STAT_COMMAND_TERMINATED == psp->scsi_status))
        memcpy(psp->sensep, psp->swb_d.ucSenseBuf, psp->sense_len);
    else
        psp->sense_len = 0;
    psp->sense_resid = 0;
    if ((psp->dxfer_len > 0) && (psp->swb_d.spt.DataTransferLength > 0))
        psp->resid = psp->dxfer_len - psp->swb_d.spt.DataTransferLength;
    else
        psp->resid = 0;

    return 0;
}

/* Executes SCSI command (or at least forwards it to lower layers) using
 * indirect interface. Clears os_err field prior to active call (whose
 * result may set it again). */
static int
do_scsi_pt_indirect(struct sg_pt_base * vp, struct sg_pt_handle * shp,
                    int time_secs, int vb)
{
    BOOL status;
    DWORD returned;
    struct sg_pt_win32_scsi * psp = vp->implp;

    if (0 == psp->swb_i.spt.CdbLength) {
        if (vb)
            pr2ws("No command (cdb) given\n");
        return SCSI_PT_DO_BAD_PARAMS;
    }
    if (psp->dxfer_len > (int)sizeof(psp->swb_i.ucDataBuf)) {
        int extra = psp->dxfer_len - (int)sizeof(psp->swb_i.ucDataBuf);
        struct sg_pt_win32_scsi * epsp;

        if (vb > 4)
            pr2ws("spt_indirect: dxfer_len (%d) too large for initial data\n"
                  "  buffer (%d bytes), try enlarging\n", psp->dxfer_len,
                  (int)sizeof(psp->swb_i.ucDataBuf));
        epsp = (struct sg_pt_win32_scsi *)
               calloc(sizeof(struct sg_pt_win32_scsi) + extra, 1);
        if (NULL == epsp) {
            pr2ws("do_scsi_pt: failed to enlarge data buffer to %d bytes\n",
                  psp->dxfer_len);
            psp->os_err = ENOMEM;
            return -psp->os_err;
        }
        memcpy(epsp, psp, sizeof(struct sg_pt_win32_scsi));
        free(psp);
        vp->implp = epsp;
        psp = epsp;
    }
    psp->swb_i.spt.Length = sizeof (SCSI_PASS_THROUGH);
    psp->swb_i.spt.DataBufferOffset =
                offsetof(SCSI_PASS_THROUGH_WITH_BUFFERS, ucDataBuf);
    psp->swb_i.spt.PathId = shp->bus;
    psp->swb_i.spt.TargetId = shp->target;
    psp->swb_i.spt.Lun = shp->lun;
    psp->swb_i.spt.TimeOutValue = time_secs;
    psp->swb_i.spt.DataTransferLength = psp->dxfer_len;
    if (vb > 4) {
        pr2ws(" spt_indirect, adapter: %s  Length=%d ScsiStatus=%d PathId=%d "
              "TargetId=%d Lun=%d\n", shp->adapter,
              (int)psp->swb_i.spt.Length, (int)psp->swb_i.spt.ScsiStatus,
              (int)psp->swb_i.spt.PathId, (int)psp->swb_i.spt.TargetId,
              (int)psp->swb_i.spt.Lun);
        pr2ws("    CdbLength=%d SenseInfoLength=%d DataIn=%d "
              "DataTransferLength=%u\n",
              (int)psp->swb_i.spt.CdbLength,
              (int)psp->swb_i.spt.SenseInfoLength,
              (int)psp->swb_i.spt.DataIn,
              (unsigned int)psp->swb_i.spt.DataTransferLength);
        pr2ws("    TimeOutValue=%u DataBufferOffset=%u "
              "SenseInfoOffset=%u\n",
              (unsigned int)psp->swb_i.spt.TimeOutValue,
              (unsigned int)psp->swb_i.spt.DataBufferOffset,
              (unsigned int)psp->swb_i.spt.SenseInfoOffset);
    }
    if ((psp->dxfer_len > 0) &&
        (SCSI_IOCTL_DATA_OUT == psp->swb_i.spt.DataIn))
        memcpy(psp->swb_i.ucDataBuf, psp->dxferp, psp->dxfer_len);
    status = DeviceIoControl(shp->fh, IOCTL_SCSI_PASS_THROUGH,
                            &psp->swb_i,
                            sizeof(psp->swb_i),
                            &psp->swb_i,
                            sizeof(psp->swb_i),
                            &returned,
                            NULL);
    if (! status) {
        uint32_t u = (uint32_t)GetLastError();

        if (vb) {
            char b[128];

            pr2ws("%s: DeviceIoControl: %s [%u]\n", __func__,
                  get_err_str(u, sizeof(b), b), u);
        }
        psp->transport_err = (int)u;
        psp->os_err = EIO;
        return 0;       /* let app find transport error */
    }
    if ((psp->dxfer_len > 0) && (SCSI_IOCTL_DATA_IN == psp->swb_i.spt.DataIn))
        memcpy(psp->dxferp, psp->swb_i.ucDataBuf, psp->dxfer_len);

    psp->scsi_status = psp->swb_i.spt.ScsiStatus;
    if ((SAM_STAT_CHECK_CONDITION == psp->scsi_status) ||
        (SAM_STAT_COMMAND_TERMINATED == psp->scsi_status))
        memcpy(psp->sensep, psp->swb_i.ucSenseBuf, psp->sense_len);
    else
        psp->sense_len = 0;
    psp->sense_resid = 0;
    if ((psp->dxfer_len > 0) && (psp->swb_i.spt.DataTransferLength > 0))
        psp->resid = psp->dxfer_len - psp->swb_i.spt.DataTransferLength;
    else
        psp->resid = 0;

    return 0;
}

/* Executes SCSI or NVME command (or at least forwards it to lower layers).
 * Clears os_err field prior to active call (whose result may set it
 * again). Returns 0 on success, positive SCSI_PT_DO_* errors for syntax
 * like errors and negated errnos for OS errors. For Windows its errors
 * are placed in psp->transport_err and a errno is simulated. */
int
do_scsi_pt(struct sg_pt_base * vp, int dev_fd, int time_secs, int vb)
{
    int res;
    struct sg_pt_win32_scsi * psp = vp->implp;
    struct sg_pt_handle * shp;

    if (! (vp && ((psp = vp->implp)))) {
        if (vb)
            pr2ws("%s: NULL 1st argument to this function\n", __func__);
        return SCSI_PT_DO_BAD_PARAMS;
    }
    psp->os_err = 0;
    if (dev_fd >= 0) {
        if ((psp->dev_fd >= 0) && (dev_fd != psp->dev_fd)) {
            if (vb)
                pr2ws("%s: file descriptor given to create() and here "
                      "differ\n", __func__);
            return SCSI_PT_DO_BAD_PARAMS;
        }
        psp->dev_fd = dev_fd;
    } else if (psp->dev_fd < 0) {       /* so no dev_fd in ctor */
        if (vb)
            pr2ws("%s: missing device file descriptor\n", __func__);
        return SCSI_PT_DO_BAD_PARAMS;
    } else
        dev_fd = psp->dev_fd;
    shp = get_open_pt_handle(psp, dev_fd, vb > 3);
    if (NULL == shp)
        return -psp->os_err;

    if (! (shp->bus_type_failed || shp->checked_handle)) {
        res = get_bus_type(shp, shp->dname, NULL, vb);
        if (res < 0) {
            if (vb)
                pr2ws("%s: get_bus_type() errno=%d\n", __func__, -res);
        }
    }
    if (shp->bus_type_failed)
        psp->os_err = EIO;
    if (psp->os_err)
        return -psp->os_err;
    psp->is_nvme = shp->is_nvme;

    if (psp->is_nvme)
        return do_nvme_pt(psp, shp, time_secs, vb);
    else if (spt_direct)
        return do_scsi_pt_direct(psp, shp, time_secs, vb);
    else
        return do_scsi_pt_indirect(vp, shp, time_secs, vb);
}

int
get_scsi_pt_result_category(const struct sg_pt_base * vp)
{
    const struct sg_pt_win32_scsi * psp = vp->implp;

    if (psp->transport_err)     /* give transport error highest priority */
        return SCSI_PT_RESULT_TRANSPORT_ERR;
    else if (psp->os_err)
        return SCSI_PT_RESULT_OS_ERR;
    else if ((SAM_STAT_CHECK_CONDITION == psp->scsi_status) ||
             (SAM_STAT_COMMAND_TERMINATED == psp->scsi_status))
        return SCSI_PT_RESULT_SENSE;
    else if (psp->scsi_status)
        return SCSI_PT_RESULT_STATUS;
    else
        return SCSI_PT_RESULT_GOOD;
}

int
get_scsi_pt_resid(const struct sg_pt_base * vp)
{
    const struct sg_pt_win32_scsi * psp = vp->implp;

    return psp->resid;
}

int
get_scsi_pt_status_response(const struct sg_pt_base * vp)
{
    const struct sg_pt_win32_scsi * psp = vp->implp;

    if (NULL == psp)
        return 0;
    return psp->nvme_direct ? (int)psp->nvme_status : psp->scsi_status;
}

uint32_t
get_pt_result(const struct sg_pt_base * vp)
{
    const struct sg_pt_win32_scsi * psp = vp->implp;

    if (NULL == psp)
        return 0;
    return psp->nvme_direct ? psp->nvme_result : (uint32_t)psp->scsi_status;
}

int
get_scsi_pt_sense_len(const struct sg_pt_base * vp)
{
    const struct sg_pt_win32_scsi * psp = vp->implp;
    int len;

    len = psp->sense_len - psp->sense_resid;
    return (len > 0) ? len : 0;
}

int
get_scsi_pt_duration_ms(const struct sg_pt_base * vp __attribute__ ((unused)))
{
    // const struct sg_pt_freebsd_scsi * psp = vp->implp;

    return -1;
}

int
get_scsi_pt_transport_err(const struct sg_pt_base * vp)
{
    const struct sg_pt_win32_scsi * psp = vp->implp;

    return psp->transport_err;
}

void
set_scsi_pt_transport_err(struct sg_pt_base * vp, int err)
{
    struct sg_pt_win32_scsi * psp = vp->implp;

    psp->transport_err = err;
}

int
get_scsi_pt_os_err(const struct sg_pt_base * vp)
{
    const struct sg_pt_win32_scsi * psp = vp->implp;

    return psp->os_err;
}

bool
pt_device_is_nvme(const struct sg_pt_base * vp)
{
    const struct sg_pt_win32_scsi * psp = vp->implp;

    return psp ? psp->is_nvme : false;
}

/* If a NVMe block device (which includes the NSID) handle is associated
 *  * with 'vp', then its NSID is returned (values range from 0x1 to
 *   * 0xffffffe). Otherwise 0 is returned. */
uint32_t
get_pt_nvme_nsid(const struct sg_pt_base * vp)
{
    const struct sg_pt_win32_scsi * psp = vp->implp;

    return psp->nvme_nsid;
}

/* Use the transport_err for Windows errors. */
char *
get_scsi_pt_transport_err_str(const struct sg_pt_base * vp, int max_b_len,
                              char * b)
{
    struct sg_pt_win32_scsi * psp = (struct sg_pt_win32_scsi *)vp->implp;

    if ((max_b_len < 2) || (NULL == psp) || (NULL == b)) {
        if (b && (max_b_len > 0))
            b[0] = '\0';
        return b;
    }
    return get_err_str(psp->transport_err, max_b_len, b);
}

char *
get_scsi_pt_os_err_str(const struct sg_pt_base * vp, int max_b_len, char * b)
{
    const struct sg_pt_win32_scsi * psp = vp->implp;
    const char * cp;

    cp = safe_strerror(psp->os_err);
    strncpy(b, cp, max_b_len);
    if ((int)strlen(cp) >= max_b_len)
        b[max_b_len - 1] = '\0';
    return b;
}

#if (HAVE_NVME && (! IGNORE_NVME))


static void
build_sense_buffer(bool desc, uint8_t *buf, uint8_t skey, uint8_t asc,
                   uint8_t ascq)
{
    if (desc) {
        buf[0] = 0x72;  /* descriptor, current */
        buf[1] = skey;
        buf[2] = asc;
        buf[3] = ascq;
        buf[7] = 0;
    } else {
        buf[0] = 0x70;  /* fixed, current */
        buf[2] = skey;
        buf[7] = 0xa;   /* Assumes length is 18 bytes */
        buf[12] = asc;
        buf[13] = ascq;
    }
}

/* Set in_bit to -1 to indicate no bit position of invalid field */
static void
mk_sense_asc_ascq(struct sg_pt_win32_scsi * psp, int sk, int asc, int ascq,
                  int vb)
{
    bool dsense = psp->scsi_dsense;
    int slen = psp->sense_len;
    int n;
    uint8_t * sbp = (uint8_t *)psp->sensep;

    psp->scsi_status = SAM_STAT_CHECK_CONDITION;
    if ((slen < 8) || ((! dsense) && (slen < 14))) {
        if (vb)
            pr2ws("%s: sense_len=%d too short, want 14 or more\n",
                  __func__, slen);
        return;
    }
    n = dsense ? 8 : ((slen < 18) ? slen : 18);
    psp->sense_resid = (slen > n) ? (slen - n) : 0;
    memset(sbp, 0, slen);
    build_sense_buffer(dsense, sbp, sk, asc, ascq);
    if (vb > 3)
        pr2ws("%s:  [sense_key,asc,ascq]: [0x%x,0x%x,0x%x]\n", __func__, sk,
              asc, ascq);
}

static void
mk_sense_from_nvme_status(struct sg_pt_win32_scsi * psp, int vb)
{
    bool ok;
    bool dsense = psp->scsi_dsense;
    int n;
    int slen = psp->sense_len;
    uint8_t sstatus, sk, asc, ascq;
    uint8_t * sbp = (uint8_t *)psp->sensep;

    ok = sg_nvme_status2scsi(psp->nvme_status, &sstatus, &sk, &asc, &ascq);
    if (! ok) { /* can't find a mapping to a SCSI error, so ... */
        sstatus = SAM_STAT_CHECK_CONDITION;
        sk = SPC_SK_ILLEGAL_REQUEST;
        asc = 0xb;
        ascq = 0x0;     /* asc: "WARNING" purposely vague */
    }

    psp->scsi_status = sstatus;
    if ((slen < 8) || ((! dsense) && (slen < 14))) {
        if (vb)
            pr2ws("%s: sense_len=%d too short, want 14 or more\n", __func__,
                  slen);
        return;
    }
    n = (dsense ? 8 : ((slen < 18) ? slen : 18));
    psp->sense_resid = (slen > n) ? slen - n : 0;
    memset(sbp, 0, slen);
    build_sense_buffer(dsense, sbp, sk, asc, ascq);
    if (vb > 3)
        pr2ws("%s: [status, sense_key,asc,ascq]: [0x%x, 0x%x,0x%x,0x%x]\n",
              __func__, sstatus, sk, asc, ascq);
}

/* Set in_bit to -1 to indicate no bit position of invalid field */
static void
mk_sense_invalid_fld(struct sg_pt_win32_scsi * psp, bool in_cdb, int in_byte,
                     int in_bit, int vb)
{
    bool dsense = psp->scsi_dsense;
    int sl, asc, n;
    int slen = psp->sense_len;
    uint8_t * sbp = (uint8_t *)psp->sensep;
    uint8_t sks[4];

    psp->scsi_status = SAM_STAT_CHECK_CONDITION;
    asc = in_cdb ? INVALID_FIELD_IN_CDB : INVALID_FIELD_IN_PARAM_LIST;
    if ((slen < 8) || ((! dsense) && (slen < 14))) {
        if (vb)
            pr2ws("%s: max_response_len=%d too short, want 14 or more\n",
                  __func__, slen);
        return;
    }
    n = dsense ? 8 : ((slen < 18) ? slen : 18);
    psp->sense_resid = (slen > n) ? (slen - n) : 0;
    memset(sbp, 0, slen);
    build_sense_buffer(dsense, sbp, SPC_SK_ILLEGAL_REQUEST, asc, 0);
    memset(sks, 0, sizeof(sks));
    sks[0] = 0x80;
    if (in_cdb)
        sks[0] |= 0x40;
    if (in_bit >= 0) {
        sks[0] |= 0x8;
        sks[0] |= (0x7 & in_bit);
    }
    sg_put_unaligned_be16(in_byte, sks + 1);
    if (dsense) {
        sl = sbp[7] + 8;
        sbp[7] = sl;
        sbp[sl] = 0x2;
        sbp[sl + 1] = 0x6;
        memcpy(sbp + sl + 4, sks, 3);
    } else
        memcpy(sbp + 15, sks, 3);
    if (vb > 3)
        pr2ws("%s:  [sense_key,asc,ascq]: [0x5,0x%x,0x0] %c byte=%d, bit=%d\n",
              __func__, asc, in_cdb ? 'C' : 'D', in_byte, in_bit);
}

/* If cmdp is NULL then dp, dlen and is_read are ignored, those values are
 * obtained from psp. Returns 0 for success. Returns SG_LIB_NVME_STATUS if
 * there is non-zero NVMe status (SCT|SC from the completion queue) with the
 * value placed in psp->nvme_status. If Unix error from ioctl then return
 * negated value (equivalent -errno from basic Unix system functions like
 * open()). CDW0 from the completion queue is placed in psp->nvme_result in
 * the absence of an error.
 * The following code is based on os_win32.cpp in smartmontools:
 *           Copyright (C) 2004-17 Christian Franke
 * The code is licensed with a GPL-2. */
static int
do_nvme_admin_cmd(struct sg_pt_win32_scsi * psp, struct sg_pt_handle * shp,
                  const uint8_t * cmdp, uint8_t * dp, uint32_t dlen,
                  bool is_read, int time_secs, int vb)
{
    const uint32_t cmd_len = 64;
    int res;
    uint32_t n, alloc_len;
    uint32_t pg_sz = sg_get_page_size();
    uint32_t slen = psp->sense_len;
    uint8_t * sbp = psp->sensep;
    NVME_PASS_THROUGH_IOCTL * pthru;
    uint8_t * free_pthru;
    DWORD num_out = 0;
    BOOL ok;

    psp->os_err = 0;
    psp->transport_err = 0;
    if (NULL == cmdp) {
        if (! psp->have_nvme_cmd)
            return SCSI_PT_DO_BAD_PARAMS;
        cmdp = psp->nvme_cmd;
        is_read = psp->is_read;
        dlen = psp->dxfer_len;
        dp = psp->dxferp;
    }
    if (vb > 2) {
        pr2ws("NVMe command:\n");
        hex2stderr((const uint8_t *)cmdp, cmd_len, 1);
        if ((vb > 3) && (! is_read) && dp) {
            if (dlen > 0) {
                n = dlen;
                if ((dlen < 512) || (vb > 5))
                    pr2ws("\nData-out buffer (%u bytes):\n", n);
                else {
                    pr2ws("\nData-out buffer (first 512 of %u bytes):\n", n);
                    n = 512;
                }
                hex2stderr((const uint8_t *)dp, n, 0);
            }
        }
    }
    alloc_len = sizeof(NVME_PASS_THROUGH_IOCTL) + dlen;
    pthru = (NVME_PASS_THROUGH_IOCTL *)sg_memalign(alloc_len, pg_sz,
                                                   &free_pthru, vb);
    if (NULL == pthru) {
        res = SG_LIB_OS_BASE_ERR + ENOMEM;
        if (vb > 1)
            pr2ws("%s: unable to allocate memory\n", __func__);
        psp->os_err = res;
        return -res;
    }
    if (dp && (dlen > 0) && (! is_read))
        memcpy(pthru->DataBuffer, dp, dlen);    /* dout-out buffer */
    /* Set NVMe command */
    pthru->SrbIoCtrl.HeaderLength = sizeof(SRB_IO_CONTROL);
    memcpy(pthru->SrbIoCtrl.Signature, NVME_SIG_STR, sizeof(NVME_SIG_STR)-1);
    pthru->SrbIoCtrl.Timeout = (time_secs > 0) ? time_secs : DEF_TIMEOUT;
    pthru->SrbIoCtrl.ControlCode = NVME_PASS_THROUGH_SRB_IO_CODE;
    pthru->SrbIoCtrl.ReturnCode = 0;
    pthru->SrbIoCtrl.Length = alloc_len - sizeof(SRB_IO_CONTROL);

    memcpy(pthru->NVMeCmd, cmdp, cmd_len);
    if (dlen > 0)
        pthru->Direction = is_read ? 2 : 1;
    else
        pthru->Direction = 0;
    pthru->ReturnBufferLen = alloc_len;
    shp = get_open_pt_handle(psp, psp->dev_fd, vb > 1);
    if (NULL == shp) {
        res = -psp->os_err;     /* -ENODEV */
        goto err_out;
    }

    ok = DeviceIoControl(shp->fh, IOCTL_SCSI_MINIPORT, pthru, alloc_len,
                         pthru, alloc_len, &num_out, (OVERLAPPED*)0);
    if (! ok) {
        n = (uint32_t)GetLastError();
        psp->transport_err = n;
        psp->os_err = EIO;      /* simulate Unix error,  */
        if (vb > 2) {
            char b[128];

            pr2ws("%s: IOCTL_SCSI_MINIPORT failed: %s [%u]\n", __func__,
                  get_err_str(n, sizeof(b), b), n);
pr2ws("handle=%u, alloc_len=%u, num_out=%u\n", shp->fh, alloc_len, (uint32_t)num_out);
        }
    }
    /* nvme_status is SCT|SC, therefor it excludes DNR+More */
    psp->nvme_status = 0x3ff & (pthru->CplEntry[3] >> 17);
    if (psp->nvme_status && (vb > 1)) {
        uint16_t s = psp->nvme_status;
        char b[80];

        pr2ws("%s: opcode=0x%x failed: NVMe status: %s [0x%x]\n", __func__,
              cmdp[0], sg_get_nvme_cmd_status_str(s, sizeof(b), b), s);
    }
    psp->nvme_result = sg_get_unaligned_le32(pthru->CplEntry + 0);

    psp->sense_resid = 0;
    if (psp->nvme_direct && sbp && (slen > 3)) {
        /* build 16 byte "sense" buffer */
        n = (slen < 16) ? slen : 16;
        memset(sbp, 0 , n);
        psp->sense_resid = (slen > 16) ? (slen - 16) : 0;
        sg_put_unaligned_le32(pthru->CplEntry[0], sbp + SG_NVME_PT_CQ_DW0);
        if (n > 7) {
            sg_put_unaligned_le32(pthru->CplEntry[1],
                                  sbp + SG_NVME_PT_CQ_DW1);
            if (n > 11) {
                sg_put_unaligned_le32(pthru->CplEntry[2],
                                      sbp + SG_NVME_PT_CQ_DW2);
                if (n > 15)
                    sg_put_unaligned_le32(pthru->CplEntry[3],
                                          sbp + SG_NVME_PT_CQ_DW3);
            }
        }
    }
    if (! ok) {
        res = -psp->os_err;
        goto err_out;
    } else if (psp->nvme_status) {
        res = SG_LIB_NVME_STATUS;
        goto err_out;
    }

    if (dp && (dlen > 0) && is_read) {
        memcpy(dp, pthru->DataBuffer, dlen);    /* data-in buffer */
        if (vb > 3) {
            n = dlen;
            if ((dlen < 1024) || (vb > 5))
                pr2ws("\nData-in buffer (%u bytes):\n", n);
            else {
                pr2ws("\nData-in buffer (first 1024 of %u bytes):\n", n);
                n = 1024;
            }
            hex2stderr((const uint8_t *)dp, n, 0);
        }
    }
    res = 0;
err_out:
    if (free_pthru)
        free(free_pthru);
    return res;
}


/* Returns 0 on success; otherwise a positive value is returned */
static int
sntl_cache_identity(struct sg_pt_win32_scsi * psp, struct sg_pt_handle * shp,
                    int time_secs, int vb)
{
    static bool is_read = true;
    uint32_t pg_sz = sg_get_page_size();
    uint8_t * up;
    uint8_t * cmdp;

    up = sg_memalign(pg_sz, pg_sz, &psp->free_nvme_id_ctlp, vb > 3);
    psp->nvme_id_ctlp = up;
    if (NULL == up) {
        pr2ws("%s: sg_memalign() failed to get memory\n", __func__);
        return -ENOMEM;
    }
    cmdp = psp->nvme_cmd;
    memset(cmdp, 0, sizeof(psp->nvme_cmd));
    cmdp[0] =  0x6;   /* Identify */
    /* leave nsid as 0, should it be broadcast (0xffffffff) ? */
    /* CNS=0x1 Identify controller: */
    sg_put_unaligned_le32(0x1, cmdp + SG_NVME_PT_CDW10);
    sg_put_unaligned_le64((uint64_t)(sg_uintptr_t)up, cmdp + SG_NVME_PT_ADDR);
    sg_put_unaligned_le32(pg_sz, cmdp + SG_NVME_PT_DATA_LEN);
    return do_nvme_admin_cmd(psp, shp, cmdp, up, pg_sz, is_read, time_secs,
                             vb);
}


static const char * nvme_scsi_vendor_str = "NVMe    ";
static const uint16_t inq_resp_len = 36;

static int
sntl_inq(struct sg_pt_win32_scsi * psp, struct sg_pt_handle * shp,
         const uint8_t * cdbp, int time_secs, int vb)
{
    bool evpd;
    bool cp_id_ctl = false;
    int res;
    uint16_t n, alloc_len, pg_cd;
    uint32_t pg_sz = sg_get_page_size();
    uint8_t * nvme_id_ns = NULL;
    uint8_t * free_nvme_id_ns = NULL;
    uint8_t inq_dout[256];
    uint8_t * cmdp;

    if (vb > 3)
        pr2ws("%s: time_secs=%d\n", __func__, time_secs);
    if (0x2 & cdbp[1]) {        /* Reject CmdDt=1 */
        mk_sense_invalid_fld(psp, true, 1, 1, vb);
        return 0;
    }
    if (NULL == psp->nvme_id_ctlp) {
        res = sntl_cache_identity(psp, shp, time_secs, vb);
        if (SG_LIB_NVME_STATUS == res) {
            mk_sense_from_nvme_status(psp, vb);
            return 0;
        } else if (res) /* should be negative errno */
            return res;
    }
    memset(inq_dout, 0, sizeof(inq_dout));
    alloc_len = sg_get_unaligned_be16(cdbp + 3);
    evpd = !!(0x1 & cdbp[1]);
    pg_cd = cdbp[2];
    if (evpd) {         /* VPD page responses */
        switch (pg_cd) {
        case 0:
            /* inq_dout[0] = (PQ=0)<<5 | (PDT=0); prefer pdt=0xd --> SES */
            inq_dout[1] = pg_cd;
            n = 8;
            sg_put_unaligned_be16(n - 4, inq_dout + 2);
            inq_dout[4] = 0x0;
            inq_dout[5] = 0x80;
            inq_dout[6] = 0x83;
            inq_dout[n - 1] = 0xde;     /* last VPD number */
            break;
        case 0x80:
            /* inq_dout[0] = (PQ=0)<<5 | (PDT=0); prefer pdt=0xd --> SES */
            inq_dout[1] = pg_cd;
            sg_put_unaligned_be16(20, inq_dout + 2);
            memcpy(inq_dout + 4, psp->nvme_id_ctlp + 4, 20);    /* SN */
            n = 24;
            break;
        case 0x83:
            if ((psp->nvme_nsid > 0) &&
                (psp->nvme_nsid < SG_NVME_BROADCAST_NSID)) {
                nvme_id_ns = sg_memalign(pg_sz, pg_sz, &free_nvme_id_ns,
                                         vb > 3);
                if (nvme_id_ns) {
                    cmdp = psp->nvme_cmd;
                    memset(cmdp, 0, sizeof(psp->nvme_cmd));
                    cmdp[SG_NVME_PT_OPCODE] =  0x6;   /* Identify */
                    sg_put_unaligned_le32(psp->nvme_nsid,
                                          cmdp + SG_NVME_PT_NSID);
                    /* CNS=0x0 Identify controller: */
                    sg_put_unaligned_le32(0x0, cmdp + SG_NVME_PT_CDW10);
                    sg_put_unaligned_le64((uint64_t)(sg_uintptr_t)nvme_id_ns,
                                          cmdp + SG_NVME_PT_ADDR);
                    sg_put_unaligned_le32(pg_sz, cmdp + SG_NVME_PT_DATA_LEN);
                    res = do_nvme_admin_cmd(psp, shp, cmdp, nvme_id_ns, pg_sz,
                                            true, time_secs, vb > 3);
                    if (res) {
                        free(free_nvme_id_ns);
                        free_nvme_id_ns = NULL;
                        nvme_id_ns = NULL;
                    }
                }
            }
            n = sg_make_vpd_devid_for_nvme(psp->nvme_id_ctlp, nvme_id_ns,
                                           0 /* pdt */, -1 /*tproto */,
                                           inq_dout, sizeof(inq_dout));
            if (n > 3)
                sg_put_unaligned_be16(n - 4, inq_dout + 2);
            if (free_nvme_id_ns) {
                free(free_nvme_id_ns);
                free_nvme_id_ns = NULL;
                nvme_id_ns = NULL;
            }
            break;
        case 0xde:
            inq_dout[1] = pg_cd;
            sg_put_unaligned_be16((16 + 4096) - 4, inq_dout + 2);
            n = 16 + 4096;
            cp_id_ctl = true;
            break;
        default:        /* Point to page_code field in cdb */
            mk_sense_invalid_fld(psp, true, 2, 7, vb);
            return 0;
        }
        if (alloc_len > 0) {
            n = (alloc_len < n) ? alloc_len : n;
            n = (n < psp->dxfer_len) ? n : psp->dxfer_len;
            psp->resid = psp->dxfer_len - n;
            if (n > 0) {
                if (cp_id_ctl) {
                    memcpy(psp->dxferp, inq_dout, (n < 16 ? n : 16));
                    if (n > 16)
                        memcpy(psp->dxferp + 16,
                               psp->nvme_id_ctlp, n - 16);
                } else
                    memcpy(psp->dxferp, inq_dout, n);
            }
        }
    } else {            /* Standard INQUIRY response */
        /* inq_dout[0] = (PQ=0)<<5 | (PDT=0); pdt=0 --> SBC; 0xd --> SES */
        inq_dout[2] = 6;   /* version: SPC-4 */
        inq_dout[3] = 2;   /* NORMACA=0, HISUP=0, response data format: 2 */
        inq_dout[4] = 31;  /* so response length is (or could be) 36 bytes */
        inq_dout[6] = 0x40;   /* ENCSERV=1 */
        inq_dout[7] = 0x2;    /* CMDQUE=1 */
        memcpy(inq_dout + 8, nvme_scsi_vendor_str, 8);  /* NVMe not Intel */
        memcpy(inq_dout + 16, psp->nvme_id_ctlp + 24, 16); /* Prod <-- MN */
        memcpy(inq_dout + 32, psp->nvme_id_ctlp + 64, 4);  /* Rev <-- FR */
        if (alloc_len > 0) {
            n = (alloc_len < inq_resp_len) ? alloc_len : inq_resp_len;
            n = (n < psp->dxfer_len) ? n : psp->dxfer_len;
            psp->resid = psp->dxfer_len - n;
            if (n > 0)
                memcpy(psp->dxferp, inq_dout, n);
        }
    }
    return 0;
}

static int
sntl_rluns(struct sg_pt_win32_scsi * psp, struct sg_pt_handle * shp,
           const uint8_t * cdbp, int time_secs, int vb)
{
    int res;
    uint16_t sel_report;
    uint32_t alloc_len, k, n, num, max_nsid;
    uint8_t * rl_doutp;
    uint8_t * up;

    if (vb > 3)
        pr2ws("%s: time_secs=%d\n", __func__, time_secs);

    sel_report = cdbp[2];
    alloc_len = sg_get_unaligned_be32(cdbp + 6);
    if (NULL == psp->nvme_id_ctlp) {
        res = sntl_cache_identity(psp, shp, time_secs, vb);
        if (SG_LIB_NVME_STATUS == res) {
            mk_sense_from_nvme_status(psp, vb);
            return 0;
        } else if (res)
            return res;
    }
    max_nsid = sg_get_unaligned_le32(psp->nvme_id_ctlp + 516);
    switch (sel_report) {
    case 0:
    case 2:
        num = max_nsid;
        break;
    case 1:
    case 0x10:
    case 0x12:
        num = 0;
        break;
    case 0x11:
        num = (1 == psp->nvme_nsid) ? max_nsid :  0;
        break;
    default:
        if (vb > 1)
            pr2ws("%s: bad select_report value: 0x%x\n", __func__,
                  sel_report);
        mk_sense_invalid_fld(psp, true, 2, 7, vb);
        return 0;
    }
    rl_doutp = (uint8_t *)calloc(num + 1, 8);
    if (NULL == rl_doutp) {
        pr2ws("%s: calloc() failed to get memory\n", __func__);
        return -ENOMEM;
    }
    for (k = 0, up = rl_doutp + 8; k < num; ++k, up += 8)
        sg_put_unaligned_be16(k, up);
    n = num * 8;
    sg_put_unaligned_be32(n, rl_doutp);
    n+= 8;
    if (alloc_len > 0) {
        n = (alloc_len < n) ? alloc_len : n;
        n = (n < psp->dxfer_len) ? n : psp->dxfer_len;
        psp->resid = psp->dxfer_len - n;
        if (n > 0)
            memcpy(psp->dxferp, rl_doutp, n);
    }
    res = 0;
    free(rl_doutp);
    return res;
}

static int
sntl_tur(struct sg_pt_win32_scsi * psp, struct sg_pt_handle * shp,
         int time_secs, int vb)
{
    int res;
    uint32_t pow_state;
    uint8_t * cmdp;

    if (vb > 4)
        pr2ws("%s: enter\n", __func__);
    if (NULL == psp->nvme_id_ctlp) {
        res = sntl_cache_identity(psp, shp, time_secs, vb);
        if (SG_LIB_NVME_STATUS == res) {
            mk_sense_from_nvme_status(psp, vb);
            return 0;
        } else if (res)
            return res;
    }
    cmdp = psp->nvme_cmd;
    memset(cmdp, 0, sizeof(psp->nvme_cmd));
    cmdp[SG_NVME_PT_OPCODE] =  0xa; /* Get feature */
    sg_put_unaligned_le32(SG_NVME_BROADCAST_NSID, cmdp + SG_NVME_PT_NSID);
    /* SEL=0 (current), Feature=2 Power Management */
    sg_put_unaligned_le32(0x2, cmdp + SG_NVME_PT_CDW10);
    res = do_nvme_admin_cmd(psp, shp, cmdp, NULL, 0, false, time_secs, vb);
    if (0 != res) {
        if (SG_LIB_NVME_STATUS == res) {
            mk_sense_from_nvme_status(psp, vb);
            return 0;
        } else
            return res;
    } else {
        psp->os_err = 0;
        psp->nvme_status = 0;
    }
    pow_state = (0x1f & psp->nvme_result);
    if (vb > 3)
        pr2ws("%s: pow_state=%u\n", __func__, pow_state);
#if 0   /* pow_state bounces around too much on laptop */
    if (pow_state)
        mk_sense_asc_ascq(psp, SPC_SK_NOT_READY, LOW_POWER_COND_ON_ASC, 0,
                          vb);
#endif
    return 0;
}

static int
sntl_req_sense(struct sg_pt_win32_scsi * psp, struct sg_pt_handle * shp,
               const uint8_t * cdbp, int time_secs, int vb)
{
    bool desc;
    int res;
    uint32_t pow_state, alloc_len, n;
    uint8_t rs_dout[64];
    uint8_t * cmdp;

    if (vb > 3)
        pr2ws("%s: time_secs=%d\n", __func__, time_secs);
    if (NULL == psp->nvme_id_ctlp) {
        res = sntl_cache_identity(psp, shp, time_secs, vb);
        if (SG_LIB_NVME_STATUS == res) {
            mk_sense_from_nvme_status(psp, vb);
            return 0;
        } else if (res)
            return res;
    }
    desc = !!(0x1 & cdbp[1]);
    alloc_len = cdbp[4];
    cmdp = psp->nvme_cmd;
    memset(cmdp, 0, sizeof(psp->nvme_cmd));
    cmdp[SG_NVME_PT_OPCODE] =  0xa; /* Get feature */
    sg_put_unaligned_le32(SG_NVME_BROADCAST_NSID, cmdp + SG_NVME_PT_NSID);
    /* SEL=0 (current), Feature=2 Power Management */
    sg_put_unaligned_le32(0x2, cmdp + SG_NVME_PT_CDW10);
    res = do_nvme_admin_cmd(psp, shp, cmdp, NULL, 0, false, time_secs, vb);
    if (0 != res) {
        if (SG_LIB_NVME_STATUS == res) {
            mk_sense_from_nvme_status(psp, vb);
            return 0;
        } else
            return res;
    } else {
        psp->os_err = 0;
        psp->nvme_status = 0;
    }
    psp->sense_resid = psp->sense_len;
    pow_state = (0x1f & psp->nvme_result);
    if (vb > 3)
        pr2ws("%s: pow_state=%u\n", __func__, pow_state);
    memset(rs_dout, 0, sizeof(rs_dout));
    if (pow_state)
        build_sense_buffer(desc, rs_dout, SPC_SK_NO_SENSE,
                           LOW_POWER_COND_ON_ASC, 0);
    else
        build_sense_buffer(desc, rs_dout, SPC_SK_NO_SENSE,
                           NO_ADDITIONAL_SENSE, 0);
    n = desc ? 8 : 18;
    n = (n < alloc_len) ? n : alloc_len;
    n = (n < psp->dxfer_len) ? n : psp->dxfer_len;
    psp->resid = psp->dxfer_len - n;
    if (n > 0)
        memcpy(psp->dxferp, rs_dout, n);
    return 0;
}

/* This is not really a SNTL. For SCSI SEND DIAGNOSTIC(PF=1) NVMe-MI
 * has a special command (SES Send) to tunnel through pages to an
 * enclosure. The NVMe enclosure is meant to understand the SES
 * (SCSI Enclosure Services) use of diagnostics pages that are
 * related to SES. */
static int
sntl_senddiag(struct sg_pt_win32_scsi * psp, struct sg_pt_handle * shp,
               const uint8_t * cdbp, int time_secs, int vb)
{
    bool pf, self_test;
    int res;
    uint8_t st_cd, dpg_cd;
    uint32_t alloc_len, n, dout_len, dpg_len, nvme_dst;
    uint32_t pg_sz = sg_get_page_size();
    uint8_t * dop;
    uint8_t * cmdp;

    st_cd = 0x7 & (cdbp[1] >> 5);
    self_test = !! (0x4 & cdbp[1]);
    pf = !! (0x10 & cdbp[1]);
    if (vb > 3)
        pr2ws("%s: pf=%d, self_test=%d (st_code=%d)\n", __func__, (int)pf,
              (int)self_test, (int)st_cd);
    cmdp = psp->nvme_cmd;
    if (self_test || st_cd) {
        memset(cmdp, 0, sizeof(psp->nvme_cmd));
        cmdp[SG_NVME_PT_OPCODE] = 0x14;   /* Device self-test */
        /* just this namespace (if there is one) and controller */
        sg_put_unaligned_le32(psp->nvme_nsid, cmdp + SG_NVME_PT_NSID);
        switch (st_cd) {
        case 0: /* Here if self_test is set, do short self-test */
        case 1: /* Background short */
        case 5: /* Foreground short */
            nvme_dst = 1;
            break;
        case 2: /* Background extended */
        case 6: /* Foreground extended */
            nvme_dst = 2;
            break;
        case 4: /* Abort self-test */
            nvme_dst = 0xf;
            break;
        default:
            pr2ws("%s: bad self-test code [0x%x]\n", __func__, st_cd);
            mk_sense_invalid_fld(psp, true, 1, 7, vb);
            return 0;
        }
        sg_put_unaligned_le32(nvme_dst, cmdp + SG_NVME_PT_CDW10);
        res = do_nvme_admin_cmd(psp, shp, cmdp, NULL, 0, false, time_secs,
                                vb);
        if (0 != res) {
            if (SG_LIB_NVME_STATUS == res) {
                mk_sense_from_nvme_status(psp, vb);
                return 0;
            } else
                return res;
        }
    }
    alloc_len = sg_get_unaligned_be16(cdbp + 3); /* parameter list length */
    dout_len = psp->dxfer_len;
    if (pf) {
        if (0 == alloc_len) {
            mk_sense_invalid_fld(psp, true, 3, 7, vb);
            if (vb)
                pr2ws("%s: PF bit set bit param_list_len=0\n", __func__);
            return 0;
        }
    } else {    /* PF bit clear */
        if (alloc_len) {
            mk_sense_invalid_fld(psp, true, 3, 7, vb);
            if (vb)
                pr2ws("%s: param_list_len>0 but PF clear\n", __func__);
            return 0;
        } else
            return 0;     /* nothing to do */
        if (dout_len > 0) {
            if (vb)
                pr2ws("%s: dout given but PF clear\n", __func__);
            return SCSI_PT_DO_BAD_PARAMS;
        }
    }
    if (dout_len < 4) {
        if (vb)
            pr2ws("%s: dout length (%u bytes) too short\n", __func__,
                  dout_len);
        return SCSI_PT_DO_BAD_PARAMS;
    }
    n = dout_len;
    n = (n < alloc_len) ? n : alloc_len;
    dop = psp->dxferp;
    if (! is_aligned(dop, pg_sz)) {  /* caller best use sg_memalign(,pg_sz) */
        if (vb)
            pr2ws("%s: dout [0x%" PRIx64 "] not page aligned\n", __func__,
                  (uint64_t)(sg_uintptr_t)psp->dxferp);
        return SCSI_PT_DO_BAD_PARAMS;
    }
    dpg_cd = dop[0];
    dpg_len = sg_get_unaligned_be16(dop + 2) + 4;
    /* should we allow for more than one D_PG is dout ?? */
    n = (n < dpg_len) ? n : dpg_len;    /* not yet ... */

    if (vb)
        pr2ws("%s: passing through d_pg=0x%x, len=%u to NVME_MI SES send\n",
              __func__, dpg_cd, dpg_len);
    memset(cmdp, 0, sizeof(psp->nvme_cmd));
    cmdp[SG_NVME_PT_OPCODE] = 0x1d;   /* MI Send */
    /* And 0x1d is same opcode as the SCSI SEND DIAGNOSTIC command */
    sg_put_unaligned_le64((uint64_t)(sg_uintptr_t)dop,
                          cmdp + SG_NVME_PT_ADDR);
   /* NVMe 4k page size. Maybe determine this? */
   /* N.B. Maybe n  > 0x1000, is this a problem?? */
    sg_put_unaligned_le32(0x1000, cmdp + SG_NVME_PT_DATA_LEN);
    /* NVMe Message Header */
    sg_put_unaligned_le32(0x0804, cmdp + SG_NVME_PT_CDW10);
    /* NVME-MI SES Send; (0x8 -> NVME-MI SES Receive) */
    sg_put_unaligned_le32(0x9, cmdp + SG_NVME_PT_CDW11);
    /* 'n' is number of bytes SEND DIAGNOSTIC dpage */
    sg_put_unaligned_le32(n, cmdp + SG_NVME_PT_CDW13);
    res = do_nvme_admin_cmd(psp, shp, cmdp, dop, n, false, time_secs, vb);
    if (0 != res) {
        if (SG_LIB_NVME_STATUS == res) {
            mk_sense_from_nvme_status(psp, vb);
            return 0;
        }
    }
    return res;
}

/* This is not really a SNTL. For SCSI RECEIVE DIAGNOSTIC RESULTS(PCV=1)
 * NVMe-MI has a special command (SES Receive) to read pages through a
 * tunnel from an enclosure. The NVMe enclosure is meant to understand the
 * SES (SCSI Enclosure Services) use of diagnostics pages that are
 * related to SES. */
static int
sntl_recvdiag(struct sg_pt_win32_scsi * psp, struct sg_pt_handle * shp,
               const uint8_t * cdbp, int time_secs, int vb)
{
    bool pcv;
    int res;
    uint8_t dpg_cd;
    uint32_t alloc_len, n, din_len;
    uint32_t pg_sz = sg_get_page_size();
    uint8_t * dip;
    uint8_t * cmdp;

    pcv = !! (0x1 & cdbp[1]);
    dpg_cd = cdbp[2];
    alloc_len = sg_get_unaligned_be16(cdbp + 3); /* parameter list length */
    if (vb > 3)
        pr2ws("%s: dpg_cd=0x%x, pcv=%d, alloc_len=0x%x\n", __func__,
              dpg_cd, (int)pcv, alloc_len);
    din_len = psp->dxfer_len;
    n = (din_len < alloc_len) ? din_len : alloc_len;
    dip = psp->dxferp;
    if (! is_aligned(dip, pg_sz)) {  /* caller best use sg_memalign(,pg_sz) */
        if (vb)
            pr2ws("%s: din [0x%" PRIx64 "] not page aligned\n", __func__,
                  (uint64_t)(sg_uintptr_t)psp->dxferp);
        return SCSI_PT_DO_BAD_PARAMS;
    }

    if (vb)
        pr2ws("%s: expecting d_pg=0x%x from NVME_MI SES receive\n", __func__,
              dpg_cd);
    cmdp = psp->nvme_cmd;
    memset(cmdp, 0, sizeof(psp->nvme_cmd));
    cmdp[SG_NVME_PT_OPCODE] = 0x1e;   /* MI Receive */
    sg_put_unaligned_le64((uint64_t)(sg_uintptr_t)dip,
                          cmdp + SG_NVME_PT_ADDR);
   /* NVMe 4k page size. Maybe determine this? */
   /* N.B. Maybe n  > 0x1000, is this a problem?? */
    sg_put_unaligned_le32(0x1000, cmdp + SG_NVME_PT_DATA_LEN);
    /* NVMe Message Header */
    sg_put_unaligned_le32(0x0804, cmdp + SG_NVME_PT_CDW10);
    /* NVME-MI SES Receive */
    sg_put_unaligned_le32(0x8, cmdp + SG_NVME_PT_CDW11);
    /* Diagnostic page code */
    sg_put_unaligned_le32(dpg_cd, cmdp + SG_NVME_PT_CDW12);
    /* 'n' is number of bytes expected in diagnostic page */
    sg_put_unaligned_le32(n, cmdp + SG_NVME_PT_CDW13);
    res = do_nvme_admin_cmd(psp, shp, cmdp, dip, n, true, time_secs, vb);
    if (0 != res) {
        if (SG_LIB_NVME_STATUS == res) {
            mk_sense_from_nvme_status(psp, vb);
            return 0;
        } else
            return res;
    }
    psp->resid = din_len - n;
    return res;
}

#define F_SA_LOW                0x80    /* cdb byte 1, bits 4 to 0 */
#define F_SA_HIGH               0x100   /* as used by variable length cdbs */
#define FF_SA (F_SA_HIGH | F_SA_LOW)
#define F_INV_OP                0x200

static struct opcode_info_t {
        uint8_t opcode;
        uint16_t sa;            /* service action, 0 for none */
        uint32_t flags;         /* OR-ed set of F_* flags */
        uint8_t len_mask[16];   /* len=len_mask[0], then mask for cdb[1]... */
                                /* ignore cdb bytes after position 15 */
    } opcode_info_arr[] = {
    {0x0, 0, 0, {6,              /* TEST UNIT READY */
      0, 0, 0, 0, 0xc7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} },
    {0x3, 0, 0, {6,             /* REQUEST SENSE */
      0xe1, 0, 0, 0xff, 0xc7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} },
    {0x12, 0, 0, {6,            /* INQUIRY */
      0xe3, 0xff, 0xff, 0xff, 0xc7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} },
    {0x1c, 0, 0, {6,            /* RECEIVE DIAGNOSTIC RESULTS */
      0x1, 0xff, 0xff, 0xff, 0xc7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} },
    {0x1d, 0, 0, {6,            /* SEND DIAGNOSTIC */
      0xf7, 0x0, 0xff, 0xff, 0xc7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} },
    {0xa0, 0, 0, {12,           /* REPORT LUNS */
      0xe3, 0xff, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0xc7, 0, 0, 0, 0} },
    {0xa3, 0xc, F_SA_LOW, {12,  /* REPORT SUPPORTED OPERATION CODES */
      0xc, 0x87, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0xc7, 0, 0, 0,
      0} },
    {0xa3, 0xd, F_SA_LOW, {12,  /* REPORT SUPPORTED TASK MAN. FUNCTIONS */
      0xd, 0x80, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0xc7, 0, 0, 0, 0} },

    {0xff, 0xffff, 0xffff, {0,  /* Sentinel, keep as last element */
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} },
};

static int
sntl_rep_opcodes(struct sg_pt_win32_scsi * psp, struct sg_pt_handle * shp,
                 const uint8_t * cdbp, int time_secs, int vb)
{
    bool rctd;
    uint8_t reporting_opts, req_opcode, supp;
    uint16_t req_sa, u;
    uint32_t alloc_len, offset, a_len;
    uint32_t pg_sz = sg_get_page_size();
    int k, len, count, bump;
    const struct opcode_info_t *oip;
    uint8_t *arr;
    uint8_t *free_arr;

    if (vb > 3)
        pr2ws("%s: time_secs=%d\n", __func__, time_secs);
    if (shp) { ; }      /* suppress warning */
    rctd = !!(cdbp[2] & 0x80);      /* report command timeout desc. */
    reporting_opts = cdbp[2] & 0x7;
    req_opcode = cdbp[3];
    req_sa = sg_get_unaligned_be16(cdbp + 4);
    alloc_len = sg_get_unaligned_be32(cdbp + 6);
    if (alloc_len < 4 || alloc_len > 0xffff) {
        mk_sense_invalid_fld(psp, true, 6, -1, vb);
        return 0;
    }
    a_len = pg_sz - 72;
    arr = sg_memalign(pg_sz, pg_sz, &free_arr, vb > 3);
    if (NULL == arr) {
        pr2ws("%s: calloc() failed to get memory\n", __func__);
        return -ENOMEM;
    }
    switch (reporting_opts) {
    case 0: /* all commands */
        count = 0;
        bump = rctd ? 20 : 8;
        for (offset = 4, oip = opcode_info_arr;
             (oip->flags != 0xffff) && (offset < a_len); ++oip) {
            if (F_INV_OP & oip->flags)
                continue;
            ++count;
            arr[offset] = oip->opcode;
            sg_put_unaligned_be16(oip->sa, arr + offset + 2);
            if (rctd)
                arr[offset + 5] |= 0x2;
            if (FF_SA & oip->flags)
                arr[offset + 5] |= 0x1;
            sg_put_unaligned_be16(oip->len_mask[0], arr + offset + 6);
            if (rctd)
                sg_put_unaligned_be16(0xa, arr + offset + 8);
            offset += bump;
        }
        sg_put_unaligned_be32(count * bump, arr + 0);
        break;
    case 1: /* one command: opcode only */
    case 2: /* one command: opcode plus service action */
    case 3: /* one command: if sa==0 then opcode only else opcode+sa */
        for (oip = opcode_info_arr; oip->flags != 0xffff; ++oip) {
            if ((req_opcode == oip->opcode) && (req_sa == oip->sa))
                break;
        }
        if ((0xffff == oip->flags) || (F_INV_OP & oip->flags)) {
            supp = 1;
            offset = 4;
        } else {
            if (1 == reporting_opts) {
                if (FF_SA & oip->flags) {
                    mk_sense_invalid_fld(psp, true, 2, 2, vb);
                    free(free_arr);
                    return 0;
                }
                req_sa = 0;
            } else if ((2 == reporting_opts) && 0 == (FF_SA & oip->flags)) {
                mk_sense_invalid_fld(psp, true, 4, -1, vb);
                free(free_arr);
                return 0;
            }
            if ((0 == (FF_SA & oip->flags)) && (req_opcode == oip->opcode))
                supp = 3;
            else if (0 == (FF_SA & oip->flags))
                supp = 1;
            else if (req_sa != oip->sa)
                supp = 1;
            else
                supp = 3;
            if (3 == supp) {
                u = oip->len_mask[0];
                sg_put_unaligned_be16(u, arr + 2);
                arr[4] = oip->opcode;
                for (k = 1; k < u; ++k)
                    arr[4 + k] = (k < 16) ?
                oip->len_mask[k] : 0xff;
                offset = 4 + u;
            } else
                offset = 4;
        }
        arr[1] = (rctd ? 0x80 : 0) | supp;
        if (rctd) {
            sg_put_unaligned_be16(0xa, arr + offset);
            offset += 12;
        }
        break;
    default:
        mk_sense_invalid_fld(psp, true, 2, 2, vb);
        free(free_arr);
        return 0;
    }
    offset = (offset < a_len) ? offset : a_len;
    len = (offset < alloc_len) ? offset : alloc_len;
    psp->resid = psp->dxfer_len - len;
    if (len > 0)
        memcpy(psp->dxferp, arr, len);
    free(free_arr);
    return 0;
}

static int
sntl_rep_tmfs(struct sg_pt_win32_scsi * psp, struct sg_pt_handle * shp,
              const uint8_t * cdbp, int time_secs, int vb)
{
    bool repd;
    uint32_t alloc_len, len;
    uint8_t arr[16];

    if (vb > 3)
        pr2ws("%s: time_secs=%d\n", __func__, time_secs);
    if (shp) { ; }      /* suppress warning */
    memset(arr, 0, sizeof(arr));
    repd = !!(cdbp[2] & 0x80);
    alloc_len = sg_get_unaligned_be32(cdbp + 6);
    if (alloc_len < 4) {
        mk_sense_invalid_fld(psp, true, 6, -1, vb);
        return 0;
    }
    arr[0] = 0xc8;          /* ATS | ATSS | LURS */
    arr[1] = 0x1;           /* ITNRS */
    if (repd) {
        arr[3] = 0xc;
        len = 16;
    } else
        len = 4;

    len = (len < alloc_len) ? len : alloc_len;
    psp->resid = psp->dxfer_len - len;
    if (len > 0)
        memcpy(psp->dxferp, arr, len);
    return 0;
}

/* Executes NVMe Admin command (or at least forwards it to lower layers).
 * Returns 0 for success, negative numbers are negated 'errno' values from
 * OS system calls. Positive return values are errors from this package.
 * When time_secs is 0 the Linux NVMe Admin command default of 60 seconds
 * is used. */
static int
do_nvme_pt(struct sg_pt_win32_scsi * psp, struct sg_pt_handle * shp,
           int time_secs, int vb)
{
    bool scsi_cdb = false;
    uint32_t cmd_len = 0;
    uint16_t sa;
    const uint8_t * cdbp = NULL;

    if (psp->have_nvme_cmd) {
        cdbp = psp->nvme_cmd;
        cmd_len = 64;
        psp->nvme_direct = true;
    } else if (spt_direct) {
        if (psp->swb_d.spt.CdbLength > 0) {
            cdbp = psp->swb_d.spt.Cdb;
            cmd_len = psp->swb_d.spt.CdbLength;
            scsi_cdb = true;
            psp->nvme_direct = false;
        }
    } else {
        if (psp->swb_i.spt.CdbLength > 0) {
            cdbp = psp->swb_i.spt.Cdb;
            cmd_len = psp->swb_i.spt.CdbLength;
            scsi_cdb = true;
            psp->nvme_direct = false;
        }
    }
    if (NULL == cdbp) {
        if (vb)
            pr2ws("%s: Missing NVMe or SCSI command (set_scsi_pt_cdb())"
                  " cmd_len=%u\n", __func__, cmd_len);
        return SCSI_PT_DO_BAD_PARAMS;
    }
    if (vb > 3)
        pr2ws("%s: opcode=0x%x, cmd_len=%u, fdev_name: %s\n", __func__,
              cdbp[0], cmd_len, shp->dname);
    /* direct NVMe command (i.e. 64 bytes long) or SNTL */
    if (scsi_cdb) {
        switch (cdbp[0]) {
        case SCSI_INQUIRY_OPC:
            return sntl_inq(psp, shp, cdbp, time_secs, vb);
        case SCSI_REPORT_LUNS_OPC:
            return sntl_rluns(psp, shp, cdbp, time_secs, vb);
        case SCSI_TEST_UNIT_READY_OPC:
            return sntl_tur(psp, shp, time_secs, vb);
        case SCSI_REQUEST_SENSE_OPC:
            return sntl_req_sense(psp, shp, cdbp, time_secs, vb);
        case SCSI_SEND_DIAGNOSTIC_OPC:
            return sntl_senddiag(psp, shp, cdbp, time_secs, vb);
        case SCSI_RECEIVE_DIAGNOSTIC_OPC:
            return sntl_recvdiag(psp, shp, cdbp, time_secs, vb);
        case SCSI_MAINT_IN_OPC:
            sa = 0x1f & cdbp[1];        /* service action */
            if (SCSI_REP_SUP_OPCS_OPC == sa)
                return sntl_rep_opcodes(psp, shp, cdbp, time_secs,
                                        vb);
            else if (SCSI_REP_SUP_TMFS_OPC == sa)
                return sntl_rep_tmfs(psp, shp, cdbp, time_secs, vb);
            /* fall through */
        default:
            if (vb > 2) {
                char b[64];

                sg_get_command_name(cdbp, -1, sizeof(b), b);
                pr2ws("%s: no translation to NVMe for SCSI %s command\n",
                      __func__, b);
            }
            mk_sense_asc_ascq(psp, SPC_SK_ILLEGAL_REQUEST, INVALID_OPCODE,
                              0, vb);
            return 0;
        }
    }
    return do_nvme_admin_cmd(psp, shp, NULL, NULL, 0, false, time_secs, vb);
}

#else           /* (HAVE_NVME && (! IGNORE_NVME)) */

static int
do_nvme_pt(struct sg_pt_win32_scsi * psp, struct sg_pt_handle * shp,
           int time_secs, int vb)
{
    if (vb)
        pr2ws("%s: not supported [time_secs=%d]\n", __func__, time_secs);
    if (psp) { ; }              /* suppress warning */
    if (dhp) { ; }              /* suppress warning */
    return -ENOTTY;             /* inappropriate ioctl error */
}

#endif          /* (HAVE_NVME && (! IGNORE_NVME)) */
