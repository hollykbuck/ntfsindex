#pragma once

#include <cstdint>

#pragma pack(push, 1)

// NTFS signatures
enum NtfsSignature : uint32_t {
    SIGN_FILE = 0x454C4946, // 'FILE'
    SIGN_INDX = 0x58444E49, // 'INDX'
    SIGN_BAAD = 0x44414142, // 'BAAD'
    SIGN_RSTR = 0x52545352, // 'RSTR'
    SIGN_RCRD = 0x44524352, // 'RCRD'
};

// NTFS record flags
enum RecordFlags : uint16_t {
    RECORD_FLAG_IN_USE = 0x0001,
    RECORD_FLAG_DIR    = 0x0002,
    RECORD_FLAG_SYSTEM = 0x0004,
    RECORD_FLAG_INDEX  = 0x0008,
};

// NTFS Boot Sector (Volume Boot Record)
struct NTFS_BOOT {
    uint8_t  jump_code[3];
    uint8_t  system_id[8]; // "NTFS    "
    uint8_t  bytes_per_sector[2];
    uint8_t  sectors_per_cluster;
    uint8_t  unused1[7];
    uint8_t  media_type;
    uint8_t  unused2[2];
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint8_t  unused3[4];
    uint8_t  bios_drive_num;
    uint8_t  unused4;
    uint8_t  signature_ex;
    uint8_t  unused5;
    uint64_t sectors_per_volume;
    uint64_t mft_clst;  // Starting cluster of $MFT
    uint64_t mft2_clst; // Starting cluster of $MFTMirr
    int8_t   record_size; // Size of MFT record. If > 0, size in clusters; if < 0, 2^|record_size| bytes
    uint8_t  unused6[3];
    int8_t   index_size;  // Size of Index record. If > 0, size in clusters; if < 0, 2^|index_size| bytes
    uint8_t  unused7[3];
    uint64_t serial_num;
    uint32_t check_sum;
    uint8_t  boot_code[0x200 - 0x50 - 2 - 4];
    uint8_t  boot_magic[2]; // 0x55, 0xAA
};
static_assert(sizeof(NTFS_BOOT) == 0x200, "NTFS_BOOT size must be 512 bytes");

// NTFS Record Header
struct NTFS_RECORD_HEADER {
    uint32_t sign;    // NtfsSignature
    uint16_t fix_off; // Offset to the Update Sequence Array (USA)
    uint16_t fix_num; // Size of USA (USN + sector original values)
    uint64_t lsn;     // Log file sequence number
};
static_assert(sizeof(NTFS_RECORD_HEADER) == 0x10, "NTFS_RECORD_HEADER size must be 16 bytes");

// MFT reference structure
struct MFT_REF {
    uint32_t low;
    uint16_t high;
    uint16_t seq;

    uint64_t get_record_id() const {
        return low | (static_cast<uint64_t>(high) << 32);
    }
};
static_assert(sizeof(MFT_REF) == 8, "MFT_REF size must be 8 bytes");

// MFT Record (normally 1024 bytes)
struct MFT_REC {
    NTFS_RECORD_HEADER rhdr;
    uint16_t seq;
    uint16_t hard_links;
    uint16_t attr_off; // Offset to the first attribute
    uint16_t flags;    // RecordFlags
    uint32_t used;     // Used size of MFT record
    uint32_t total;    // Total size of MFT record
    MFT_REF  parent_ref;
    uint16_t next_attr_id;
    uint16_t res;
    uint32_t mft_record; // Record index of this MFT record
};
static_assert(sizeof(MFT_REC) == 0x30, "MFT_REC size must be 48 bytes");

// Attribute Types
enum AttributeType : uint32_t {
    ATTR_ZERO       = 0x00,
    ATTR_STD        = 0x10, // $STANDARD_INFORMATION
    ATTR_LIST       = 0x20, // $ATTRIBUTE_LIST
    ATTR_NAME       = 0x30, // $FILE_NAME
    ATTR_OBJECT_ID  = 0x40, // $OBJECT_ID
    ATTR_SECURE     = 0x50, // $SECURITY_DESCRIPTOR
    ATTR_LABEL      = 0x60, // $VOLUME_NAME
    ATTR_VOL_INFO   = 0x70, // $VOLUME_INFORMATION
    ATTR_DATA       = 0x80, // $DATA
    ATTR_ROOT       = 0x90, // $INDEX_ROOT
    ATTR_ALLOC      = 0xA0, // $INDEX_ALLOCATION
    ATTR_BITMAP     = 0xB0, // $BITMAP
    ATTR_REPARSE    = 0xC0, // $REPARSE_POINT
    ATTR_EA_INFO    = 0xD0, // $EA_INFORMATION
    ATTR_EA         = 0xE0, // $EA
    ATTR_PROPERTYSET	= 0xF0,
    ATTR_LOGGED_UTILITY_STREAM = 0x100,
    ATTR_END        = 0xFFFFFFFF,
};

// Resident Attribute Header Details
struct ATTR_RESIDENT {
    uint32_t data_size;
    uint16_t data_off; // Offset to data from start of attribute
    uint8_t  flags;    // e.g. 1 = indexed
    uint8_t  res;
};

// Non-resident Attribute Header Details
struct ATTR_NONRESIDENT {
    uint64_t svcn; // Starting VCN
    uint64_t evcn; // Ending VCN
    uint16_t run_off; // Offset to packed runs from start of attribute
    uint8_t  c_unit;
    uint8_t  res1[5];
    uint64_t alloc_size;
    uint64_t data_size;
    uint64_t valid_size;
    uint64_t total_size;
};

// General Attribute structure
struct ATTRIB {
    uint32_t type;     // AttributeType
    uint32_t size;     // Length of this attribute record (including name and data)
    uint8_t  non_res;  // 0 = Resident, 1 = Non-resident
    uint8_t  name_len; // Name length in characters
    uint16_t name_off; // Offset to name from start of attribute
    uint16_t flags;    // Attribute flags (compression, encryption, sparse)
    uint16_t id;       // Unique attribute ID

    union {
        ATTR_RESIDENT    res;
        ATTR_NONRESIDENT nres;
    };
};
static_assert(sizeof(ATTRIB) == 0x48, "ATTRIB size must be 72 bytes");

// Duplicate information block
struct NTFS_DUP_INFO {
    uint64_t cr_time;
    uint64_t m_time;
    uint64_t c_time;
    uint64_t a_time;
    uint64_t alloc_size;
    uint64_t data_size;
    uint32_t fa; // File attributes (e.g. FILE_ATTRIBUTE_DIRECTORY)
    uint32_t extend_data;
};
static_assert(sizeof(NTFS_DUP_INFO) == 0x38, "NTFS_DUP_INFO size must be 56 bytes");

// File Name Attribute ($FILE_NAME, 0x30)
struct ATTR_FILE_NAME {
    MFT_REF       home; // Parent directory MFT reference
    NTFS_DUP_INFO dup;
    uint8_t       name_len; // Name length in characters
    uint8_t       type;     // Name type (0 = POSIX, 1 = Unicode, 2 = DOS, 3 = Unicode & DOS)
    uint16_t      name[1];  // Variable-sized name (UTF-16LE)
};

#pragma pack(pop)
