#pragma once

// TODO: I think we should rename `vm_` to `mvm_` to correspond to the new project name "Microvium"

/* TODO(low): I think this unit should be refactored:

1. Create a new header file called `vm_bytecode.h`. The VM has two interfaces to
   the outside world: byte front-end, represented in microvium.h, and the bytecode
   interface represented in vm_bytecode.

2. Move all definitions out of here and into either microvium.c or vm_bytecode.h,
   depending on whether they're internal to the implementation of the engine or
   whether they represent the bytecode interface.

3. We should probably refactor the macros into static const values and inline
   functions, and let the optimizer sort things out.

*/

#include "stdbool.h"
#include "stdint.h"
#include "assert.h"
#include "string.h"
#include "stdlib.h"
#include "setjmp.h"

#include "microvium.h"
#include "microvium_port.h"
#include "microvium_bytecode.h"
#include "microvium_opcodes.h"

#define VM_BYTECODE_VERSION 1

#if MVM_SAFE_MODE
#define VM_ASSERT(vm, predicate) do { if (!(predicate)) MVM_FATAL_ERROR(vm, MVM_E_ASSERTION_FAILED); } while (false)
#else
#define VM_ASSERT(vm, predicate)
#endif

// Offset of field in a struct
#define OFFSETOF(TYPE, ELEMENT) ((size_t)&(((TYPE *)0)->ELEMENT))

#define VM_ALLOCATION_BUCKET_SIZE 256
#define VM_GC_ALLOCATION_UNIT     2    // Don't change
#define VM_GC_MIN_ALLOCATION_SIZE (VM_GC_ALLOCATION_UNIT * 2)

#define VM_TAG_MASK               0xC000 // The tag is the top 2 bits
#define VM_VALUE_MASK             0x3FFF // The value is the remaining 14 bits
#define VM_VALUE_SIGN_BIT         0x2000 // Sign bit used for signed numbers

#define VM_VALUE_UNSIGNED         0x0000
#define VM_VALUE_SIGNED           0x2000
#define VM_SIGN_EXTENTION         0xC000
#define VM_OVERFLOW_BIT           0x4000

// TODO(low): I think these should be inline functions rather than macros
#define VM_VALUE_OF(v) ((v) & VM_VALUE_MASK)
#define VM_TAG_OF(v) ((TeValueTag)((v) & VM_TAG_MASK))
#define VM_IS_INT14(v) (VM_TAG_OF(v) == VM_TAG_INT)
#define VM_IS_GC_P(v) (VM_TAG_OF(v) == VM_TAG_GC_P)
#define VM_IS_DATA_P(v) (VM_TAG_OF(v) == VM_TAG_DATA_P)
#define VM_IS_PGM_P(v) (VM_TAG_OF(v) == VM_TAG_PGM_P)

// This is the only valid way of representing NaN
#define VM_IS_NAN(v) ((v) == VM_VALUE_NAN)
// This is the only valid way of representing infinity
#define VM_IS_INF(v) ((v) == VM_VALUE_INF)
// This is the only valid way of representing -infinity
#define VM_IS_NEG_INF(v) ((v) == VM_VALUE_NEG_INF)
// This is the only valid way of representing negative zero
#define VM_IS_NEG_ZERO(v) ((v) == VM_VALUE_NEG_ZERO)

#define VM_NOT_IMPLEMENTED(vm) (MVM_FATAL_ERROR(vm, MVM_E_NOT_IMPLEMENTED), -1)

// An error corresponding to an internal inconsistency in the VM. Such an error
// cannot be caused by incorrect usage of the VM. In safe mode, this function
// should terminate the application. If not in safe mode, it is assumed that
// this function will never be invoked.
#define VM_UNEXPECTED_INTERNAL_ERROR(vm) (MVM_FATAL_ERROR(vm, MVM_E_UNEXPECTED), -1)

#define VM_VALUE_OF_DYNAMIC(v) ((void*)((vm_TsDynamicHeader*)v + 1))
#define VM_DYNAMIC_TYPE(v) (((vm_TsDynamicHeader*)v)->type)

#define VM_MAX_INT14 0x1FFF
#define VM_MIN_INT14 (-0x2000)

#if MVM_SAFE_MODE
#define VM_EXEC_SAFE_MODE(code) code
#define VM_SAFE_CHECK_NOT_NULL(v) do { if ((v) == NULL) return MVM_E_UNEXPECTED; } while (false)
#define VM_SAFE_CHECK_NOT_NULL_2(v) do { if ((v) == NULL) { MVM_FATAL_ERROR(vm, MVM_E_UNEXPECTED); return NULL; } } while (false)
#define VM_ASSERT_UNREACHABLE(vm) (MVM_FATAL_ERROR(vm, MVM_E_UNEXPECTED), -1)
#else
#define VM_EXEC_SAFE_MODE(code)
#define VM_SAFE_CHECK_NOT_NULL(v)
#define VM_SAFE_CHECK_NOT_NULL_2(v)
#define VM_ASSERT_UNREACHABLE(vm)
#endif

