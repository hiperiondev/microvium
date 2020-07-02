#pragma once

#include "stdint.h"

#define MVM_BYTECODE_VERSION 1

typedef struct mvm_TsBytecodeHeader {
  /* TODO: I think the performance of accessing this header would improve
  slightly if the offsets were stored as auto-relative-offsets. My reasoning is
  that we don't need to keep the pBytecode pointer for the second lookup. But
  it's maybe worth doing some tests.
  */
  uint8_t bytecodeVersion; // MVM_BYTECODE_VERSION
  uint8_t headerSize;
  uint16_t bytecodeSize; // Including header
  uint16_t crc; // CCITT16 (header and data, of everything after the CRC)
  uint16_t requiredEngineVersion;
  uint32_t requiredFeatureFlags;
  uint16_t globalVariableCount;
  uint16_t gcRootsOffset; // Points to a table of pointers to GC roots in data memory (to use in addition to the global variables as roots)
  uint16_t gcRootsCount;
  uint16_t importTableOffset; // vm_TsImportTableEntry
  uint16_t importTableSize;
  uint16_t exportTableOffset; // vm_TsExportTableEntry
  uint16_t exportTableSize;
  uint16_t shortCallTableOffset; // vm_TsShortCallTableEntry
  uint16_t shortCallTableSize;
  uint16_t stringTableOffset; // Alphabetical index of UNIQUED_STRING values (TODO: Check these are always generated at 2-byte alignment)
  uint16_t stringTableSize;
  uint16_t arrayProtoPointer; // Pointer to array prototype
  uint16_t initialDataOffset; // Note: the initial-data section MUST be second-last in the bytecode file
  uint16_t initialDataSize;
  uint16_t initialHeapOffset; // Note: the initial heap MUST be the last thing in the bytecode file, since it's the only thing that changes size from one snapshot to the next on the native VM.
  uint16_t initialHeapSize;
} mvm_TsBytecodeHeader;

typedef enum mvm_TeFeatureFlags {
  FF_FLOAT_SUPPORT = 0,
} mvm_TeFeatureFlags;