#define VM_READ_BC_1_AT(offset, pBytecode) MVM_READ_PROGMEM_1(MVM_PROGMEM_P_ADD((pBytecode), offset));
#define VM_READ_BC_2_AT(offset, pBytecode) MVM_READ_PROGMEM_2(MVM_PROGMEM_P_ADD((pBytecode), offset));
#define VM_READ_BC_4_AT(offset, pBytecode) MVM_READ_PROGMEM_4(MVM_PROGMEM_P_ADD((pBytecode), offset));
#define VM_READ_BC_8_AT(offset, pBytecode) MVM_READ_PROGMEM_8(MVM_PROGMEM_P_ADD((pBytecode), offset));
#define VM_READ_BC_N_AT(pTarget, offset, size, pBytecode) MVM_READ_PROGMEM_N(pTarget, MVM_PROGMEM_P_ADD((pBytecode), offset), size);

#define VM_READ_BC_1_FIELD(fieldName, structOffset, structType, pBytecode) VM_READ_BC_1_AT(structOffset + OFFSETOF(structType, fieldName), pBytecode);
#define VM_READ_BC_2_FIELD(fieldName, structOffset, structType, pBytecode) VM_READ_BC_2_AT(structOffset + OFFSETOF(structType, fieldName), pBytecode);

#define VM_READ_BC_1_HEADER_FIELD(fieldName, pBytecode) VM_READ_BC_1_FIELD(fieldName, 0, mvm_TsBytecodeHeader, pBytecode);
#define VM_READ_BC_2_HEADER_FIELD(fieldName, pBytecode) VM_READ_BC_2_FIELD(fieldName, 0, mvm_TsBytecodeHeader, pBytecode);

#define VM_BOTTOM_OF_STACK(vm) ((uint16_t*)(vm->stack + 1))
#define VM_TOP_OF_STACK(vm) (VM_BOTTOM_OF_STACK(vm) + MVM_STACK_SIZE / 2)
#define VM_IS_UNSIGNED(v) ((v & VM_VALUE_SIGN_BIT) == VM_VALUE_UNSIGNED)
#define VM_SIGN_EXTEND(v) (VM_IS_UNSIGNED(v) ? v : (v | VM_SIGN_EXTENTION))

// Internally, we don't need to use the mvm prefix for these common types
typedef mvm_Value Value;
typedef mvm_VM VM;
typedef mvm_TeError TeError;

/**
 * Type code indicating the type of data.
 *
 * This enumeration is divided into reference types (TC_REF_) and value types
 * (TC_VAL_). Reference type codes are used on allocations, whereas value type
 * codes are never used on allocations. The space for the type code in the
 * allocation header is 4 bits, so there are up to 16 reference types and these
 * must be the first 16 types in the enumeration.
 *
 * Value types are for the values that can be represented within the 16-bit
 * mvm_Value without interpreting it as a pointer.
 */
typedef enum TeTypeCode {
  // Note: only type code values in the range 0-15 can be used as the types for
  // allocations, since the allocation header allows 4 bits for the type

  /* --------------------------- Reference types --------------------------- */

  // TC_REF_NONE is used for allocations which are never addressable by a vm_Value,
  // and so their type will never be checked. This is only for internal data
  // structures.
  TC_REF_NONE           = 0x0,

  TC_REF_INT32          = 0x1, // 32-bit signed integer
  TC_REF_DOUBLE         = 0x2, // 64-bit float

  /**
   * UTF8-encoded string that may or may not be unique.
   *
   * Note: If a TC_REF_STRING is in bytecode, it is because it encodes a value
   * that is illegal as a property index in Microvium (i.e. it encodes an
   * integer).
   */
  TC_REF_STRING         = 0x3,

  /**
   * A string whose address uniquely identifies its contents, and does not
   * encode an integer in the range 0 to 0x1FFF
   */
  TC_REF_UNIQUE_STRING  = 0x4,

  TC_REF_PROPERTY_LIST  = 0x5, // Object represented as linked list of properties
  TC_REF_LIST           = 0x6, // Array represented as linked list
  TC_REF_TUPLE          = 0x7, // Array represented as contiguous block of memory
  TC_REF_FUNCTION       = 0x8, // Local function
  TC_REF_HOST_FUNC      = 0x9, // External function by index in import table
  // Structs are records with a fixed set of fields, and the field keys are
  // stored separately (TODO: Some work is required on refining these).
  TC_REF_STRUCT         = 0xA,

  TC_REF_BIG_INT        = 0xB, // Reserved
  TC_REF_SYMBOL         = 0xC, // Reserved
  TC_REF_RESERVED_1     = 0xD, // Reserved
  TC_REF_RESERVED_2     = 0xE, // Reserved
  TC_REF_RESERVED_3     = 0xF, // Reserved

  /* ----------------------------- Value types ----------------------------- */
  TC_VAL_INT14         = 0x10,
  TC_VAL_UNDEFINED     = 0x11,
  TC_VAL_NULL          = 0x12,
  TC_VAL_TRUE          = 0x13,
  TC_VAL_FALSE         = 0x14,
  TC_VAL_NAN           = 0x15,
  TC_VAL_NEG_ZERO      = 0x16,
  TC_VAL_DELETED       = 0x17, // Placeholder for properties and list items that have been deleted or holes in arrays
} TeTypeCode;

// Tag values
typedef enum TeValueTag {
  VM_TAG_INT    = 0x0000,
  VM_TAG_GC_P   = 0x4000,
  VM_TAG_DATA_P = 0x8000,
  VM_TAG_PGM_P  = 0xC000,
} TeValueTag;

// Note: VM_VALUE_NAN must be used instead of a pointer to a double that has a
// NaN value (i.e. the values must be normalized to use the following table).
// Operations will assume this canonical form.

// Some well-known values
typedef enum vm_TeWellKnownValues {
  VM_VALUE_UNDEFINED     = (VM_TAG_PGM_P | (int)TC_VAL_UNDEFINED),
  VM_VALUE_NULL          = (VM_TAG_PGM_P | (int)TC_VAL_NULL),
  VM_VALUE_TRUE          = (VM_TAG_PGM_P | (int)TC_VAL_TRUE),
  VM_VALUE_FALSE         = (VM_TAG_PGM_P | (int)TC_VAL_FALSE),
  VM_VALUE_NAN           = (VM_TAG_PGM_P | (int)TC_VAL_NAN),
  VM_VALUE_NEG_ZERO      = (VM_TAG_PGM_P | (int)TC_VAL_NEG_ZERO),
  VM_VALUE_DELETED       = (VM_TAG_PGM_P | (int)TC_VAL_DELETED),
  VM_VALUE_MAX_WELLKNOWN,
} vm_TeWellKnownValues;

// Note: These offsets don't include the tag
typedef uint16_t DO_t; // Offset into data memory space
typedef uint16_t GO_t; // Offset into garbage collected memory space
typedef uint16_t BO_t; // Offset into bytecode (pgm/ROM) memory space

/**
 * A pointer into one of the memory spaces, including the corresponding tag.
 *
 * Use the hungarian prefix vp when declaring values or paramters that are
 * intended to be pointers.
 *
 * Pointers are values that can generically refer into any address space.
 * Unfortunately, Microvium is designed to un in environments where bytecode is
 * stored non-locally, such as arduino where flash memory is a completely
 * separate address space. So, it is not assumed that there is a native pointer
 * that can homogenously refer to any memory address. Instead, we use the same
 * format as the mvm_Value, with a 2-bit tag indicating what kind of pointer it
 * is. Access to these pointers needs to be done indirectly, such as through
 * `vm_readUInt16` and similar methods;
 */
typedef mvm_Value vm_Pointer;

typedef uint16_t vm_HeaderWord;
typedef struct vm_TsStack vm_TsStack;



typedef struct vm_TsBucket {
  vm_Pointer vpAddressStart;
  struct vm_TsBucket* prev;
} vm_TsBucket;

struct mvm_VM {
  void* context;

  MVM_PROGMEM_P pBytecode;

  // Start of the last bucket of GC memory
  vm_TsBucket* pLastBucket;
  // End of the last bucket of GC memory
  vm_Pointer vpBucketEnd;
  // Where to allocate next GC allocation
  vm_Pointer vpAllocationCursor;
  uint8_t* pAllocationCursor;
  // Handles - values to treat as GC roots
  mvm_Handle* gc_handles;

  vm_TsStack* stack;
  vm_Pointer uniqueStrings; // Linked list of unique strings in GC memory (excludes those in ROM)
  uint16_t* dataMemory;
};

typedef struct TsUniqueStringCell {
  vm_Pointer next;
  Value str;
} TsUniqueStringCell;

typedef struct vm_TsExportTableEntry {
  mvm_VMExportID exportID;
  mvm_Value exportValue;
} vm_TsExportTableEntry;

typedef union vm_TsShortCallTableEntry {
  // If `function` high-bit is set, the `function` is an index into the
  // resolvedImports table. If `function` high-bit is not set, `function` is an
  // offset to a local function in the bytecode
  uint16_t function;
  uint8_t argCount;
} vm_TsShortCallTableEntry;

typedef struct vm_TsRegisters {
  uint16_t* pFrameBase;
  uint16_t* pStackPointer;
  BO_t programCounter;
  uint16_t argCount;
} vm_TsRegisters;

struct vm_TsStack {
  // Allocate registers along with the stack, because these are needed at the same time (i.e. while the VM is active)
  vm_TsRegisters reg;
  // ... (stack memory) ...
};

typedef struct vm_TsDynamicHeader {
  /* 4 least-significant-bits are the type code (TeTypeCode) */
  uint16_t headerData;
} vm_TsDynamicHeader;

typedef struct vm_TsFunctionHeader {
  // Note: The vm_TsFunctionHeader _starts_ at the target of the function
  // pointer, but there may be an additional vm_TsDynamicHeader _preceding_ the
  // pointer target.
  uint8_t maxStackDepth;
} vm_TsFunctionHeader;

typedef struct vm_TsImportTableEntry {
  mvm_HostFunctionID hostFunctionID;
} vm_TsImportTableEntry;
