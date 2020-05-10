/*
 * This file contains the Microvium virtual machine C implementation.
 *
 * For the moment, I'm keeping it all in one file for usability. User's can
 * treat this file as a black box that contains the VM, and there's only one
 * file they need to have built into their project in order to have Microvium
 * running.
 *
 * The two interfaces to this file are:
 *
 *   1. `microvium.h`, which is the interface from the user side (how to use the
 *      VM)
 *   2. `microvium_bytecode.h` which contains types related to the bytecode
 *      format.
 *
 * User-facing functions and definitions are all prefixed with `mvm_` to
 * namespace them separately from other functions in their project.
 *
 * Internal functions and definitions don't require a prefix, but for legacy
 * reasons, many have a `vm_` prefix. Perhaps this should be `ivm_` for
 * "internal VM".
 */

#include "microvium.h"

#include <ctype.h>

#include "microvium_internals.h"
#include "math.h"

static void vm_readMem(VM* vm, void* target, Pointer source, uint16_t size);
static void vm_writeMem(VM* vm, Pointer target, void* source, uint16_t size);

// Number of words on the stack required for saving the caller state
#define VM_FRAME_SAVE_SIZE_WORDS 3

static const Pointer vpGCSpaceStart = 0x4000;

// TODO: I think we can remove `vm_` from the internal methods and use `mvm_` for the external
static bool vm_isHandleInitialized(VM* vm, const mvm_Handle* handle);
static void* vm_deref(VM* vm, Value pSrc);
static TeError vm_run(VM* vm);
static void vm_push(VM* vm, uint16_t value);
static uint16_t vm_pop(VM* vm);
static TeError vm_setupCallFromExternal(VM* vm, Value func, Value* args, uint8_t argCount);
static Value vm_convertToString(VM* vm, Value value);
static Value vm_concat(VM* vm, Value left, Value right);
static Value vm_convertToNumber(VM* vm, Value value);
static TeTypeCode deepTypeOf(VM* vm, Value value);
static bool vm_isString(VM* vm, Value value);
static MVM_FLOAT64 vm_readDouble(VM* vm, TeTypeCode type, Value value);
static int32_t vm_readInt32(VM* vm, TeTypeCode type, Value value);
static inline vm_HeaderWord vm_readHeaderWord(VM* vm, Pointer pAllocation);
static uint16_t vm_readUInt16(VM* vm, Pointer p);
static void vm_writeUInt16(VM* vm, Pointer p, Value value);
static TeError vm_resolveExport(VM* vm, mvm_VMExportID id, Value* result);
static inline mvm_TfHostFunction* vm_getResolvedImports(VM* vm);
static inline uint16_t vm_getResolvedImportCount(VM* vm);
static void gc_createNextBucket(VM* vm, uint16_t bucketSize);
static Value gc_allocateWithHeader(VM* vm, uint16_t sizeBytes, TeTypeCode typeCode, uint16_t headerVal2, void** out_target);
static Pointer gc_allocateWithoutHeader(VM* vm, uint16_t sizeBytes, void** out_pTarget);
static void gc_markAllocation(uint16_t* markTable, GO_t p, uint16_t size);
static void gc_traceValue(VM* vm, uint16_t* markTable, Value value, uint16_t* pTotalSize);
static inline void gc_updatePointer(VM* vm, uint16_t* pWord, uint16_t* markTable, uint16_t* offsetTable);
static inline bool gc_isMarked(uint16_t* markTable, Pointer ptr);
static void gc_freeGCMemory(VM* vm);
static void* gc_deref(VM* vm, Pointer vp);
static Value vm_allocString(VM* vm, size_t sizeBytes, void** data);
static TeError getProperty(VM* vm, Value objectValue, Value propertyName, Value* propertyValue);
static TeError setProperty(VM* vm, Value objectValue, Value propertyName, Value propertyValue);
static TeError toPropertyName(VM* vm, Value* value);
static Value toUniqueString(VM* vm, Value value);
static int memcmp_pgm(void* p1, MVM_PROGMEM_P p2, size_t size);
static MVM_PROGMEM_P pgm_deref(VM* vm, Pointer vp);
static uint16_t vm_stringSizeUtf8(VM* vm, Value str);
static Value uintToStr(VM* vm, uint16_t i);
static bool vm_stringIsNonNegativeInteger(VM* vm, Value str);
static TeError toInt32Internal(mvm_VM* vm, mvm_Value value, int32_t* out_result);
static int32_t mvm_float64ToInt32(MVM_FLOAT64 value);

const Value mvm_undefined = VM_VALUE_UNDEFINED;
const Value vm_null = VM_VALUE_NULL;

static inline TeTypeCode vm_typeCodeFromHeaderWord(vm_HeaderWord headerWord) {
  CODE_COVERAGE(1); // Hit
  return (TeTypeCode)(headerWord >> 12);
}

static inline uint16_t vm_paramOfHeaderWord(vm_HeaderWord headerWord) {
  CODE_COVERAGE(2); // Hit
  return headerWord & 0xFFF;
}

TeError mvm_restore(mvm_VM** result, MVM_PROGMEM_P pBytecode, size_t bytecodeSize, void* context, mvm_TfResolveImport resolveImport) {
  CODE_COVERAGE(3); // Hit
  mvm_TfHostFunction* resolvedImports;
  mvm_TfHostFunction* resolvedImport;
  uint16_t* dataMemory;
  MVM_PROGMEM_P pImportTableStart;
  MVM_PROGMEM_P pImportTableEnd;
  MVM_PROGMEM_P pImportTableEntry;
  BO_t initialDataOffset;
  BO_t initialHeapOffset;
  uint16_t initialDataSize;
  uint16_t initialHeapSize;

  #if MVM_SAFE_MODE
    uint16_t x = 0x4243;
    bool isLittleEndian = ((uint8_t*)&x)[0] == 0x43;
    VM_ASSERT(NULL, isLittleEndian);
  #endif
  // TODO(low): CRC validation on input code

  TeError err = MVM_E_SUCCESS;
  VM* vm = NULL;

  // Bytecode size field is located at the second word
  if (bytecodeSize < 4) {
    CODE_COVERAGE_UNTESTED(21); // Not hit
    return MVM_E_INVALID_BYTECODE;
  }
  uint16_t expectedBytecodeSize = VM_READ_BC_2_HEADER_FIELD(bytecodeSize, pBytecode);
  if (bytecodeSize != expectedBytecodeSize) {
    CODE_COVERAGE_UNTESTED(240); // Not hit
    return MVM_E_INVALID_BYTECODE;
  }
  uint8_t headerSize = VM_READ_BC_1_HEADER_FIELD(headerSize, pBytecode);
  if (bytecodeSize < headerSize) {
    CODE_COVERAGE_UNTESTED(241); // Not hit
    return MVM_E_INVALID_BYTECODE;
  }
  // For the moment we expect an exact header size
  if (headerSize != sizeof (mvm_TsBytecodeHeader)) {
    CODE_COVERAGE_UNTESTED(242); // Not hit
    return MVM_E_INVALID_BYTECODE;
  }

  uint8_t bytecodeVersion = VM_READ_BC_1_HEADER_FIELD(bytecodeVersion, pBytecode);
  if (bytecodeVersion != VM_BYTECODE_VERSION) {
    CODE_COVERAGE_UNTESTED(430); // Not hit
    return MVM_E_INVALID_BYTECODE;
  }

  uint16_t dataMemorySize = VM_READ_BC_2_HEADER_FIELD(dataMemorySize, pBytecode);
  uint16_t importTableOffset = VM_READ_BC_2_HEADER_FIELD(importTableOffset, pBytecode);
  uint16_t importTableSize = VM_READ_BC_2_HEADER_FIELD(importTableSize, pBytecode);

  uint16_t importCount = importTableSize / sizeof (vm_TsImportTableEntry);

  size_t allocationSize = sizeof(mvm_VM) +
    sizeof(mvm_TfHostFunction) * importCount +  // Import table
    dataMemorySize; // Data memory (globals)
  vm = malloc(allocationSize);
  if (!vm) {
    err = MVM_E_MALLOC_FAIL;
    goto LBL_EXIT;
  }
  #if MVM_SAFE_MODE
    memset(vm, 0, allocationSize);
  #else
    memset(vm, 0, sizeof (mvm_VM));
  #endif
  resolvedImports = vm_getResolvedImports(vm);
  vm->context = context;
  vm->pBytecode = pBytecode;
  vm->dataMemory = (void*)(resolvedImports + importCount);
  vm->uniqueStrings = VM_VALUE_NULL;

  pImportTableStart = MVM_PROGMEM_P_ADD(pBytecode, importTableOffset);
  pImportTableEnd = MVM_PROGMEM_P_ADD(pImportTableStart, importTableSize);
  // Resolve imports (linking)
  resolvedImport = resolvedImports;
  pImportTableEntry = pImportTableStart;
  while (pImportTableEntry < pImportTableEnd) {
    CODE_COVERAGE(431); // Hit
    mvm_HostFunctionID hostFunctionID = MVM_READ_PROGMEM_2(pImportTableEntry);
    pImportTableEntry = MVM_PROGMEM_P_ADD(pImportTableEntry, sizeof (vm_TsImportTableEntry));
    mvm_TfHostFunction handler = NULL;
    err = resolveImport(hostFunctionID, context, &handler);
    if (err != MVM_E_SUCCESS) {
      CODE_COVERAGE_UNTESTED(432); // Not hit
      goto LBL_EXIT;
    }
    if (!handler) {
      CODE_COVERAGE_UNTESTED(433); // Not hit
      err = MVM_E_UNRESOLVED_IMPORT;
      goto LBL_EXIT;
    } else {
      CODE_COVERAGE(434); // Hit
    }
    *resolvedImport++ = handler;
  }

  // The GC is empty to start
  gc_freeGCMemory(vm);

  // Initialize data
  initialDataOffset = VM_READ_BC_2_HEADER_FIELD(initialDataOffset, pBytecode);
  initialDataSize = VM_READ_BC_2_HEADER_FIELD(initialDataSize, pBytecode);
  dataMemory = vm->dataMemory;
  VM_ASSERT(vm, initialDataSize <= dataMemorySize);
  VM_READ_BC_N_AT(dataMemory, initialDataOffset, initialDataSize, pBytecode);

  // Initialize heap
  initialHeapOffset = VM_READ_BC_2_HEADER_FIELD(initialHeapOffset, pBytecode);
  initialHeapSize = VM_READ_BC_2_HEADER_FIELD(initialHeapSize, pBytecode);
  if (initialHeapSize) {
    CODE_COVERAGE(435); // Hit
    gc_createNextBucket(vm, initialHeapSize);
    VM_ASSERT(vm, !vm->pLastBucket->prev); // Only one bucket
    uint8_t* heapStart = vm->pAllocationCursor;
    VM_READ_BC_N_AT(heapStart, initialHeapOffset, initialHeapSize, pBytecode);
    vm->vpAllocationCursor += initialHeapSize;
    vm->pAllocationCursor += initialHeapSize;
  } else {
    CODE_COVERAGE_UNTESTED(436); // Not hit
  }

LBL_EXIT:
  if (err != MVM_E_SUCCESS) {
    CODE_COVERAGE_UNTESTED(437); // Not hit
    *result = NULL;
    if (vm) {
      free(vm);
      vm = NULL;
    } else {
      CODE_COVERAGE_UNTESTED(438); // Not hit
    }
  } else {
    CODE_COVERAGE(439); // Hit
  }
  *result = vm;
  return err;
}

void* mvm_getContext(VM* vm) {
  return vm->context;
}

static const Value smallLiterals[] = {
  /* VM_SLV_NULL */         VM_VALUE_NULL,
  /* VM_SLV_UNDEFINED */    VM_VALUE_UNDEFINED,
  /* VM_SLV_FALSE */        VM_VALUE_FALSE,
  /* VM_SLV_TRUE */         VM_VALUE_TRUE,
  /* VM_SLV_INT_0 */        VM_TAG_INT | 0,
  /* VM_SLV_INT_1 */        VM_TAG_INT | 1,
  /* VM_SLV_INT_2 */        VM_TAG_INT | 2,
  /* VM_SLV_INT_MINUS_1 */  VM_TAG_INT | ((uint16_t)(-1) & VM_VALUE_MASK),
};
#define smallLiteralsSize (sizeof smallLiterals / sizeof smallLiterals[0])


static TeError vm_run(VM* vm) {
  CODE_COVERAGE(4); // Hit

  #define CACHE_REGISTERS() do { \
    programCounter = MVM_PROGMEM_P_ADD(pBytecode, reg->programCounter); \
    argCount = reg->argCount; \
    pFrameBase = reg->pFrameBase; \
    pStackPointer = reg->pStackPointer; \
  } while (false)

  #define FLUSH_REGISTER_CACHE() do { \
    reg->programCounter = (BO_t)MVM_PROGMEM_P_SUB(programCounter, pBytecode); \
    reg->argCount = argCount; \
    reg->pFrameBase = pFrameBase; \
    reg->pStackPointer = pStackPointer; \
  } while (false)

  // TODO: This macro just adds extra layers of checks, since the result is
  // typically used in another if statement, even though we've just come out of
  // an if statement on the same condition.
  #define VALUE_TO_BOOL(result, value) do { \
    if (VM_IS_INT14(value)) result = value != 0; \
    else if (value == VM_VALUE_TRUE) result = true; \
    else if (value == VM_VALUE_FALSE) result = false; \
    else result = mvm_toBool(vm, value); \
  } while (false)

  #define READ_PGM_1(target) do { \
    target = MVM_READ_PROGMEM_1(programCounter);\
    programCounter = MVM_PROGMEM_P_ADD(programCounter, 1); \
  } while (false)

  #define READ_PGM_2(target) do { \
    target = MVM_READ_PROGMEM_2(programCounter); \
    programCounter = MVM_PROGMEM_P_ADD(programCounter, 2); \
  } while (false)

  // Reinterpret reg1 as 8-bit signed
  #define SIGN_EXTEND_REG_1() reg1 = (uint16_t)((int16_t)((int8_t)reg1))

  #define PUSH(v) *(pStackPointer++) = v
  #define POP() (*(--pStackPointer))
  #define INSTRUCTION_RESERVED() VM_ASSERT(vm, false)

  VM_SAFE_CHECK_NOT_NULL(vm);
  VM_SAFE_CHECK_NOT_NULL(vm->stack);


  // TODO(low): I'm not sure that these variables should be cached for the whole duration of vm_run rather than being calculated on demand
  vm_TsRegisters* reg = &vm->stack->reg;
  uint16_t* bottomOfStack = VM_BOTTOM_OF_STACK(vm);
  MVM_PROGMEM_P pBytecode = vm->pBytecode;
  uint16_t* dataMemory = vm->dataMemory;
  TeError err = MVM_E_SUCCESS;

  uint16_t* pFrameBase;
  uint16_t argCount; // Of active function
  register MVM_PROGMEM_P programCounter;
  register uint16_t* pStackPointer;
  register uint16_t reg1 = 0;
  register uint16_t reg2 = 0;
  register uint16_t reg3 = 0;

  CACHE_REGISTERS();

  VM_EXEC_SAFE_MODE(
    uint16_t bytecodeSize = VM_READ_BC_2_HEADER_FIELD(bytecodeSize, vm->pBytecode);
    uint16_t stringTableOffset = VM_READ_BC_2_HEADER_FIELD(stringTableOffset, vm->pBytecode);
    uint16_t stringTableSize = VM_READ_BC_2_HEADER_FIELD(stringTableSize, vm->pBytecode);

    // It's an implementation detail that no code starts before the end of the string table
    MVM_PROGMEM_P minProgramCounter = MVM_PROGMEM_P_ADD(vm->pBytecode, (stringTableOffset + stringTableSize));
    MVM_PROGMEM_P maxProgramCounter = MVM_PROGMEM_P_ADD(vm->pBytecode, bytecodeSize);
  )

// TODO(low): I think we need unit tests that explicitly test that every
// instruction is implemented and has the correct behavior. I'm thinking the
// way to do this would be to just replace all operation implementation with
// some kind of abort, and then progressively re-enable the individually when
// test cases hit them.

// This forms the start of the run loop
LBL_DO_NEXT_INSTRUCTION:
  CODE_COVERAGE(59); // Hit
  // Instruction bytes are divided into two nibbles
  READ_PGM_1(reg3);
  reg1 = reg3 & 0xF;
  reg3 = reg3 >> 4;

  if (reg3 >= VM_OP_DIVIDER_1) {
    CODE_COVERAGE(428); // Hit
    reg2 = POP();
  } else {
    CODE_COVERAGE(429); // Hit
  }

  VM_ASSERT(vm, reg3 < VM_OP_END);
  MVM_SWITCH_CONTIGUOUS(reg3, (VM_OP_END - 1)) {

/* ------------------------------------------------------------------------- */
/*                         VM_OP_LOAD_SMALL_LITERAL                          */
/*   Expects:                                                                */
/*     reg1: small literal ID                                                */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS(VM_OP_LOAD_SMALL_LITERAL): {
      CODE_COVERAGE(60); // Hit

      TABLE_COVERAGE(reg1, smallLiteralsSize, 448); // Hit 7/8
      if (reg1 < smallLiteralsSize) {
        reg1 = smallLiterals[reg1];
      }
      else {
        VM_INVALID_BYTECODE(vm);
      }

      goto LBL_TAIL_PUSH_REG1;
    }

/* ------------------------------------------------------------------------- */
/*                             VM_OP_LOAD_VAR_1                              */
/*   Expects:                                                                */
/*     reg1: variable index                                                  */
/* ------------------------------------------------------------------------- */
// TODO: Consolidate

    MVM_CASE_CONTIGUOUS (VM_OP_LOAD_VAR_1):
      CODE_COVERAGE(61); // Hit
      reg1 = pStackPointer[-reg1 - 1];
      goto LBL_TAIL_PUSH_REG1;

/* ------------------------------------------------------------------------- */
/*                            VM_OP_LOAD_GLOBAL_1                            */
/*   Expects:                                                                */
/*     reg1: variable index                                                  */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP_LOAD_GLOBAL_1):
    LBL_OP_LOAD_GLOBAL:
      CODE_COVERAGE(62); // Hit
      reg1 = dataMemory[reg1];
      goto LBL_TAIL_PUSH_REG1;

/* ------------------------------------------------------------------------- */
/*                             VM_OP_LOAD_ARG_1                              */
/*   Expects:                                                                */
/*     reg1: argument index                                                  */
/* ------------------------------------------------------------------------- */
// TODO: Consolidate

    MVM_CASE_CONTIGUOUS (VM_OP_LOAD_ARG_1):
      CODE_COVERAGE(63); // Hit
      if (reg1 < argCount) {
        CODE_COVERAGE(64); // Hit
        reg1 = pFrameBase[-3 - (int16_t)argCount + reg1];
      } else{
        CODE_COVERAGE_UNTESTED(65); // Not hit
        reg1 = VM_VALUE_UNDEFINED;
      }
      goto LBL_TAIL_PUSH_REG1;

/* ------------------------------------------------------------------------- */
/*                               VM_OP_CALL_1                                */
/*   Expects:                                                                */
/*     reg1: index into short-call table                                     */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP_CALL_1): {
      CODE_COVERAGE_UNTESTED(66); // Not hit
      BO_t shortCallTableOffset = VM_READ_BC_2_HEADER_FIELD(shortCallTableOffset, pBytecode);
      MVM_PROGMEM_P shortCallTableEntry = MVM_PROGMEM_P_ADD(pBytecode, shortCallTableOffset + reg1 * sizeof (vm_TsShortCallTableEntry));

      #if MVM_SAFE_MODE
        uint16_t shortCallTableSize = VM_READ_BC_2_HEADER_FIELD(shortCallTableOffset, pBytecode);
        MVM_PROGMEM_P shortCallTableEnd = MVM_PROGMEM_P_ADD(pBytecode, shortCallTableOffset + shortCallTableSize);
        VM_ASSERT(vm, shortCallTableEntry < shortCallTableEnd);
      #endif

      uint16_t tempFunction = MVM_READ_PROGMEM_2(shortCallTableEntry);
      shortCallTableEntry = MVM_PROGMEM_P_ADD(shortCallTableEntry, 2);
      uint8_t tempArgCount = MVM_READ_PROGMEM_1(shortCallTableEntry);

      // The high bit of function indicates if this is a call to the host
      bool isHostCall = tempFunction & 0x8000;
      tempFunction = tempFunction & 0x7FFF;

      reg1 = tempArgCount;

      if (isHostCall) {
        CODE_COVERAGE_UNTESTED(67); // Not hit
        reg2 = tempFunction;
        goto LBL_CALL_HOST_COMMON;
      } else {
        CODE_COVERAGE_UNTESTED(68); // Not hit
        reg2 = tempFunction;
        goto LBL_CALL_COMMON;
      }
    }

/* ------------------------------------------------------------------------- */
/*                             VM_OP_EXTENDED_1                              */
/*   Expects:                                                                */
/*     reg1: vm_TeOpcodeEx1                                                  */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP_EXTENDED_1):
      CODE_COVERAGE(69); // Hit
      goto LBL_OP_EXTENDED_1;

/* ------------------------------------------------------------------------- */
/*                             VM_OP_EXTENDED_2                              */
/*   Expects:                                                                */
/*     reg1: vm_TeOpcodeEx2                                                  */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP_EXTENDED_2):
      CODE_COVERAGE(70); // Hit
      goto LBL_OP_EXTENDED_2;

/* ------------------------------------------------------------------------- */
/*                             VM_OP_EXTENDED_3                              */
/*   Expects:                                                                */
/*     reg1: vm_TeOpcodeEx3                                                  */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP_EXTENDED_3):
      CODE_COVERAGE(71); // Hit
      goto LBL_OP_EXTENDED_3;

/* ------------------------------------------------------------------------- */
/*                                VM_OP_POP                                  */
/*   Expects:                                                                */
/*     reg1: pop count - 1                                                   */
/*     reg2: unused value already popped off the stack                       */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP_POP): {
      CODE_COVERAGE(72); // Hit
      pStackPointer -= reg1;
      goto LBL_DO_NEXT_INSTRUCTION;
    }

/* ------------------------------------------------------------------------- */
/*                             VM_OP_STORE_VAR_1                             */
/*   Expects:                                                                */
/*     reg1: variable index                                                  */
/*     reg2: value to store                                                  */
/* ------------------------------------------------------------------------- */
// TODO: Consolidate
    MVM_CASE_CONTIGUOUS (VM_OP_STORE_VAR_1): {
      CODE_COVERAGE_UNTESTED(73); // Not hit
      pStackPointer[-reg1 - 2] = reg2;
      goto LBL_DO_NEXT_INSTRUCTION;
    }

/* ------------------------------------------------------------------------- */
/*                           VM_OP_STORE_GLOBAL_1                            */
/*   Expects:                                                                */
/*     reg1: variable index                                                  */
/*     reg2: value to store                                                  */
/* ------------------------------------------------------------------------- */
// TODO: Consolidate

    MVM_CASE_CONTIGUOUS (VM_OP_STORE_GLOBAL_1): {
      CODE_COVERAGE_UNTESTED(74); // Not hit
      dataMemory[reg1] = reg2;
      goto LBL_DO_NEXT_INSTRUCTION;
    }

/* ------------------------------------------------------------------------- */
/*                            VM_OP_STRUCT_GET_1                             */
/*   Expects:                                                                */
/*     reg1: field index                                                     */
/*     reg2: struct reference                                                */
/* ------------------------------------------------------------------------- */
// TODO: Consolidate

    MVM_CASE_CONTIGUOUS (VM_OP_STRUCT_GET_1): {
      CODE_COVERAGE_UNTESTED(75); // Not hit
      INSTRUCTION_RESERVED();
      goto LBL_DO_NEXT_INSTRUCTION;
    }

/* ------------------------------------------------------------------------- */
/*                            VM_OP_STRUCT_SET_1                             */
/*   Expects:                                                                */
/*     reg1: field index                                                     */
/*     reg2: value to store                                                  */
/* ------------------------------------------------------------------------- */
// TODO: Consolidate
    MVM_CASE_CONTIGUOUS (VM_OP_STRUCT_SET_1): {
      CODE_COVERAGE_UNTESTED(76); // Not hit
      INSTRUCTION_RESERVED();
      goto LBL_DO_NEXT_INSTRUCTION;
    }

/* ------------------------------------------------------------------------- */
/*                              VM_OP_NUM_OP                                 */
/*   Expects:                                                                */
/*     reg1: vm_TeNumberOp                                                   */
/*     reg2: first popped operand                                            */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP_NUM_OP): {
    LBL_OP_NUM_OP:
      CODE_COVERAGE(77); // Hit

      int32_t reg1I = 0;
      int32_t reg2I = 0;

      reg3 = reg1;

      // If it's a binary operator, then we pop a second operand
      if (reg3 < VM_NUM_OP_DIVIDER) {
        CODE_COVERAGE(440); // Hit
        reg1 = POP();

        if (toInt32Internal(vm, reg1, &reg1I) != MVM_E_SUCCESS) {
          CODE_COVERAGE(444); // Hit
          goto LBL_NUM_OP_FLOAT64;
        } else {
          CODE_COVERAGE(445); // Hit
        }
      } else {
        CODE_COVERAGE(441); // Hit
        reg1 = 0;
      }

      // Convert second operand to a float
      if (toInt32Internal(vm, reg2, &reg2I) != MVM_E_SUCCESS) {
        CODE_COVERAGE(442); // Hit
        goto LBL_NUM_OP_FLOAT64;
      } else {
        CODE_COVERAGE(443); // Hit
      }

      VM_ASSERT(vm, reg3 < VM_NUM_OP_END);
      MVM_SWITCH_CONTIGUOUS (reg3, (VM_NUM_OP_END - 1)) {
        MVM_CASE_CONTIGUOUS(VM_NUM_OP_LESS_THAN): {
          CODE_COVERAGE(78); // Hit
          reg1 = reg1I < reg2I;
          goto LBL_TAIL_PUSH_REG1_BOOL;
        }
        MVM_CASE_CONTIGUOUS(VM_NUM_OP_GREATER_THAN): {
          CODE_COVERAGE(79); // Hit
          reg1 = reg1I > reg2I;
          goto LBL_TAIL_PUSH_REG1_BOOL;
        }
        MVM_CASE_CONTIGUOUS(VM_NUM_OP_LESS_EQUAL): {
          CODE_COVERAGE(80); // Hit
          reg1 = reg1I <= reg2I;
          goto LBL_TAIL_PUSH_REG1_BOOL;
        }
        MVM_CASE_CONTIGUOUS(VM_NUM_OP_GREATER_EQUAL): {
          CODE_COVERAGE(81); // Hit
          reg1 = reg1I >= reg2I;
          goto LBL_TAIL_PUSH_REG1_BOOL;
        }
        MVM_CASE_CONTIGUOUS(VM_NUM_OP_ADD_NUM): {
          CODE_COVERAGE(82); // Hit
          #if __has_builtin(__builtin_add_overflow)
            if (__builtin_add_overflow(reg1I, reg2I, &reg1I)) {
              goto LBL_NUM_OP_FLOAT64;
            }
          #elif MVM_PORT_INT32_OVERFLOW_CHECKS
            int32_t result = reg1I + reg2I;
            // Check overflow https://blog.regehr.org/archives/1139
            if (((reg1I ^ result) & (reg2I ^ result)) < 0) goto LBL_NUM_OP_FLOAT64;
            reg1I = result;
          #else
            reg1I = reg1I + reg2I;
          #endif
          break;
        }
        MVM_CASE_CONTIGUOUS(VM_NUM_OP_SUBTRACT): {
          CODE_COVERAGE(83); // Hit
          #if __has_builtin(__builtin_sub_overflow)
            if (__builtin_sub_overflow(reg1I, reg2I, &reg1I)) {
              goto LBL_NUM_OP_FLOAT64;
            }
          #elif MVM_PORT_INT32_OVERFLOW_CHECKS
            reg2I = -reg2I;
            int32_t result = reg1I + reg2I;
            // Check overflow https://blog.regehr.org/archives/1139
            if (((reg1I ^ result) & (reg2I ^ result)) < 0) goto LBL_NUM_OP_FLOAT64;
            reg1I = result;
          #else
            reg1I = reg1I - reg2I;
          #endif
          break;
        }
        MVM_CASE_CONTIGUOUS(VM_NUM_OP_MULTIPLY): {
          CODE_COVERAGE(84); // Hit
          #if __has_builtin(__builtin_mul_overflow)
            if (__builtin_mul_overflow(reg1I, reg2I, &reg1I)) {
              goto LBL_NUM_OP_FLOAT64;
            }
          #elif MVM_PORT_INT32_OVERFLOW_CHECKS
            // There isn't really an efficient way to determine multiplied
            // overflow on embedded devices without accessing the hardware
            // status registers. The fast shortcut here is to just assume that
            // anything more than 14-bit multiplication could overflow a 32-bit
            // integer.
            if (VM_IS_INT14(reg1) && VM_IS_INT14(reg2)) {
              reg1I = reg1I * reg2I;
            } else {
              goto LBL_NUM_OP_FLOAT64;
            }
          #else
            reg1I = reg1I * reg2I;
          #endif
          break;
        }
        MVM_CASE_CONTIGUOUS(VM_NUM_OP_DIVIDE): {
          CODE_COVERAGE(85); // Hit
          // With division, we leave it up to the user to write code that
          // performs integer division instead of floating point division, so
          // this instruction is always the case where they're doing floating
          // point division.
          goto LBL_NUM_OP_FLOAT64;
          break;
        }
        MVM_CASE_CONTIGUOUS(VM_NUM_OP_DIVIDE_AND_TRUNC): {
          CODE_COVERAGE(86); // Hit
          if (reg2I == 0) {
            reg1I = 0;
            break;
          }
          reg1I = reg1I / reg2I;
          break;
        }
        MVM_CASE_CONTIGUOUS(VM_NUM_OP_REMAINDER): {
          CODE_COVERAGE(87); // Hit
          if (reg2I == 0) {
            CODE_COVERAGE(26); // Hit
            reg1 = VM_VALUE_NAN;
            goto LBL_TAIL_PUSH_REG1;
          }
          CODE_COVERAGE(90); // Hit
          reg1I = reg1I % reg2I;
          break;
        }
        MVM_CASE_CONTIGUOUS(VM_NUM_OP_POWER): {
          CODE_COVERAGE_UNTESTED(88); // Not hit
          VM_NOT_IMPLEMENTED(vm);
          break;
        }
        MVM_CASE_CONTIGUOUS(VM_NUM_OP_NEGATE): {
          CODE_COVERAGE(89); // Hit
          #if MVM_PORT_INT32_OVERFLOW_CHECKS
          // Note: Zero negates to negative zero, which is not representable as an int32
          if ((reg2I == INT32_MIN) || (reg2I == 0)) goto LBL_NUM_OP_FLOAT64;
          #endif
          reg1I = -reg2I;
          break;
        }
        MVM_CASE_CONTIGUOUS(VM_NUM_OP_UNARY_PLUS): {
          reg1I = reg2I;
          break;
        }
      } // End of switch vm_TeNumberOp for int32

      // Convert the result from a 32-bit integer
      reg1 = mvm_newInt32(vm, reg1I);
      goto LBL_TAIL_PUSH_REG1;
    } // End of case VM_OP_NUM_OP

/* ------------------------------------------------------------------------- */
/*                              VM_OP_BIT_OP                                 */
/*   Expects:                                                                */
/*     reg1: vm_TeBitwiseOp                                                  */
/*     reg2: first popped operand                                            */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP_BIT_OP): {
      CODE_COVERAGE_UNTESTED(92); // Not hit
      reg3 = reg1;

      // If it's a binary operator, then we pop a second operand
      if (reg3 < VM_BIT_OP_DIVIDER)
        reg1 = POP();

      VM_ASSERT(vm, reg3 < VM_BIT_OP_END);
      MVM_SWITCH_CONTIGUOUS (reg3, (VM_BIT_OP_END - 1)) {
        MVM_CASE_CONTIGUOUS(VM_BIT_OP_SHR_ARITHMETIC): {
          CODE_COVERAGE_UNTESTED(93); // Not hit
          VM_NOT_IMPLEMENTED(vm);
          break;
        }
        MVM_CASE_CONTIGUOUS(VM_BIT_OP_SHR_BITWISE): {
          CODE_COVERAGE_UNTESTED(94); // Not hit
          VM_NOT_IMPLEMENTED(vm);
          break;
        }
        MVM_CASE_CONTIGUOUS(VM_BIT_OP_SHL): {
          CODE_COVERAGE_UNTESTED(95); // Not hit
          VM_NOT_IMPLEMENTED(vm);
          break;
        }
        MVM_CASE_CONTIGUOUS(VM_BIT_OP_OR): {
          CODE_COVERAGE_UNTESTED(96); // Not hit
          VM_NOT_IMPLEMENTED(vm);
          break;
        }
        MVM_CASE_CONTIGUOUS(VM_BIT_OP_AND): {
          CODE_COVERAGE_UNTESTED(97); // Not hit
          VM_NOT_IMPLEMENTED(vm);
          break;
        }
        MVM_CASE_CONTIGUOUS(VM_BIT_OP_XOR): {
          CODE_COVERAGE_UNTESTED(98); // Not hit
          VM_NOT_IMPLEMENTED(vm);
          break;
        }
        MVM_CASE_CONTIGUOUS(VM_BIT_OP_NOT): {
          CODE_COVERAGE_UNTESTED(99); // Not hit
          VM_NOT_IMPLEMENTED(vm);
          break;
        }
        MVM_CASE_CONTIGUOUS(VM_BIT_OP_OR_ZERO): {
          CODE_COVERAGE_UNTESTED(100); // Not hit
          VM_NOT_IMPLEMENTED(vm);
          break;
        }
      }

      CODE_COVERAGE_UNTESTED(101); // Not hit
      VM_NOT_IMPLEMENTED(vm); break;
    } // End of case VM_OP_BIT_OP

  } // End of primary switch

// All cases should loop explicitly back
VM_ASSERT_UNREACHABLE(vm);

/* ------------------------------------------------------------------------- */
/*                             LBL_OP_EXTENDED_1                             */
/*   Expects:                                                                */
/*     reg1: vm_TeOpcodeEx1                                                  */
/* ------------------------------------------------------------------------- */

LBL_OP_EXTENDED_1: {
  CODE_COVERAGE(102); // Hit

  reg3 = reg1;

  if (reg3 >= VM_OP1_DIVIDER_1) {
    CODE_COVERAGE(103); // Hit
    reg2 = POP();
    reg1 = POP();
  } else {
    CODE_COVERAGE(104); // Hit
  }

  VM_ASSERT(vm, reg3 <= VM_OP1_END);
  MVM_SWITCH_CONTIGUOUS (reg3, VM_OP1_END - 1) {

/* ------------------------------------------------------------------------- */
/*                              VM_OP1_RETURN_x                              */
/*   Expects: -                                                              */
/*     reg1: vm_TeOpcodeEx1                                                  */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP1_RETURN_1):
    MVM_CASE_CONTIGUOUS (VM_OP1_RETURN_2):
    MVM_CASE_CONTIGUOUS (VM_OP1_RETURN_3):
    MVM_CASE_CONTIGUOUS (VM_OP1_RETURN_4): {
      CODE_COVERAGE(105); // Hit
      // reg2 is used for the result
      if (reg1 & VM_RETURN_FLAG_UNDEFINED) {
        CODE_COVERAGE_UNTESTED(106); // Not hit
        reg2 = VM_VALUE_UNDEFINED;
      } else {
        CODE_COVERAGE(107); // Hit
        reg2 = POP();
      }

      // reg3 is the original arg count
      reg3 = argCount;

      // Pop variables/parameters
      pStackPointer = pFrameBase;

      // Restore caller state
      programCounter = MVM_PROGMEM_P_ADD(pBytecode, POP());
      argCount = POP();
      pFrameBase = bottomOfStack + POP();

      // Pop arguments
      pStackPointer -= reg3;
      // Pop function reference
      if (reg1 & VM_RETURN_FLAG_POP_FUNCTION) {
        CODE_COVERAGE(108); // Hit
        (void)POP();
      } else {
        CODE_COVERAGE_UNTESTED(109); // Not hit
      }

      // Push result
      PUSH(reg2);

      if (programCounter == pBytecode) {
        CODE_COVERAGE(110); // Hit
        goto LBL_EXIT;
      } else {
        CODE_COVERAGE(111); // Hit
      }
      goto LBL_DO_NEXT_INSTRUCTION;
    }

/* ------------------------------------------------------------------------- */
/*                              VM_OP1_OBJECT_NEW                            */
/*   Expects: -                                                              */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP1_OBJECT_NEW): {
      CODE_COVERAGE(112); // Hit
      TsPropertyList* pObject;
      reg1 = gc_allocateWithHeader(vm, sizeof (TsPropertyList), TC_REF_PROPERTY_LIST, sizeof (TsPropertyList), (void**)&pObject);
      pObject->first = 0;
      goto LBL_TAIL_PUSH_REG1;
    }

/* ------------------------------------------------------------------------- */
/*                               VM_OP1_LOGICAL_NOT                          */
/*   Expects: -                                                              */
/*     reg1: erroneously popped value                                        */
/*     reg2: value to operate on (popped from stack)                         */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP1_LOGICAL_NOT): {
      CODE_COVERAGE_UNTESTED(113); // Not hit
      // This operation is grouped as a binary operation, but it actually
      // only uses one operand, so we need to push the other back onto the
      // stack.
      PUSH(reg1);
      bool b;
      VALUE_TO_BOOL(b, reg2);
      reg1 = b ? VM_VALUE_FALSE : VM_VALUE_TRUE;
      goto LBL_TAIL_PUSH_REG1;
    }

/* ------------------------------------------------------------------------- */
/*                              VM_OP1_OBJECT_GET_1                          */
/*   Expects: -                                                              */
/*     reg1: objectValue                                                     */
/*     reg2: propertyName                                                    */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP1_OBJECT_GET_1): {
      CODE_COVERAGE(114); // Hit
      Value propValue;
      err = getProperty(vm, reg1, reg2, &propValue);
      reg1 = propValue;
      if (err != MVM_E_SUCCESS) goto LBL_EXIT;
      goto LBL_TAIL_PUSH_REG1;
    }

/* ------------------------------------------------------------------------- */
/*                                 VM_OP1_ADD                                */
/*   Expects: -                                                              */
/*     reg1: left operand                                                    */
/*     reg2: right operand                                                   */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP1_ADD): {
      CODE_COVERAGE(115); // Hit
      // Special case for adding unsigned 12 bit numbers, for example in most
      // loops. 12 bit unsigned addition does not require any overflow checks
      if (((reg1 & 0xF000) == 0) && ((reg2 & 0xF000) == 0)) {
        CODE_COVERAGE(116); // Hit
        reg1 = reg1 + reg2;
        goto LBL_TAIL_PUSH_REG1;
      } else {
        CODE_COVERAGE(119); // Hit
      }
      if (vm_isString(vm, reg1) || vm_isString(vm, reg2)) {
        CODE_COVERAGE(120); // Hit
        reg1 = vm_convertToString(vm, reg1);
        reg2 = vm_convertToString(vm, reg2);
        reg1 = vm_concat(vm, reg1, reg2);
        goto LBL_TAIL_PUSH_REG1;
      } else {
        CODE_COVERAGE(121); // Hit
        // Interpret like any of the other numeric operations
        PUSH(reg1);
        reg1 = VM_NUM_OP_ADD_NUM;
        goto LBL_OP_NUM_OP;
      }
    }

/* ------------------------------------------------------------------------- */
/*                                 VM_OP1_EQUAL                              */
/*   Expects: -                                                              */
/*     reg1: left operand                                                    */
/*     reg2: right operand                                                   */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP1_EQUAL): {
      CODE_COVERAGE_UNTESTED(122); // Not hit
      if(mvm_equal(vm, reg1, reg2)) {
        CODE_COVERAGE_UNTESTED(483); // Not hit
        reg1 = VM_VALUE_TRUE;
      } else {
        CODE_COVERAGE_UNTESTED(484); // Not hit
        reg1 = VM_VALUE_FALSE;
      }
      goto LBL_TAIL_PUSH_REG1;
    }

/* ------------------------------------------------------------------------- */
/*                                 VM_OP1_NOT_EQUAL                          */
/*   Expects: -                                                              */
/*     reg1: left operand                                                    */
/*     reg2: right operand                                                   */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP1_NOT_EQUAL): {
      if(mvm_equal(vm, reg1, reg2)) {
        CODE_COVERAGE_UNTESTED(123); // Not hit
        reg1 = VM_VALUE_FALSE;
      } else {
        CODE_COVERAGE_UNTESTED(485); // Not hit
        reg1 = VM_VALUE_TRUE;
      }
      goto LBL_DO_NEXT_INSTRUCTION;
    }

/* ------------------------------------------------------------------------- */
/*                                 VM_OP1_OBJECT_SET_1                       */
/*   Expects: -                                                              */
/*     reg1: property name                                                   */
/*     reg2: value                                                           */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP1_OBJECT_SET_1): {
      CODE_COVERAGE(124); // Hit
      reg3 = POP(); // object
      err = setProperty(vm, reg3, reg1, reg2);
      if (err != MVM_E_SUCCESS) {
        CODE_COVERAGE_UNTESTED(125); // Not hit
        goto LBL_EXIT;
      } else {
        CODE_COVERAGE(126); // Hit
      }
      goto LBL_DO_NEXT_INSTRUCTION;
    }

  } // End of VM_OP_EXTENDED_1 switch

  // All cases should jump to whatever tail they intend. Nothing should get here
  VM_ASSERT_UNREACHABLE(vm);

} // End of LBL_OP_EXTENDED_1

/* ------------------------------------------------------------------------- */
/*                             LBL_OP_EXTENDED_2                             */
/*   Expects:                                                                */
/*     reg1: vm_TeOpcodeEx2                                                  */
/* ------------------------------------------------------------------------- */

LBL_OP_EXTENDED_2: {
  CODE_COVERAGE(127); // Hit
  reg3 = reg1;

  // All the ex-2 instructions have an 8-bit parameter. This is stored in
  // reg1 for consistency with 4-bit and 16-bit literal modes
  READ_PGM_1(reg1);

  // Some operations pop an operand off the stack. This goes into reg2
  if (reg3 < VM_OP2_DIVIDER_1) {
    CODE_COVERAGE(128); // Hit
    reg2 = POP();
  } else {
    CODE_COVERAGE(129); // Hit
  }

  VM_ASSERT(vm, reg3 < VM_OP2_END);
  MVM_SWITCH_CONTIGUOUS (reg3, (VM_OP2_END - 1)) {

/* ------------------------------------------------------------------------- */
/*                             VM_OP2_BRANCH_1                              */
/*   Expects:                                                                */
/*     reg1: signed 8-bit offset to branch to, encoded in 16-bit unsigned    */
/*     reg2: condition to branch on                                          */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP2_BRANCH_1): {
      CODE_COVERAGE(130); // Hit
      SIGN_EXTEND_REG_1();
      goto LBL_BRANCH_COMMON;
    }

/* ------------------------------------------------------------------------- */
/*                             VM_OP2_STORE_ARG                             */
/*   Expects:                                                                */
/*     reg1: unsigned index of argument in which to store                    */
/*     reg2: value to store                                                  */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP2_STORE_ARG): {
      CODE_COVERAGE_UNTESTED(131); // Not hit
      VM_NOT_IMPLEMENTED(vm);
      goto LBL_DO_NEXT_INSTRUCTION;
    }

/* ------------------------------------------------------------------------- */
/*                             VM_OP2_STORE_GLOBAL_2                        */
/*   Expects:                                                                */
/*     reg1: unsigned index of global in which to store                      */
/*     reg2: value to store                                                  */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP2_STORE_GLOBAL_2): {
      CODE_COVERAGE_UNTESTED(132); // Not hit
      VM_NOT_IMPLEMENTED(vm);
      goto LBL_DO_NEXT_INSTRUCTION;
    }

/* ------------------------------------------------------------------------- */
/*                             VM_OP2_STORE_VAR_2                           */
/*   Expects:                                                                */
/*     reg1: unsigned index of variable in which to store                    */
/*     reg2: value to store                                                  */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP2_STORE_VAR_2): {
      CODE_COVERAGE_UNTESTED(133); // Not hit
      VM_NOT_IMPLEMENTED(vm);
      goto LBL_DO_NEXT_INSTRUCTION;
    }

/* ------------------------------------------------------------------------- */
/*                             VM_OP2_STRUCT_GET_2                          */
/*   Expects:                                                                */
/*     reg1: unsigned index of field                                         */
/*     reg2: reference to struct                                             */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP2_STRUCT_GET_2): {
      CODE_COVERAGE_UNTESTED(134); // Not hit
      VM_NOT_IMPLEMENTED(vm);
      goto LBL_DO_NEXT_INSTRUCTION;
    }

/* ------------------------------------------------------------------------- */
/*                             VM_OP2_STRUCT_SET_2                          */
/*   Expects:                                                                */
/*     reg1: unsigned index of field                                         */
/*     reg2: value to store                                                  */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP2_STRUCT_SET_2): {
      CODE_COVERAGE_UNTESTED(135); // Not hit
      VM_NOT_IMPLEMENTED(vm);
      goto LBL_DO_NEXT_INSTRUCTION;
    }

/* ------------------------------------------------------------------------- */
/*                             VM_OP2_JUMP_1                                */
/*   Expects:                                                                */
/*     reg1: signed 8-bit offset to branch to, encoded in 16-bit unsigned    */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP2_JUMP_1): {
      CODE_COVERAGE(136); // Hit
      SIGN_EXTEND_REG_1();
      goto LBL_JUMP_COMMON;
    }

/* ------------------------------------------------------------------------- */
/*                             VM_OP2_CALL_HOST                             */
/*   Expects:                                                                */
/*     reg1: arg count                                                       */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP2_CALL_HOST): {
      CODE_COVERAGE_UNTESTED(137); // Not hit
      // Function index is in reg2
      READ_PGM_1(reg2);
      goto LBL_CALL_HOST_COMMON;
    }

/* ------------------------------------------------------------------------- */
/*                             VM_OP2_CALL_3                                */
/*   Expects:                                                                */
/*     reg1: arg count                                                       */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP2_CALL_3): {
      CODE_COVERAGE(138); // Hit
      // The function was pushed before the arguments
      Value functionValue = pStackPointer[-reg1 - 1];

      // Functions can only be bytecode memory, so if it's not in bytecode then it's not a function
      if (!VM_IS_PGM_P(functionValue)) {
        CODE_COVERAGE_UNTESTED(139); // Not hit
        err = MVM_E_TARGET_NOT_CALLABLE;
        goto LBL_EXIT;
      } else {
        CODE_COVERAGE(140); // Hit
      }

      uint16_t headerWord = vm_readHeaderWord(vm, functionValue);
      TeTypeCode typeCode = vm_typeCodeFromHeaderWord(headerWord);
      if (typeCode == TC_REF_FUNCTION) {
        CODE_COVERAGE(141); // Hit
        VM_ASSERT(vm, VM_IS_PGM_P(functionValue));
        reg2 = VM_VALUE_OF(functionValue);
        goto LBL_CALL_COMMON;
      } else {
        CODE_COVERAGE(142); // Hit
      }

      if (typeCode == TC_REF_HOST_FUNC) {
        CODE_COVERAGE(143); // Hit
        reg2 = vm_readUInt16(vm, functionValue);
        goto LBL_CALL_HOST_COMMON;
      } else {
        CODE_COVERAGE_UNTESTED(144); // Not hit
      }

      err = MVM_E_TARGET_NOT_CALLABLE;
      goto LBL_EXIT;
    }

/* ------------------------------------------------------------------------- */
/*                             VM_OP2_CALL_2                                */
/*   Expects:                                                                */
/*     reg1: arg count                                                       */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP2_CALL_2): {
      CODE_COVERAGE_UNTESTED(145); // Not hit
      // Uses 16 bit literal for function offset
      READ_PGM_2(reg2);
      goto LBL_CALL_COMMON;
    }

/* ------------------------------------------------------------------------- */
/*                             VM_OP2_LOAD_GLOBAL_2                         */
/*   Expects:                                                                */
/*     reg1: unsigned global variable index                                  */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP2_LOAD_GLOBAL_2): {
      CODE_COVERAGE(146); // Hit
      goto LBL_OP_LOAD_GLOBAL;
    }

/* ------------------------------------------------------------------------- */
/*                              VM_OP2_LOAD_VAR_2                           */
/*   Expects:                                                                */
/*     reg1: unsigned variable index relative to stack pointer               */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP2_LOAD_VAR_2): {
      CODE_COVERAGE_UNTESTED(147); // Not hit
      VM_NOT_IMPLEMENTED(vm);
      goto LBL_DO_NEXT_INSTRUCTION;
    }

/* ------------------------------------------------------------------------- */
/*                              VM_OP2_LOAD_ARG_2                           */
/*   Expects:                                                                */
/*     reg1: unsigned variable index relative to stack pointer               */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP2_LOAD_ARG_2): {
      CODE_COVERAGE_UNTESTED(148); // Not hit
      VM_NOT_IMPLEMENTED(vm);
      goto LBL_DO_NEXT_INSTRUCTION;
    }

/* ------------------------------------------------------------------------- */
/*                              VM_OP2_RETURN_ERROR                         */
/*   Expects:                                                                */
/*     reg1: mvm_TeError                                                     */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP2_RETURN_ERROR): {
      CODE_COVERAGE_UNTESTED(149); // Not hit
      err = (TeError)reg1;
      goto LBL_EXIT;
    }

  } // End of vm_TeOpcodeEx2 switch

  // All cases should jump to whatever tail they intend. Nothing should get here
  VM_ASSERT_UNREACHABLE(vm);

} // End of LBL_OP_EXTENDED_2

/* ------------------------------------------------------------------------- */
/*                             LBL_OP_EXTENDED_3                             */
/*   Expects:                                                                */
/*     reg1: vm_TeOpcodeEx3                                                  */
/* ------------------------------------------------------------------------- */

LBL_OP_EXTENDED_3:  {
  CODE_COVERAGE(150); // Hit
  reg3 = reg1;

  // Ex-3 instructions have a 16-bit parameter
  READ_PGM_2(reg1);

  if (reg3 >= VM_OP3_DIVIDER_1) {
    CODE_COVERAGE(151); // Hit
    reg2 = POP();
  } else {
    CODE_COVERAGE(152); // Hit
  }

  VM_ASSERT(vm, reg3 < VM_OP3_END);
  MVM_SWITCH_CONTIGUOUS (reg3, (VM_OP3_END - 1)) {

/* ------------------------------------------------------------------------- */
/*                             VM_OP3_JUMP_2                                */
/*   Expects:                                                                */
/*     reg1: signed offset                                                   */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP3_JUMP_2): {
      CODE_COVERAGE(153); // Hit
      goto LBL_JUMP_COMMON;
    }

/* ------------------------------------------------------------------------- */
/*                             VM_OP3_LOAD_LITERAL                          */
/*   Expects:                                                                */
/*     reg1: literal value                                                   */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP3_LOAD_LITERAL): {
      CODE_COVERAGE(154); // Hit
      goto LBL_TAIL_PUSH_REG1;
    }

/* ------------------------------------------------------------------------- */
/*                             VM_OP3_LOAD_GLOBAL_3                         */
/*   Expects:                                                                */
/*     reg1: global variable index                                           */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP3_LOAD_GLOBAL_3): {
      CODE_COVERAGE_UNTESTED(155); // Not hit
      goto LBL_OP_LOAD_GLOBAL;
    }

/* ------------------------------------------------------------------------- */
/*                             VM_OP3_BRANCH_2                              */
/*   Expects:                                                                */
/*     reg1: signed offset                                                   */
/*     reg2: condition                                                       */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP3_BRANCH_2): {
      CODE_COVERAGE(156); // Hit
      goto LBL_BRANCH_COMMON;
    }

/* ------------------------------------------------------------------------- */
/*                             VM_OP3_STORE_GLOBAL_3                        */
/*   Expects:                                                                */
/*     reg1: global variable index                                           */
/*     reg2: condition                                                       */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP3_STORE_GLOBAL_3): {
      CODE_COVERAGE_UNTESTED(157); // Not hit
      VM_NOT_IMPLEMENTED(vm);
      goto LBL_DO_NEXT_INSTRUCTION;
    }

/* ------------------------------------------------------------------------- */
/*                             VM_OP3_OBJECT_GET_2                          */
/*   Expects:                                                                */
/*     reg1: property key value                                              */
/*     reg2: object value                                                    */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP3_OBJECT_GET_2): {
      CODE_COVERAGE_UNTESTED(158); // Not hit
      VM_NOT_IMPLEMENTED(vm);
      goto LBL_DO_NEXT_INSTRUCTION;
    }

/* ------------------------------------------------------------------------- */
/*                             VM_OP3_OBJECT_SET_2                          */
/*   Expects:                                                                */
/*     reg1: property key value                                              */
/*     reg2: value                                                           */
/* ------------------------------------------------------------------------- */

    MVM_CASE_CONTIGUOUS (VM_OP3_OBJECT_SET_2): {
      CODE_COVERAGE_UNTESTED(159); // Not hit
      VM_NOT_IMPLEMENTED(vm);
      goto LBL_DO_NEXT_INSTRUCTION;
    }

  } // End of vm_TeOpcodeEx3 switch
  // All cases should jump to whatever tail they intend. Nothing should get here
  VM_ASSERT_UNREACHABLE(vm);
} // End of LBL_OP_EXTENDED_3

/* ------------------------------------------------------------------------- */
/*                             LBL_BRANCH_COMMON                             */
/*   Expects:                                                                */
/*     reg1: signed 16-bit amount to jump by if the condition is truthy      */
/*     reg2: condition to branch on                                          */
/* ------------------------------------------------------------------------- */
LBL_BRANCH_COMMON: {
  CODE_COVERAGE(160); // Hit
  VALUE_TO_BOOL(reg2, reg2);
  if (reg2) programCounter = MVM_PROGMEM_P_ADD(programCounter, (int16_t)reg1);
  goto LBL_DO_NEXT_INSTRUCTION;
}

/* ------------------------------------------------------------------------- */
/*                             LBL_JUMP_COMMON                               */
/*   Expects:                                                                */
/*     reg1: signed 16-bit amount to jump by                                 */
/* ------------------------------------------------------------------------- */
LBL_JUMP_COMMON: {
  CODE_COVERAGE(161); // Hit
  programCounter = MVM_PROGMEM_P_ADD(programCounter, (int16_t)reg1);
  goto LBL_DO_NEXT_INSTRUCTION;
}



/* ------------------------------------------------------------------------- */
/*                          LBL_CALL_HOST_COMMON                             */
/*   Expects:                                                                */
/*     reg1: reg1: argument count                                            */
/*     reg2: index in import table                                           */
/* ------------------------------------------------------------------------- */
LBL_CALL_HOST_COMMON: {
  CODE_COVERAGE(162); // Hit
  // Save caller state
  PUSH(pFrameBase - bottomOfStack);
  PUSH(argCount);
  PUSH((uint16_t)MVM_PROGMEM_P_SUB(programCounter, pBytecode));

  // Set up new frame
  pFrameBase = pStackPointer;
  argCount = reg1;
  programCounter = pBytecode; // "null" (signifies that we're outside the VM)

  VM_ASSERT(vm, reg2 < vm_getResolvedImportCount(vm));
  mvm_TfHostFunction hostFunction = vm_getResolvedImports(vm)[reg2];
  Value result = VM_VALUE_UNDEFINED;
  Value* args = pStackPointer - 3 - reg1;

  uint16_t importTableOffset = VM_READ_BC_2_HEADER_FIELD(importTableOffset, pBytecode);

  uint16_t importTableEntry = importTableOffset + reg2 * sizeof (vm_TsImportTableEntry);
  mvm_HostFunctionID hostFunctionID = VM_READ_BC_2_AT(importTableEntry, pBytecode);

  FLUSH_REGISTER_CACHE();
  VM_ASSERT(vm, reg1 < 256);
  err = hostFunction(vm, hostFunctionID, &result, args, (uint8_t)reg1);
  if (err != MVM_E_SUCCESS) goto LBL_EXIT;
  CACHE_REGISTERS();

  // Restore caller state
  programCounter = MVM_PROGMEM_P_ADD(pBytecode, POP());
  argCount = POP();
  pFrameBase = bottomOfStack + POP();

  // Pop arguments
  pStackPointer -= reg1;

  // Pop function pointer
  (void)POP();
  // TODO(high): Not all host call operation will push the function
  // onto the stack, so it's invalid to just pop it here. A clean
  // solution may be to have a "flags" register which specifies things
  // about the current context, one of which will be whether the
  // function was called by pushing it onto the stack. This gets rid
  // of some of the different RETURN opcodes we have

  PUSH(result);
  goto LBL_DO_NEXT_INSTRUCTION;
} // End of LBL_CALL_HOST_COMMON

/* ------------------------------------------------------------------------- */
/*                             LBL_CALL_COMMON                               */
/*   Expects:                                                                */
/*     reg1: number of arguments                                             */
/*     reg2: offset of target function in bytecode                           */
/* ------------------------------------------------------------------------- */
LBL_CALL_COMMON: {
  CODE_COVERAGE(163); // Hit
  uint16_t programCounterToReturnTo = (uint16_t)MVM_PROGMEM_P_SUB(programCounter, pBytecode);
  programCounter = MVM_PROGMEM_P_ADD(pBytecode, reg2);

  uint8_t maxStackDepth;
  READ_PGM_1(maxStackDepth);
  if (pStackPointer + (maxStackDepth + VM_FRAME_SAVE_SIZE_WORDS) > VM_TOP_OF_STACK(vm)) {
    err = MVM_E_STACK_OVERFLOW;
    goto LBL_EXIT;
  }

  // Save caller state (VM_FRAME_SAVE_SIZE_WORDS)
  PUSH(pFrameBase - bottomOfStack);
  PUSH(argCount);
  PUSH(programCounterToReturnTo);

  // Set up new frame
  pFrameBase = pStackPointer;
  argCount = reg1;

  goto LBL_DO_NEXT_INSTRUCTION;
} // End of LBL_CALL_COMMON

/* ------------------------------------------------------------------------- */
/*                             LBL_NUM_OP_FLOAT64                            */
/*   Expects:                                                                */
/*     reg1: left operand (second pop), or zero for unary ops                */
/*     reg2: right operand (first pop), or single operand for unary ops      */
/*     reg3: vm_TeNumberOp                                                   */
/* ------------------------------------------------------------------------- */
LBL_NUM_OP_FLOAT64: {
  CODE_COVERAGE_UNIMPLEMENTED(447); // Hit

  // It's a little less efficient to convert 2 operands even for unary
  // operators, but this path is slow anyway and it saves on code space if we
  // don't check.
  MVM_FLOAT64 reg1F = mvm_toFloat64(vm, reg1);
  MVM_FLOAT64 reg2F = mvm_toFloat64(vm, reg2);

  VM_ASSERT(vm, reg3 < VM_NUM_OP_END);
  MVM_SWITCH_CONTIGUOUS (reg3, (VM_NUM_OP_END - 1)) {
    MVM_CASE_CONTIGUOUS(VM_NUM_OP_LESS_THAN): {
      CODE_COVERAGE(449); // Hit
      reg1 = reg1F < reg2F;
      goto LBL_TAIL_PUSH_REG1_BOOL;
    }
    MVM_CASE_CONTIGUOUS(VM_NUM_OP_GREATER_THAN): {
      CODE_COVERAGE(450); // Hit
      reg1 = reg1F > reg2F;
      goto LBL_TAIL_PUSH_REG1_BOOL;
    }
    MVM_CASE_CONTIGUOUS(VM_NUM_OP_LESS_EQUAL): {
      CODE_COVERAGE(451); // Hit
      reg1 = reg1F <= reg2F;
      goto LBL_TAIL_PUSH_REG1_BOOL;
    }
    MVM_CASE_CONTIGUOUS(VM_NUM_OP_GREATER_EQUAL): {
      CODE_COVERAGE(452); // Hit
      reg1 = reg1F >= reg2F;
      goto LBL_TAIL_PUSH_REG1_BOOL;
    }
    MVM_CASE_CONTIGUOUS(VM_NUM_OP_ADD_NUM): {
      CODE_COVERAGE(453); // Hit
      reg1F = reg1F + reg2F;
      break;
    }
    MVM_CASE_CONTIGUOUS(VM_NUM_OP_SUBTRACT): {
      CODE_COVERAGE(454); // Hit
      reg1F = reg1F - reg2F;
      break;
    }
    MVM_CASE_CONTIGUOUS(VM_NUM_OP_MULTIPLY): {
      CODE_COVERAGE(455); // Hit
      reg1F = reg1F * reg2F;
      break;
    }
    MVM_CASE_CONTIGUOUS(VM_NUM_OP_DIVIDE): {
      CODE_COVERAGE(456); // Hit
      reg1F = reg1F / reg2F;
      break;
    }
    MVM_CASE_CONTIGUOUS(VM_NUM_OP_DIVIDE_AND_TRUNC): {
      CODE_COVERAGE(457); // Hit
      reg1F = mvm_float64ToInt32((reg1F / reg2F));
      break;
    }
    MVM_CASE_CONTIGUOUS(VM_NUM_OP_REMAINDER): {
      CODE_COVERAGE(458); // Hit
      reg1F = fmod(reg1F, reg2F);
      break;
    }
    MVM_CASE_CONTIGUOUS(VM_NUM_OP_POWER): {
      CODE_COVERAGE_UNTESTED(459); // Not hit
      VM_NOT_IMPLEMENTED(vm);
      break;
    }
    MVM_CASE_CONTIGUOUS(VM_NUM_OP_NEGATE): {
      CODE_COVERAGE(460); // Hit
      reg1F = -reg2F;
      break;
    }
    MVM_CASE_CONTIGUOUS(VM_NUM_OP_UNARY_PLUS): {
      CODE_COVERAGE(461); // Hit
      reg1F = reg2F;
      break;
    }
  } // End of switch vm_TeNumberOp for float64

  // Convert the result from a float
  reg1 = mvm_newNumber(vm, reg1F);
  goto LBL_TAIL_PUSH_REG1;
} // End of LBL_NUM_OP_FLOAT64

LBL_TAIL_PUSH_REG1_BOOL:
  CODE_COVERAGE(489); // Hit
  reg1 = reg1 ? VM_VALUE_TRUE : VM_VALUE_FALSE;
  goto LBL_TAIL_PUSH_REG1;

LBL_TAIL_PUSH_REG1:
  CODE_COVERAGE(164); // Hit
  PUSH(reg1);
  goto LBL_DO_NEXT_INSTRUCTION;

LBL_EXIT:
  CODE_COVERAGE(165); // Hit
  FLUSH_REGISTER_CACHE();
  return err;
}

void mvm_free(VM* vm) {
  CODE_COVERAGE_UNTESTED(166); // Not hit
  gc_freeGCMemory(vm);
  VM_EXEC_SAFE_MODE(memset(vm, 0, sizeof(*vm)));
  free(vm);
}

/**
 * @param sizeBytes Size in bytes of the allocation, *excluding* the header
 * @param typeCode The type code to insert into the header
 * @param headerVal2 A custom 12-bit value to use in the header. Often this will be the size, or length, etc.
 * @param out_result Output VM-Pointer. Target is after allocation header.
 * @param out_target Output native pointer to region after the allocation header.
 */
// TODO: I think it would make sense to consolidate headerVal2 and sizeBytes
static Value gc_allocateWithHeader(VM* vm, uint16_t sizeBytes, TeTypeCode typeCode, uint16_t headerVal2, void** out_pTarget) {
  CODE_COVERAGE(5); // Hit
  uint16_t allocationSize;
RETRY:
  allocationSize = sizeBytes + 2; // 2 byte header
  // Round up to 2-byte boundary
  allocationSize = (allocationSize + 1) & 0xFFFE;
  // Minimum allocation size is 4 bytes
  if (allocationSize < 4) allocationSize = 4;
  // Note: this is still valid when the bucket is null
  Pointer vpAlloc = vm->vpAllocationCursor;
  void* pAlloc = vm->pAllocationCursor;
  Pointer endOfResult = vpAlloc + allocationSize;
  // Out of space?
  if (endOfResult > vm->vpBucketEnd) {
    CODE_COVERAGE(167); // Hit
    // Allocate a new bucket
    uint16_t bucketSize = VM_ALLOCATION_BUCKET_SIZE;
    if (allocationSize > bucketSize) {
      CODE_COVERAGE_UNTESTED(168); // Not hit
      bucketSize = allocationSize;
    }
    gc_createNextBucket(vm, bucketSize);
    // This must succeed the second time because we've just allocated a bucket at least as big as it needs to be
    goto RETRY;
  }
  vm->vpAllocationCursor = endOfResult;
  vm->pAllocationCursor += allocationSize;

  // Write header
  VM_ASSERT(vm, (headerVal2 & ~0xFFF) == 0);
  VM_ASSERT(vm, (typeCode & ~0xF) == 0);
  vm_HeaderWord headerWord = (typeCode << 12) | headerVal2;
  *((vm_HeaderWord*)pAlloc) = headerWord;

  *out_pTarget = (uint8_t*)pAlloc + 2; // Skip header
  return vpAlloc + 2;
}

/**
 * Allocate raw GC data.
 */
static Pointer gc_allocateWithoutHeader(VM* vm, uint16_t sizeBytes, void** out_pTarget) {
  CODE_COVERAGE(6); // Hit
  // For the sake of flash size, I'm just implementing this in terms of the one
  // that allocates with a header, which is going to be the more commonly used
  // function anyway.
  void* p;
  Pointer vp = gc_allocateWithHeader(vm, sizeBytes - 2, (TeTypeCode)0, 0, &p);
  *out_pTarget = (uint16_t*)p - 1;
  return vp - 2;
}

static void gc_createNextBucket(VM* vm, uint16_t bucketSize) {
  CODE_COVERAGE(7); // Hit
  size_t allocSize = sizeof (vm_TsBucket) + bucketSize;
  vm_TsBucket* bucket = malloc(allocSize);
  if (!bucket) {
    MVM_FATAL_ERROR(vm, MVM_E_MALLOC_FAIL);
  }
  #if MVM_SAFE_MODE
    memset(bucket, 0x7E, allocSize);
  #endif
  bucket->prev = vm->pLastBucket;
  // Note: we start the next bucket at the allocation cursor, not at what we
  // previously called the end of the previous bucket
  bucket->vpAddressStart = vm->vpAllocationCursor;
  vm->pAllocationCursor = (uint8_t*)(bucket + 1);
  vm->vpBucketEnd = vm->vpAllocationCursor + bucketSize;
  vm->pLastBucket = bucket;
}

static void gc_markAllocation(uint16_t* markTable, Pointer p, uint16_t size) {
  CODE_COVERAGE_UNTESTED(8); // Not hit
  if (VM_TAG_OF(p) != VM_TAG_GC_P) return;
  GO_t offset = VM_VALUE_OF(p);

  // Start bit
  uint16_t pWords = offset / VM_GC_ALLOCATION_UNIT;
  uint16_t slotOffset = pWords >> 4;
  uint8_t bitOffset = pWords & 15;
  markTable[slotOffset] |= 0x8000 >> bitOffset;

  // End bit
  pWords += (size / VM_GC_ALLOCATION_UNIT) - 1;
  slotOffset = pWords >> 4;
  bitOffset = pWords & 15;
  markTable[slotOffset] |= 0x8000 >> bitOffset;
}

static inline bool gc_isMarked(uint16_t* markTable, Pointer ptr) {
  CODE_COVERAGE_UNTESTED(9); // Not hit
  // VM_ASSERT(vm, VM_IS_GC_P(ptr));
  GO_t offset = VM_VALUE_OF(ptr);
  uint16_t pWords = offset / VM_GC_ALLOCATION_UNIT;
  uint16_t slotOffset = pWords >> 4;
  uint8_t bitOffset = pWords & 15;
  return markTable[slotOffset] & (0x8000 >> bitOffset);
}

static void gc_freeGCMemory(VM* vm) {
  CODE_COVERAGE(10); // Hit
  while (vm->pLastBucket) {
    CODE_COVERAGE_UNTESTED(169); // Not hit
    vm_TsBucket* prev = vm->pLastBucket->prev;
    free(vm->pLastBucket);
    vm->pLastBucket = prev;
  }
  vm->vpBucketEnd = vpGCSpaceStart;
  vm->vpAllocationCursor = vpGCSpaceStart;
  vm->pAllocationCursor = NULL;
}

static void gc_traceValue(VM* vm, uint16_t* markTable, Value value, uint16_t* pTotalSize) {
  CODE_COVERAGE_UNTESTED(11); // Not hit
  uint16_t tag = value & VM_TAG_MASK;
  if (tag == VM_TAG_INT) {
    CODE_COVERAGE_UNTESTED(170); // Not hit
    return;
  }

  /*
  # Pointers in Program Memory

  Program memory can contain pointers. For example, it's valid for bytecode to
  have a VM_OP3_LOAD_LITERAL instruction with a pointer literal parameter.
  However, pointers to GC memory must themselves be mutable, since GC memory can
  move during compaction. Thus, pointers in program memory can only ever
  reference data memory or other allocations in program memory. Pointers in data
  memory, as with everything in data memory, are in fixed locations. These are
  treated as GC roots and do not need to be referenced by values in program
  memory (see below).

  # Pointers in Data Memory

  Data memory is broadly divided into two sections:

   1. Global variables
   2. Heap allocations

  All global variables are treated as GC roots.

  The heap allocations in data memory are permanent and fixed in size and
  structure, unlike allocations in the GC heap. Members of these allocations
  that can be pointers must be recorded in the gcRoots table so that the GC can
  find them.
  */
  if (tag == VM_TAG_PGM_P) {
    CODE_COVERAGE_UNTESTED(171); // Not hit
    return;
  }

  Pointer pAllocation = value;
  if (gc_isMarked(markTable, pAllocation)) {
    CODE_COVERAGE_UNTESTED(172); // Not hit
    return;
  }

  vm_HeaderWord headerWord = vm_readHeaderWord(vm, pAllocation);
  TeTypeCode typeCode = vm_typeCodeFromHeaderWord(headerWord);
  uint16_t headerData = vm_paramOfHeaderWord(headerWord);

  uint16_t allocationSize; // Including header
  uint8_t headerSize = 2;
  switch (typeCode) {
    case TC_REF_STRUCT:
      CODE_COVERAGE_UNTESTED(173); // Not hit
      allocationSize = 0;
      VM_NOT_IMPLEMENTED(vm);
      break;

    case TC_REF_STRING:
    case TC_REF_UNIQUE_STRING:
    case TC_REF_BIG_INT:
    case TC_REF_SYMBOL:
    case TC_REF_HOST_FUNC:
    case TC_REF_INT32:
    case TC_REF_FLOAT64:
      CODE_COVERAGE_UNTESTED(174); // Not hit
      allocationSize = 2 + headerData; break;

    case TC_REF_PROPERTY_LIST: {
      CODE_COVERAGE_UNTESTED(175); // Not hit
      gc_markAllocation(markTable, pAllocation - 2, sizeof (TsAllocationHeader) + sizeof (TsPropertyList));
      Pointer pCell = vm_readUInt16(vm, pAllocation);
      while (pCell) {
        gc_markAllocation(markTable, pCell, 6);
        Pointer next = vm_readUInt16(vm, pCell + 0);
        Value key = vm_readUInt16(vm, pCell + 2);
        Value value = vm_readUInt16(vm, pCell + 4);

        // TODO(low): This shouldn't be recursive. It shouldn't use the C stack
        gc_traceValue(vm, markTable, key, pTotalSize);
        gc_traceValue(vm, markTable, value, pTotalSize);

        pCell = next;
      }
      return;
    }

    case TC_REF_LIST: {
      CODE_COVERAGE_UNTESTED(176); // Not hit
      uint16_t itemCount = headerData;
      gc_markAllocation(markTable, pAllocation - 2, 4);
      Pointer pCell = vm_readUInt16(vm, pAllocation);
      while (itemCount--) {
        CODE_COVERAGE_UNTESTED(177); // Not hit
        gc_markAllocation(markTable, pCell, 6);
        Pointer next = vm_readUInt16(vm, pCell + 0);
        Value value = vm_readUInt16(vm, pCell + 2);

        // TODO(low): This shouldn't be recursive. It shouldn't use the C stack
        gc_traceValue(vm, markTable, value, pTotalSize);

        pCell = next;
      }
      return;
    }

    case TC_REF_TUPLE: {
      CODE_COVERAGE_UNTESTED(178); // Not hit
      uint16_t itemCount = headerData;
      // Need to mark before recursing
      allocationSize = 2 + itemCount * 2;
      gc_markAllocation(markTable, pAllocation - 2, allocationSize);
      Pointer pItem = pAllocation;
      while (itemCount--) {
        CODE_COVERAGE_UNTESTED(179); // Not hit
        Value item = vm_readUInt16(vm, pItem);
        pItem += 2;
        // TODO(low): This shouldn't be recursive. It shouldn't use the C stack
        gc_traceValue(vm, markTable, item, pTotalSize);
      }
      return;
    }

    case TC_REF_FUNCTION: {
      // It shouldn't get here because functions are only stored in ROM (see
      // note at the beginning of this function)
      VM_UNEXPECTED_INTERNAL_ERROR(vm);
      return;
    }

    default: {
      VM_UNEXPECTED_INTERNAL_ERROR(vm);
      return;
    }
  }
  // Round up to nearest word
  allocationSize = (allocationSize + 3) & 0xFFFE;
  // Allocations can't be smaller than 2 words
  if (allocationSize < 4) {
    CODE_COVERAGE_UNTESTED(180); // Not hit
    allocationSize = 4;
  }

  gc_markAllocation(markTable, pAllocation - headerSize, allocationSize);
  (*pTotalSize) += allocationSize;
}

static inline void gc_updatePointer(VM* vm, uint16_t* pWord, uint16_t* markTable, uint16_t* offsetTable) {
  CODE_COVERAGE_UNTESTED(12); // Not hit
  uint16_t word = *pWord;
  uint16_t tag = word & VM_TAG_MASK;

  if (tag != VM_TAG_GC_P) {
    CODE_COVERAGE_UNTESTED(181); // Not hit
    return;
  }

  GO_t ptr = word & VM_VALUE_MASK;
  uint16_t pWords = ptr / VM_GC_ALLOCATION_UNIT;
  uint16_t slotOffset = pWords >> 4;
  uint8_t bitOffset = pWords & 15;

  uint16_t offset = offsetTable[slotOffset];
  bool inAllocation = offset & 0x0001;
  offset = offset & 0xFFFE;
  uint16_t markBits = markTable[slotOffset];
  uint16_t mask = 0x8000;
  while (bitOffset--) {
    CODE_COVERAGE_UNTESTED(182); // Not hit
    bool gc_isMarked = markBits & mask;
    if (inAllocation) {
      CODE_COVERAGE_UNTESTED(183); // Not hit
      if (gc_isMarked) {
        CODE_COVERAGE_UNTESTED(184); // Not hit
        inAllocation = false;
      } else {
        CODE_COVERAGE_UNTESTED(185); // Not hit
      }
    } else {
      CODE_COVERAGE_UNTESTED(186); // Not hit
      if (gc_isMarked) {
        CODE_COVERAGE_UNTESTED(187); // Not hit
        inAllocation = true;
      } else {
        CODE_COVERAGE_UNTESTED(188); // Not hit
        offset += VM_GC_ALLOCATION_UNIT;
      }
    }
    mask >>= 1;
  }

  *pWord -= offset;
}

// Run a garbage collection cycle
void vm_runGC(VM* vm) {
  CODE_COVERAGE_UNTESTED(13); // Not hit
  if (!vm->pLastBucket) {
    CODE_COVERAGE_UNTESTED(189); // Not hit
    return; // Nothing allocated
  }

  uint16_t allocatedSize = vm->vpAllocationCursor - vpGCSpaceStart;
  // The mark table has 1 mark bit for each 16-bit allocation unit (word) in GC
  // space, and we round up to the nearest whole byte
  uint16_t markTableSize = (allocatedSize + 15) / 16;
  // The adjustment table has one 16-bit adjustment word for every 16 mark bits.
  // It says how much a pointer at that position should be adjusted for
  // compaction.
  uint16_t adjustmentTableSize = markTableSize + 2; // TODO: Can remove the extra 2?
  // We allocate the mark table and adjustment table at the same time for
  // efficiency. The allocation size here is 1/8th the size of the heap memory
  // allocated. So a 2 kB heap requires a 256 B allocation here.
  uint8_t* temp = malloc(markTableSize + adjustmentTableSize);
  if (!temp) {
    MVM_FATAL_ERROR(vm, MVM_E_MALLOC_FAIL);
  }
  // The adjustment table is first because it needs to be 16-bit aligned
  uint16_t* adjustmentTable = (uint16_t*)temp;
  uint16_t* markTable = (uint16_t*)(temp + adjustmentTableSize); // TODO: I'm worried about the efficiency of accessing these as words
  uint16_t* markTableEnd = (uint16_t*)((uint8_t*)markTable + markTableSize);

  VM_ASSERT(vm, ((intptr_t)adjustmentTable & 1) == 0); // Needs to be 16-bit aligned for the following algorithm to work

  memset(markTable, 0, markTableSize);
  VM_EXEC_SAFE_MODE(memset(adjustmentTable, 0, adjustmentTableSize));

  // -- Mark Phase--

  uint16_t totalSize = 0;

  // Mark Global Variables
  {
    uint16_t globalVariableCount = VM_READ_BC_2_HEADER_FIELD(globalVariableCount, vm->pBytecode);

    uint16_t* p = vm->dataMemory;
    while (globalVariableCount--) {
      CODE_COVERAGE_UNTESTED(190); // Not hit
      gc_traceValue(vm, markTable, *p++, &totalSize);
    }
  }

  // Mark other roots in data memory
  {
    uint16_t gcRootsOffset = VM_READ_BC_2_HEADER_FIELD(gcRootsOffset, vm->pBytecode);
    uint16_t gcRootsCount = VM_READ_BC_2_HEADER_FIELD(gcRootsCount, vm->pBytecode);

    MVM_PROGMEM_P pTableEntry = MVM_PROGMEM_P_ADD(vm->pBytecode, gcRootsOffset);
    while (gcRootsCount--) {
      CODE_COVERAGE_UNTESTED(191); // Not hit
      // The table entry in program memory gives us an offset in data memory
      uint16_t dataOffsetWords = MVM_READ_PROGMEM_2(pTableEntry);
      uint16_t dataValue = vm->dataMemory[dataOffsetWords];
      gc_traceValue(vm, markTable, dataValue, &totalSize);
      pTableEntry = MVM_PROGMEM_P_ADD(pTableEntry, 2);
    }
  }

  if (totalSize == 0) {
    CODE_COVERAGE_UNTESTED(192); // Not hit
    // Everything is freed
    gc_freeGCMemory(vm);
    goto LBL_EXIT;
  }

  // If the allocated size is taking up less than 25% more than the used size,
  // then don't collect.
  if (allocatedSize < totalSize * 5 / 4) {
    CODE_COVERAGE_UNTESTED(193); // Not hit
    goto LBL_EXIT;
  }

  // Create adjustment table
  {
    uint16_t mask = 0x8000;
    uint16_t* pMark = markTable;
    uint16_t adjustment = 0;
    adjustmentTable[0] = adjustment & 0xFFFE;
    uint16_t* pAdjustment = &adjustmentTable[1];
    bool inAllocation = false;
    while (pMark < markTableEnd) {
      CODE_COVERAGE_UNTESTED(194); // Not hit
      bool gc_isMarked = (*pMark) & mask;
      if (inAllocation) {
        CODE_COVERAGE_UNTESTED(195); // Not hit
        if (gc_isMarked) {
          CODE_COVERAGE_UNTESTED(196); // Not hit
          inAllocation = false;
        } else {
          CODE_COVERAGE_UNTESTED(197); // Not hit
        }
      } else {
        CODE_COVERAGE_UNTESTED(198); // Not hit
        if (gc_isMarked) {
          CODE_COVERAGE_UNTESTED(199); // Not hit
          inAllocation = true;
        } else {
          CODE_COVERAGE_UNTESTED(200); // Not hit
          adjustment += VM_GC_ALLOCATION_UNIT;
        }
      }
      mask >>= 1;
      if (!mask) {
        CODE_COVERAGE_UNTESTED(201); // Not hit
        *pAdjustment++ = adjustment | (inAllocation ? 1 : 0);
        pMark++;
        mask = 0x8000;
      } else {
        CODE_COVERAGE_UNTESTED(202); // Not hit
      }
    }
  }

  // TODO(med): Pointer update: global variables
  // TODO(med): Pointer update: roots variables
  // TODO(med): Pointer update: recursion

  // Update global variables
  {
    uint16_t* p = vm->dataMemory;
    uint16_t globalVariableCount = VM_READ_BC_2_HEADER_FIELD(globalVariableCount, vm->pBytecode);

    while (globalVariableCount--) {
      CODE_COVERAGE_UNTESTED(203); // Not hit
      gc_updatePointer(vm, p++, markTable, adjustmentTable);
    }
  }

  // Compact phase

  // Temporarily reverse the linked list to make it easier to parse forwards
  // during compaction. Also, we'll change the vpAddressStart field to hold the
  // size.
  vm_TsBucket* first;
  {
    CODE_COVERAGE_UNTESTED(204); // Not hit
    vm_TsBucket* bucket = vm->pLastBucket;
    Pointer vpEndOfBucket = vm->vpBucketEnd;
    vm_TsBucket* next = NULL;
    while (bucket) {
      CODE_COVERAGE_UNTESTED(205); // Not hit
      uint16_t size = vpEndOfBucket - bucket->vpAddressStart;
      vpEndOfBucket = bucket->vpAddressStart; // TODO: I don't remember what this is for. Please comment.
      bucket->vpAddressStart/*size*/ = size;
      vm_TsBucket* prev = bucket->prev;
      bucket->prev/*next*/ = next;
      next = bucket;
      bucket = prev;
    }
    first = next;
  }

  /*
  This is basically a semispace collector. It allocates a completely new
  region and does a full copy of all the memory from the old region into the
  new.
  */
  vm->vpAllocationCursor = vpGCSpaceStart;
  vm->vpBucketEnd = vpGCSpaceStart;
  vm->pLastBucket = NULL;
  gc_createNextBucket(vm, totalSize);

  {
    VM_ASSERT(vm, vm->pLastBucket && !vm->pLastBucket->prev); // Only one bucket (the new one)
    uint16_t* source = (uint16_t*)(first + 1); // Start just after the header
    uint16_t* sourceEnd = (uint16_t*)((uint8_t*)source + first->vpAddressStart/*size*/);
    uint16_t* target = (uint16_t*)(vm->pLastBucket + 1); // Start just after the header
    if (!target) {
      CODE_COVERAGE_UNTESTED(206); // Not hit
      VM_UNEXPECTED_INTERNAL_ERROR(vm);
      return;
    } else {
      CODE_COVERAGE_UNTESTED(207); // Not hit
    }
    uint16_t* pMark = markTable;
    uint16_t mask = 0x8000;
    uint16_t markBits = *pMark++;
    bool copying = false;
    while (first) {
      CODE_COVERAGE_UNTESTED(208); // Not hit
      bool gc_isMarked = markBits & mask;
      if (copying) {
        CODE_COVERAGE_UNTESTED(209); // Not hit
        *target++ = *source++;
        if (gc_isMarked) copying = false;
      }
      else {
        CODE_COVERAGE_UNTESTED(210); // Not hit
        if (gc_isMarked) {
          CODE_COVERAGE_UNTESTED(211); // Not hit
          copying = true;
          *target++ = *source++;
        }
        else {
          CODE_COVERAGE_UNTESTED(212); // Not hit
          source++;
        }
      }

      if (source >= sourceEnd) {
        CODE_COVERAGE_UNTESTED(213); // Not hit
        vm_TsBucket* next = first->prev/*next*/;
        uint16_t size = first->vpAddressStart/*size*/;
        free(first);
        if (!next) {
          CODE_COVERAGE_UNTESTED(214); // Not hit
          break; // Done with compaction
        } else {
          CODE_COVERAGE_UNTESTED(215); // Not hit
        }
        source = (uint16_t*)(next + 1); // Start after the header
        sourceEnd = (uint16_t*)((uint8_t*)source + size);
        first = next;
      }

      mask >>= 1;
      if (!mask) {
        CODE_COVERAGE_UNTESTED(216); // Not hit
        mask = 0x8000;
        markBits = *pMark++;
      } else {
        CODE_COVERAGE_UNTESTED(217); // Not hit
      }
    }
  }
LBL_EXIT:
  CODE_COVERAGE_UNTESTED(218); // Not hit
  free(temp);
}

static void* gc_deref(VM* vm, Pointer vp) {
  CODE_COVERAGE(14); // Hit

  VM_ASSERT(vm, (vp >= vpGCSpaceStart) && (vp <= vm->vpAllocationCursor));

  // Find the right bucket
  vm_TsBucket* pBucket = vm->pLastBucket;
  VM_SAFE_CHECK_NOT_NULL_2(pBucket);
  while (vp < pBucket->vpAddressStart) {
    CODE_COVERAGE(219); // Hit
    pBucket = pBucket->prev;
    VM_SAFE_CHECK_NOT_NULL_2(pBucket);
  }

  // This would be more efficient if buckets had some kind of "offset" field which took into account all of this
  uint8_t* bucketData = ((uint8_t*)(pBucket + 1));
  uint8_t* p = bucketData + (vp - pBucket->vpAddressStart);
  return p;
}

// A function call invoked by the host
TeError mvm_call(VM* vm, Value func, Value* out_result, Value* args, uint8_t argCount) {
  CODE_COVERAGE(15); // Hit

  TeError err;
  if (out_result) {
    CODE_COVERAGE(220); // Hit
    *out_result = VM_VALUE_UNDEFINED;
  } else {
    CODE_COVERAGE_UNTESTED(221); // Not hit
  }

  vm_setupCallFromExternal(vm, func, args, argCount);

  // Run the machine until it hits the corresponding return instruction. The
  // return instruction pops the arguments off the stack and pushes the returned
  // value.
  err = vm_run(vm);
  if (err != MVM_E_SUCCESS) {
    CODE_COVERAGE_UNTESTED(222); // Not hit
    return err;
  } else {
    CODE_COVERAGE(223); // Hit
  }

  if (out_result) {
    CODE_COVERAGE(224); // Hit
    *out_result = vm_pop(vm);
  } else {
    CODE_COVERAGE_UNTESTED(225); // Not hit
  }

  // Release the stack if we hit the bottom
  if (vm->stack->reg.pStackPointer == VM_BOTTOM_OF_STACK(vm)) {
    CODE_COVERAGE(226); // Hit
    free(vm->stack);
    vm->stack = NULL;
  } else {
    CODE_COVERAGE_UNTESTED(227); // Not hit
  }

  return MVM_E_SUCCESS;
}

static TeError vm_setupCallFromExternal(VM* vm, Value func, Value* args, uint8_t argCount) {
  CODE_COVERAGE(16); // Hit
  if (deepTypeOf(vm, func) != TC_REF_FUNCTION) {
    CODE_COVERAGE_UNTESTED(228); // Not hit
    return MVM_E_TARGET_IS_NOT_A_VM_FUNCTION;
  } else {
    CODE_COVERAGE(229); // Hit
  }

  // There is no stack if this is not a reentrant invocation
  if (!vm->stack) {
    CODE_COVERAGE(230); // Hit
    // This is freed again at the end of mvm_call
    vm_TsStack* stack = malloc(sizeof (vm_TsStack) + MVM_STACK_SIZE);
    if (!stack) {
      CODE_COVERAGE_UNTESTED(231); // Not hit
      return MVM_E_MALLOC_FAIL;
    }
    memset(stack, 0, sizeof *stack);
    vm_TsRegisters* reg = &stack->reg;
    // The stack grows upward. The bottom is the lowest address.
    uint16_t* bottomOfStack = (uint16_t*)(stack + 1);
    reg->pFrameBase = bottomOfStack;
    reg->pStackPointer = bottomOfStack;
    vm->stack = stack;
  } else {
    CODE_COVERAGE_UNTESTED(232); // Not hit
  }

  vm_TsStack* stack = vm->stack;
  uint16_t* bottomOfStack = (uint16_t*)(stack + 1);
  vm_TsRegisters* reg = &stack->reg;

  VM_ASSERT(vm, reg->programCounter == 0); // Assert that we're outside the VM at the moment

  VM_ASSERT(vm, VM_TAG_OF(func) == VM_TAG_PGM_P);
  BO_t functionOffset = VM_VALUE_OF(func);
  uint8_t maxStackDepth = VM_READ_BC_1_AT(functionOffset, vm->pBytecode);
  // TODO(low): Since we know the max stack depth for the function, we could actually grow the stack dynamically rather than allocate it fixed size.
  if (vm->stack->reg.pStackPointer + (maxStackDepth + VM_FRAME_SAVE_SIZE_WORDS) > VM_TOP_OF_STACK(vm)) {
    CODE_COVERAGE_UNTESTED(233); // Not hit
    return MVM_E_STACK_OVERFLOW;
  }

  vm_push(vm, func); // We need to push the function because the corresponding RETURN instruction will pop it. The actual value is not used.
  Value* arg = &args[0];
  for (int i = 0; i < argCount; i++)
    vm_push(vm, *arg++);

  // Save caller state (VM_FRAME_SAVE_SIZE_WORDS)
  vm_push(vm, reg->pFrameBase - bottomOfStack);
  vm_push(vm, reg->argCount);
  vm_push(vm, reg->programCounter);

  // Set up new frame
  reg->pFrameBase = reg->pStackPointer;
  reg->argCount = argCount;
  reg->programCounter = functionOffset + sizeof (vm_TsFunctionHeader);

  return MVM_E_SUCCESS;
}

TeError vm_resolveExport(VM* vm, mvm_VMExportID id, Value* result) {
  CODE_COVERAGE(17); // Hit
  MVM_PROGMEM_P pBytecode = vm->pBytecode;
  uint16_t exportTableOffset = VM_READ_BC_2_HEADER_FIELD(exportTableOffset, pBytecode);
  uint16_t exportTableSize = VM_READ_BC_2_HEADER_FIELD(exportTableSize, pBytecode);

  MVM_PROGMEM_P exportTable = MVM_PROGMEM_P_ADD(vm->pBytecode, exportTableOffset);
  MVM_PROGMEM_P exportTableEnd = MVM_PROGMEM_P_ADD(exportTable, exportTableSize);

  // See vm_TsExportTableEntry
  MVM_PROGMEM_P exportTableEntry = exportTable;
  while (exportTableEntry < exportTableEnd) {
    CODE_COVERAGE(234); // Hit
    mvm_VMExportID exportID = MVM_READ_PROGMEM_2(exportTableEntry);
    if (exportID == id) {
      CODE_COVERAGE(235); // Hit
      MVM_PROGMEM_P pExportvalue = MVM_PROGMEM_P_ADD(exportTableEntry, 2);
      mvm_VMExportID exportValue = MVM_READ_PROGMEM_2(pExportvalue);
      *result = exportValue;
      return MVM_E_SUCCESS;
    } else {
      CODE_COVERAGE_UNTESTED(236); // Not hit
    }
    exportTableEntry = MVM_PROGMEM_P_ADD(exportTableEntry, sizeof (vm_TsExportTableEntry));
  }

  *result = VM_VALUE_UNDEFINED;
  return MVM_E_UNRESOLVED_EXPORT;
}

TeError mvm_resolveExports(VM* vm, const mvm_VMExportID* idTable, Value* resultTable, uint8_t count) {
  CODE_COVERAGE(18); // Hit
  TeError err = MVM_E_SUCCESS;
  while (count--) {
    CODE_COVERAGE(237); // Hit
    TeError tempErr = vm_resolveExport(vm, *idTable++, resultTable++);
    if (tempErr != MVM_E_SUCCESS) {
      CODE_COVERAGE_UNTESTED(238); // Not hit
      err = tempErr;
    } else {
      CODE_COVERAGE(239); // Hit
    }
  }
  return err;
}

void mvm_initializeHandle(VM* vm, mvm_Handle* handle) {
  CODE_COVERAGE(19); // Hit
  VM_ASSERT(vm, !vm_isHandleInitialized(vm, handle));
  handle->_next = vm->gc_handles;
  vm->gc_handles = handle;
  handle->_value = VM_VALUE_UNDEFINED;
}

void vm_cloneHandle(VM* vm, mvm_Handle* target, const mvm_Handle* source) {
  CODE_COVERAGE_UNTESTED(20); // Not hit
  VM_ASSERT(vm, !vm_isHandleInitialized(vm, source));
  mvm_initializeHandle(vm, target);
  target->_value = source->_value;
}

TeError mvm_releaseHandle(VM* vm, mvm_Handle* handle) {
  // This function doesn't contain coverage markers because node hits this path
  // non-deterministically.
  mvm_Handle** h = &vm->gc_handles;
  while (*h) {
    if (*h == handle) {
      *h = handle->_next;
      handle->_value = VM_VALUE_UNDEFINED;
      handle->_next = NULL;
      return MVM_E_SUCCESS;
    }
    h = &((*h)->_next);
  }
  handle->_value = VM_VALUE_UNDEFINED;
  handle->_next = NULL;
  return MVM_E_INVALID_HANDLE;
}

static bool vm_isHandleInitialized(VM* vm, const mvm_Handle* handle) {
  CODE_COVERAGE(22); // Hit
  mvm_Handle* h = vm->gc_handles;
  while (h) {
    CODE_COVERAGE(243); // Hit
    if (h == handle) {
      CODE_COVERAGE_UNTESTED(244); // Not hit
      return true;
    } else {
      CODE_COVERAGE(245); // Hit
    }
    h = h->_next;
  }
  return false;
}

static Value vm_convertToString(VM* vm, Value value) {
  CODE_COVERAGE(23); // Hit
  TeTypeCode type = deepTypeOf(vm, value);

  switch (type) {
    case VM_TAG_INT: {
      CODE_COVERAGE_UNTESTED(246); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    case TC_REF_INT32: {
      CODE_COVERAGE_UNTESTED(247); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    case TC_REF_FLOAT64: {
      CODE_COVERAGE_UNTESTED(248); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    case TC_REF_STRING: {
      CODE_COVERAGE(249); // Hit
      return value;
    }
    case TC_REF_UNIQUE_STRING: {
      CODE_COVERAGE(250); // Hit
      return value;
    }
    case TC_REF_PROPERTY_LIST: {
      CODE_COVERAGE_UNTESTED(251); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    case TC_REF_LIST: {
      CODE_COVERAGE_UNTESTED(252); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    case TC_REF_TUPLE: {
      CODE_COVERAGE_UNTESTED(253); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    case TC_REF_FUNCTION: {
      CODE_COVERAGE_UNTESTED(254); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    case TC_REF_HOST_FUNC: {
      CODE_COVERAGE_UNTESTED(255); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    case TC_REF_BIG_INT: {
      CODE_COVERAGE_UNTESTED(256); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    case TC_REF_SYMBOL: {
      CODE_COVERAGE_UNTESTED(257); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    case TC_VAL_UNDEFINED: {
      CODE_COVERAGE_UNTESTED(258); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    case TC_VAL_NULL: {
      CODE_COVERAGE_UNTESTED(259); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    case TC_VAL_TRUE: {
      CODE_COVERAGE_UNTESTED(260); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    case TC_VAL_FALSE: {
      CODE_COVERAGE_UNTESTED(261); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    case TC_VAL_NAN: {
      CODE_COVERAGE_UNTESTED(262); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    case TC_VAL_NEG_ZERO: {
      CODE_COVERAGE_UNTESTED(263); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    case TC_VAL_DELETED: {
      CODE_COVERAGE_UNTESTED(264); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    case TC_REF_STRUCT: {
      CODE_COVERAGE_UNTESTED(265); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    default: return VM_UNEXPECTED_INTERNAL_ERROR(vm);
  }
}

static Value vm_concat(VM* vm, Value left, Value right) {
  CODE_COVERAGE(24); // Hit
  size_t leftSize = 0;
  const char* leftStr = mvm_toStringUtf8(vm, left, &leftSize);
  size_t rightSize = 0;
  const char* rightStr = mvm_toStringUtf8(vm, right, &rightSize);
  uint8_t* data;
  Value value = vm_allocString(vm, leftSize + rightSize, (void**)&data);
  memcpy(data, leftStr, leftSize);
  memcpy(data + leftSize, rightStr, rightSize);
  return value;
}

static Value vm_convertToNumber(VM* vm, Value value) {
  CODE_COVERAGE_UNTESTED(25); // Not hit
  uint16_t tag = value & VM_TAG_MASK;
  if (tag == VM_TAG_INT) return value;

  TeTypeCode type = deepTypeOf(vm, value);
  switch (type) {
    case TC_REF_INT32: {
      CODE_COVERAGE_UNTESTED(266); // Not hit
      return value;
    }
    case TC_REF_FLOAT64: {
      CODE_COVERAGE_UNTESTED(267); // Not hit
      return value;
    }
    case TC_REF_STRING: {
      CODE_COVERAGE_UNTESTED(268); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    case TC_REF_UNIQUE_STRING: {
      CODE_COVERAGE_UNTESTED(269); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    case TC_REF_PROPERTY_LIST: {
      CODE_COVERAGE_UNTESTED(270); // Not hit
      return VM_VALUE_NAN;
    }
    case TC_REF_LIST: {
      CODE_COVERAGE_UNTESTED(271); // Not hit
      return VM_VALUE_NAN;
    }
    case TC_REF_TUPLE: {
      CODE_COVERAGE_UNTESTED(272); // Not hit
      return VM_VALUE_NAN;
    }
    case TC_REF_FUNCTION: {
      CODE_COVERAGE_UNTESTED(273); // Not hit
      return VM_VALUE_NAN;
    }
    case TC_REF_HOST_FUNC: {
      CODE_COVERAGE_UNTESTED(274); // Not hit
      return VM_VALUE_NAN;
    }
    case TC_REF_BIG_INT: {
      CODE_COVERAGE_UNTESTED(275); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    case TC_REF_SYMBOL: {
      CODE_COVERAGE_UNTESTED(276); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    case TC_VAL_UNDEFINED: {
      CODE_COVERAGE_UNTESTED(277); // Not hit
      return 0;
    }
    case TC_VAL_NULL: {
      CODE_COVERAGE_UNTESTED(278); // Not hit
      return 0;
    }
    case TC_VAL_TRUE: {
      CODE_COVERAGE_UNTESTED(279); // Not hit
      return 1;
    }
    case TC_VAL_FALSE: {
      CODE_COVERAGE_UNTESTED(280); // Not hit
      return 0;
    }
    case TC_VAL_NAN: {
      CODE_COVERAGE_UNTESTED(281); // Not hit
      return value;
    }
    case TC_VAL_NEG_ZERO: {
      CODE_COVERAGE_UNTESTED(282); // Not hit
      return value;
    }
    case TC_VAL_DELETED: {
      CODE_COVERAGE_UNTESTED(283); // Not hit
      return 0;
    }
    case TC_REF_STRUCT: {
      CODE_COVERAGE_UNTESTED(284); // Not hit
      return VM_VALUE_NAN;
    }
    default: return VM_UNEXPECTED_INTERNAL_ERROR(vm);
  }
}

/* Returns the deep type of the value, looking through pointers and boxing */
static TeTypeCode deepTypeOf(VM* vm, Value value) {
  CODE_COVERAGE(27); // Hit
  TeValueTag tag = VM_TAG_OF(value);
  if (tag == VM_TAG_INT) {
    CODE_COVERAGE(295); // Hit
    return TC_VAL_INT14;
  }

  // Check for "well known" values such as TC_VAL_UNDEFINED
  if (tag == VM_TAG_PGM_P && value < VM_VALUE_MAX_WELLKNOWN) {
    CODE_COVERAGE(296); // Hit
    // Well known types have a value that matches the corresponding type code
    return (TeTypeCode)VM_VALUE_OF(value);
  } else {
    CODE_COVERAGE(297); // Hit
  }

  // Else, value is a pointer. The type of a pointer value is the type of the value being pointed to
  vm_HeaderWord headerWord = vm_readHeaderWord(vm, value);
  TeTypeCode typeCode = vm_typeCodeFromHeaderWord(headerWord);

  return typeCode;
}

int32_t mvm_float64ToInt32(MVM_FLOAT64 value) {
  CODE_COVERAGE(486); // Hit
  if (isfinite(value)) {
    CODE_COVERAGE(487); // Hit
    return (int32_t)value;
  }
  else {
    CODE_COVERAGE(488); // Hit
    return 0;
  }
}

Value mvm_newNumber(VM* vm, MVM_FLOAT64 value) {
  CODE_COVERAGE(28); // Hit
  if (isnan(value)) {
    CODE_COVERAGE(298); // Hit
    return VM_VALUE_NAN;
  }
  if (value == -0.0) {
    CODE_COVERAGE(299); // Hit
    return VM_VALUE_NEG_ZERO;
  }

  // Doubles are very expensive to compute, so at every opportunity, we'll check
  // if we can coerce back to an integer
  int32_t valueAsInt = mvm_float64ToInt32(value);
  if (value == (MVM_FLOAT64)valueAsInt) {
    CODE_COVERAGE(300); // Hit
    return mvm_newInt32(vm, valueAsInt);
  } else {
    CODE_COVERAGE(301); // Hit
  }

  double* pResult;
  Value resultValue = gc_allocateWithHeader(vm, sizeof (MVM_FLOAT64), TC_REF_FLOAT64, sizeof (MVM_FLOAT64), (void**)&pResult);
  *pResult = value;

  return resultValue;
}

Value mvm_newInt32(VM* vm, int32_t value) {
  CODE_COVERAGE(29); // Hit
  if ((value >= VM_MIN_INT14) && (value <= VM_MAX_INT14)) {
    CODE_COVERAGE(302); // Hit
    return (value & VM_VALUE_MASK) | VM_TAG_INT;
  } else {
    CODE_COVERAGE(303); // Hit
  }

  // Int32
  int32_t* pResult;
  Value resultValue = gc_allocateWithHeader(vm, sizeof (int32_t), TC_REF_INT32, sizeof (int32_t), (void**)&pResult);
  *pResult = value;

  return resultValue;
}

bool mvm_toBool(VM* vm, Value value) {
  CODE_COVERAGE(30); // Hit
  uint16_t tag = value & VM_TAG_MASK;
  if (tag == VM_TAG_INT) {
    CODE_COVERAGE_UNTESTED(304); // Not hit
    return value != 0;
  }

  TeTypeCode type = deepTypeOf(vm, value);
  switch (type) {
    case TC_REF_INT32: {
      CODE_COVERAGE_UNTESTED(305); // Not hit
      // Int32 can't be zero, otherwise it would be encoded as an int14
      VM_ASSERT(vm, vm_readInt32(vm, type, value) != 0);
      return false;
    }
    case TC_REF_FLOAT64: {
      CODE_COVERAGE_UNTESTED(306); // Not hit
      // Double can't be zero, otherwise it would be encoded as an int14
      VM_ASSERT(vm, vm_readDouble(vm, type, value) != 0);
      return false;
    }
    case TC_REF_UNIQUE_STRING:
    case TC_REF_STRING: {
      CODE_COVERAGE_UNTESTED(307); // Not hit
      return vm_stringSizeUtf8(vm, value) != 0;
    }
    case TC_REF_PROPERTY_LIST: {
      CODE_COVERAGE_UNTESTED(308); // Not hit
      return true;
    }
    case TC_REF_LIST: {
      CODE_COVERAGE_UNTESTED(309); // Not hit
      return true;
    }
    case TC_REF_TUPLE: {
      CODE_COVERAGE_UNTESTED(310); // Not hit
      return true;
    }
    case TC_REF_FUNCTION: {
      CODE_COVERAGE_UNTESTED(311); // Not hit
      return true;
    }
    case TC_REF_HOST_FUNC: {
      CODE_COVERAGE_UNTESTED(312); // Not hit
      return true;
    }
    case TC_REF_BIG_INT: {
      CODE_COVERAGE_UNTESTED(313); // Not hit
      return VM_RESERVED(vm);
    }
    case TC_REF_SYMBOL: {
      CODE_COVERAGE_UNTESTED(314); // Not hit
      return true;
    }
    case TC_VAL_UNDEFINED: {
      CODE_COVERAGE_UNTESTED(315); // Not hit
      return false;
    }
    case TC_VAL_NULL: {
      CODE_COVERAGE_UNTESTED(316); // Not hit
      return false;
    }
    case TC_VAL_TRUE: {
      CODE_COVERAGE(317); // Hit
      return true;
    }
    case TC_VAL_FALSE: {
      CODE_COVERAGE(318); // Hit
      return false;
    }
    case TC_VAL_NAN: {
      CODE_COVERAGE_UNTESTED(319); // Not hit
      return false;
    }
    case TC_VAL_NEG_ZERO: {
      CODE_COVERAGE_UNTESTED(320); // Not hit
      return false;
    }
    case TC_VAL_DELETED: {
      CODE_COVERAGE_UNTESTED(321); // Not hit
      return false;
    }
    case TC_REF_STRUCT: {
      CODE_COVERAGE_UNTESTED(322); // Not hit
      return true;
    }
    default: return VM_UNEXPECTED_INTERNAL_ERROR(vm);
  }
}

static bool vm_isString(VM* vm, Value value) {
  CODE_COVERAGE(31); // Hit
  TeTypeCode deepType = deepTypeOf(vm, value);
  if ((deepType == TC_REF_STRING) || (deepType == TC_REF_UNIQUE_STRING)) {
    CODE_COVERAGE(323); // Hit
    return true;
  } else {
    CODE_COVERAGE(324); // Hit
    return false;
  }
}

/** Reads a numeric value that is a subset of a double */
static MVM_FLOAT64 vm_readDouble(VM* vm, TeTypeCode type, Value value) {
  CODE_COVERAGE_UNTESTED(32); // Not hit
  switch (type) {
    case TC_VAL_INT14: {
      CODE_COVERAGE_UNTESTED(325); // Not hit
      return (MVM_FLOAT64)value;
    }
    case TC_REF_INT32: {
      CODE_COVERAGE_UNTESTED(326); // Not hit
      return (MVM_FLOAT64)vm_readInt32(vm, type, value);
    }
    case TC_REF_FLOAT64: {
      CODE_COVERAGE_UNTESTED(327); // Not hit
      MVM_FLOAT64 result;
      vm_readMem(vm, &result, value, sizeof result);
      return result;
    }
    case VM_VALUE_NAN: {
      CODE_COVERAGE_UNTESTED(328); // Not hit
      return MVM_FLOAT64_NAN;
    }
    case VM_VALUE_NEG_ZERO: {
      CODE_COVERAGE_UNTESTED(329); // Not hit
      return -0.0;
    }

    // vm_readDouble is only valid for numeric types
    default: return VM_UNEXPECTED_INTERNAL_ERROR(vm);
  }
}

/** Reads a numeric value that is a subset of a 32-bit integer */
static int32_t vm_readInt32(VM* vm, TeTypeCode type, Value value) {
  CODE_COVERAGE(33); // Hit
  if (type == TC_VAL_INT14) {
    CODE_COVERAGE(330); // Hit
    if (value >= 0x2000) { // Negative
      CODE_COVERAGE(91); // Hit
      return value - 0x4000;
    }
    else {
      CODE_COVERAGE(446); // Hit
      return value;
    }
  } else if (type == TC_REF_INT32) {
    CODE_COVERAGE(331); // Hit
    int32_t result;
    vm_readMem(vm, &result, value, sizeof result);
    return result;
  } else {
    return VM_UNEXPECTED_INTERNAL_ERROR(vm);
  }
}

static void vm_push(VM* vm, uint16_t value) {
  CODE_COVERAGE(34); // Hit
  *(vm->stack->reg.pStackPointer++) = value;
}

static uint16_t vm_pop(VM* vm) {
  CODE_COVERAGE(35); // Hit
  return *(--vm->stack->reg.pStackPointer);
}

static void vm_writeUInt16(VM* vm, Pointer p, Value value) {
  CODE_COVERAGE(36); // Hit
  vm_writeMem(vm, p, &value, sizeof value);
}


static uint16_t vm_readUInt16(VM* vm, Pointer p) {
  CODE_COVERAGE(332); // Hit
  uint16_t result;
  vm_readMem(vm, &result, p, sizeof(result));
  return result;
}

static inline vm_HeaderWord vm_readHeaderWord(VM* vm, Pointer pAllocation) {
  CODE_COVERAGE(37); // Hit
  return vm_readUInt16(vm, pAllocation - 2);
}

// TODO: Audit uses of this, since it's a slow function
static void vm_readMem(VM* vm, void* target, Pointer source, uint16_t size) {
  CODE_COVERAGE(38); // Hit
  uint16_t addr = VM_VALUE_OF(source);
  switch (VM_TAG_OF(source)) {
    case VM_TAG_GC_P: {
      CODE_COVERAGE(333); // Hit
      uint8_t* sourceAddress = gc_deref(vm, source);
      memcpy(target, sourceAddress, size);
      break;
    }
    case VM_TAG_DATA_P: {
      CODE_COVERAGE_UNTESTED(334); // Not hit
      memcpy(target, (uint8_t*)vm->dataMemory + addr, size);
      break;
    }
    case VM_TAG_PGM_P: {
      CODE_COVERAGE(335); // Hit
      VM_ASSERT(vm, source > VM_VALUE_MAX_WELLKNOWN);
      VM_READ_BC_N_AT(target, addr, size, vm->pBytecode);
      break;
    }
    default: VM_UNEXPECTED_INTERNAL_ERROR(vm);
  }
}

static void vm_writeMem(VM* vm, Pointer target, void* source, uint16_t size) {
  CODE_COVERAGE(39); // Hit
  switch (VM_TAG_OF(target)) {
    case VM_TAG_GC_P: {
      CODE_COVERAGE(336); // Hit
      uint8_t* targetAddress = gc_deref(vm, target);
      memcpy(targetAddress, source, size);
      break;
    }
    case VM_TAG_DATA_P: {
      CODE_COVERAGE_UNTESTED(337); // Not hit
      uint16_t addr = VM_VALUE_OF(target);
      memcpy((uint8_t*)vm->dataMemory + addr, source, size);
      break;
    }
    case VM_TAG_PGM_P: {
      CODE_COVERAGE_UNTESTED(338); // Not hit
      MVM_FATAL_ERROR(vm, MVM_E_ATTEMPT_TO_WRITE_TO_ROM);
      break;
    }
    default: VM_UNEXPECTED_INTERNAL_ERROR(vm);
  }
}

static inline mvm_TfHostFunction* vm_getResolvedImports(VM* vm) {
  CODE_COVERAGE(40); // Hit
  return (mvm_TfHostFunction*)(vm + 1); // Starts right after the header
}

static inline uint16_t vm_getResolvedImportCount(VM* vm) {
  CODE_COVERAGE(41); // Hit
  uint16_t importTableSize = VM_READ_BC_2_HEADER_FIELD(importTableSize, vm->pBytecode);
  uint16_t importCount = importTableSize / sizeof(vm_TsImportTableEntry);
  return importCount;
}

mvm_TeType mvm_typeOf(VM* vm, Value value) {
  CODE_COVERAGE(42); // Hit
  TeTypeCode type = deepTypeOf(vm, value);
  // TODO: This should be implemented as a lookup table, not a switch
  switch (type) {
    case TC_VAL_UNDEFINED:
    case TC_VAL_DELETED: {
      CODE_COVERAGE(339); // Hit
      return VM_T_UNDEFINED;
    }

    case TC_VAL_NULL: {
      CODE_COVERAGE_UNTESTED(340); // Not hit
      return VM_T_NULL;
    }

    case TC_VAL_TRUE:
    case TC_VAL_FALSE: {
      CODE_COVERAGE(341); // Hit
      return VM_T_BOOLEAN;
    }

    case TC_VAL_INT14:
    case TC_REF_FLOAT64:
    case TC_REF_INT32:
    case TC_VAL_NAN:
    case TC_VAL_NEG_ZERO: {
      CODE_COVERAGE(342); // Hit
      return VM_T_NUMBER;
    }

    case TC_REF_STRING:
    case TC_REF_UNIQUE_STRING: {
      CODE_COVERAGE(343); // Hit
      return VM_T_STRING;
    }

    case TC_REF_LIST:
    case TC_REF_TUPLE: {
      CODE_COVERAGE_UNTESTED(344); // Not hit
      return VM_T_ARRAY;
    }

    case TC_REF_PROPERTY_LIST:
    case TC_REF_STRUCT: {
      CODE_COVERAGE_UNTESTED(345); // Not hit
      return VM_T_OBJECT;
    }

    case TC_REF_FUNCTION:
    case TC_REF_HOST_FUNC: {
      CODE_COVERAGE(346); // Hit
      return VM_T_FUNCTION;
    }

    case TC_REF_BIG_INT: {
      CODE_COVERAGE_UNTESTED(347); // Not hit
      return VM_T_BIG_INT;
    }
    case TC_REF_SYMBOL: {
      CODE_COVERAGE_UNTESTED(348); // Not hit
      return VM_T_SYMBOL;
    }

    default: VM_UNEXPECTED_INTERNAL_ERROR(vm); return VM_T_UNDEFINED;
  }
}

const char* mvm_toStringUtf8(VM* vm, Value value, size_t* out_sizeBytes) {
  CODE_COVERAGE(43); // Hit
  value = vm_convertToString(vm, value);

  vm_HeaderWord headerWord = vm_readHeaderWord(vm, value);
  TeTypeCode typeCode = vm_typeCodeFromHeaderWord(headerWord);

  VM_ASSERT(vm, (typeCode == TC_REF_STRING) || (typeCode == TC_REF_UNIQUE_STRING));

  uint16_t sourceSize = vm_paramOfHeaderWord(headerWord);

  if (out_sizeBytes) {
    CODE_COVERAGE(349); // Hit
    *out_sizeBytes = sourceSize - 1; // Without the extra safety null-terminator
  } else {
    CODE_COVERAGE_UNTESTED(350); // Not hit
  }

  // If the string is program memory, we have to allocate a copy of it in data
  // memory because program memory is not necessarily addressable
  // TODO: There should be a flag to suppress this when it isn't needed
  if (VM_IS_PGM_P(value)) {
    CODE_COVERAGE(351); // Hit
    void* data;
    gc_allocateWithHeader(vm, sourceSize, TC_REF_STRING, sourceSize, &data);
    vm_readMem(vm, data, value, sourceSize);
    return data;
  } else {
    CODE_COVERAGE(352); // Hit
    return vm_deref(vm, value);
  }
}

Value mvm_newBoolean(bool source) {
  CODE_COVERAGE(44); // Hit
  return source ? VM_VALUE_TRUE : VM_VALUE_FALSE;
}

Value vm_allocString(VM* vm, size_t sizeBytes, void** data) {
  CODE_COVERAGE(45); // Hit
  if (sizeBytes > 0x3FFF - 1) {
    CODE_COVERAGE_UNTESTED(353); // Not hit
    MVM_FATAL_ERROR(vm, MVM_E_ALLOCATION_TOO_LARGE);
  } else {
    CODE_COVERAGE(354); // Hit
  }
  // Note: allocating 1 extra byte for the extra null terminator
  Value value = gc_allocateWithHeader(vm, (uint16_t)sizeBytes + 1, TC_REF_STRING, (uint16_t)sizeBytes + 1, data);
  // Null terminator
  ((char*)(*data))[sizeBytes] = '\0';
  return value;
}

Value mvm_newString(VM* vm, const char* sourceUtf8, size_t sizeBytes) {
  CODE_COVERAGE_UNTESTED(46); // Not hit
  void* data;
  Value value = vm_allocString(vm, sizeBytes, &data);
  memcpy(data, sourceUtf8, sizeBytes);
  return value;
}

static void* vm_deref(VM* vm, Value pSrc) {
  CODE_COVERAGE(47); // Hit
  uint16_t tag = VM_TAG_OF(pSrc);
  if (tag == VM_TAG_GC_P) {
    CODE_COVERAGE(355); // Hit
    return gc_deref(vm, pSrc);
  } else {
    CODE_COVERAGE_UNTESTED(356); // Not hit
  }
  if (tag == VM_TAG_DATA_P) {
    CODE_COVERAGE_UNTESTED(357); // Not hit
    return (uint8_t*)vm->dataMemory + VM_VALUE_OF(pSrc);
  } else {
    CODE_COVERAGE_UNTESTED(358); // Not hit
  }
  // Program pointers (and integers) are not dereferenceable, so it shouldn't get here.
  VM_UNEXPECTED_INTERNAL_ERROR(vm);
  return NULL;
}

static TeError getProperty(VM* vm, Value objectValue, Value propertyName, Value* propertyValue) {
  CODE_COVERAGE(48); // Hit
  toPropertyName(vm, &propertyName);
  TeTypeCode type = deepTypeOf(vm, objectValue);
  switch (type) {
    case TC_REF_PROPERTY_LIST: {
      CODE_COVERAGE(359); // Hit
      Pointer pCell = vm_readUInt16(vm, objectValue);
      while (pCell) {
        CODE_COVERAGE(360); // Hit
        TsPropertyCell cell;
        vm_readMem(vm, &cell, pCell, sizeof cell);
        // We can do direct comparison because the strings have been uniqued,
        // and numbers are represented in a normalized way.
        if (cell.key == propertyName) {
          CODE_COVERAGE(361); // Hit
          *propertyValue = cell.value;
          return MVM_E_SUCCESS;
        } else {
          CODE_COVERAGE(362); // Hit
        }
        pCell = cell.next;
      }
      *propertyValue = VM_VALUE_UNDEFINED;
      return MVM_E_SUCCESS;
    }
    case TC_REF_LIST: {
      CODE_COVERAGE_UNIMPLEMENTED(363); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    case TC_REF_TUPLE: {
      CODE_COVERAGE_UNIMPLEMENTED(364); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    case TC_REF_STRUCT: {
      CODE_COVERAGE_UNIMPLEMENTED(365); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    default: return MVM_E_TYPE_ERROR;
  }
}

static TeError setProperty(VM* vm, Value objectValue, Value propertyName, Value propertyValue) {
  CODE_COVERAGE(49); // Hit
  toPropertyName(vm, &propertyName);
  TeTypeCode type = deepTypeOf(vm, objectValue);
  switch (type) {
    case TC_REF_PROPERTY_LIST: {
      CODE_COVERAGE(366); // Hit
      Pointer vppCell = objectValue + OFFSETOF(TsPropertyList, first);
      Pointer vpCell = vm_readUInt16(vm, vppCell);
      while (vpCell) {
        CODE_COVERAGE(367); // Hit
        Value key = vm_readUInt16(vm, vpCell + OFFSETOF(TsPropertyCell, key));
        // We can do direct comparison because the strings have been uniqued,
        // and numbers are represented in a normalized way.
        if (key == propertyName) {
          CODE_COVERAGE(368); // Hit
          vm_writeUInt16(vm, vpCell + OFFSETOF(TsPropertyCell, value), propertyValue);
          return MVM_E_SUCCESS;
        } else {
          CODE_COVERAGE(369); // Hit
        }
        vppCell = vpCell + OFFSETOF(TsPropertyCell, next);
        vpCell = vm_readUInt16(vm, vppCell);
      }
      // If we reach the end, then this is a new property
      TsPropertyCell* pNewCell;
      Pointer vpNewCell = gc_allocateWithoutHeader(vm, sizeof (TsPropertyCell), (void**)&pNewCell);
      pNewCell->key = propertyName;
      pNewCell->value = propertyValue;
      pNewCell->next = 0;
      vm_writeUInt16(vm, vppCell, vpNewCell);
      return MVM_E_SUCCESS;
    }
    case TC_REF_LIST: {
      CODE_COVERAGE_UNIMPLEMENTED(370); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    case TC_REF_TUPLE: {
      CODE_COVERAGE_UNIMPLEMENTED(371); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    case TC_REF_STRUCT: {
      CODE_COVERAGE_UNIMPLEMENTED(372); // Not hit
      return VM_NOT_IMPLEMENTED(vm);
    }
    default: return MVM_E_TYPE_ERROR;
  }
}

/** Converts the argument to either an TC_VAL_INT14 or a TC_REF_UNIQUE_STRING, or gives an error */
static TeError toPropertyName(VM* vm, Value* value) {
  CODE_COVERAGE(50); // Hit
  // Property names in microvium are either integer indexes or non-integer unique strings
  TeTypeCode type = deepTypeOf(vm, *value);
  switch (type) {
    // These are already valid property names
    case TC_VAL_INT14:
    case TC_REF_UNIQUE_STRING: {
      CODE_COVERAGE(373); // Hit
      return MVM_E_SUCCESS;
    }

    case TC_REF_INT32: {
      CODE_COVERAGE_UNTESTED(374); // Not hit
      // 32-bit numbers are out of the range of supported array indexes
      return MVM_E_RANGE_ERROR;
    }

    case TC_REF_STRING: {
      CODE_COVERAGE_UNTESTED(375); // Not hit

      // In Microvium at the moment, it's illegal to use an integer-valued
      // string as a property name. If the string is in bytecode, it will only
      // have the type TC_REF_STRING if it's a number and is illegal.
      if (VM_IS_PGM_P(*value)) {
        CODE_COVERAGE_UNTESTED(376); // Not hit
        return MVM_E_TYPE_ERROR;
      } else {
        CODE_COVERAGE_UNTESTED(377); // Not hit
      }

      // Strings which have all digits are illegal as property names
      if (vm_stringIsNonNegativeInteger(vm, *value)) {
        CODE_COVERAGE_UNTESTED(378); // Not hit
        return MVM_E_TYPE_ERROR;
      } else {
        CODE_COVERAGE_UNTESTED(379); // Not hit
      }

      // Strings need to be converted to unique strings in order to be valid
      // property names. This is because properties are searched by reference
      // equality.
      *value = toUniqueString(vm, *value);
      return MVM_E_SUCCESS;
    }
    default: {
      CODE_COVERAGE_UNTESTED(380); // Not hit
      return MVM_E_TYPE_ERROR;
    }
  }
}

// Converts a TC_REF_STRING to a TC_REF_UNIQUE_STRING
// TODO: Test cases for this function
static Value toUniqueString(VM* vm, Value value) {
  CODE_COVERAGE_UNTESTED(51); // Not hit
  VM_ASSERT(vm, deepTypeOf(vm, value) == TC_REF_STRING);
  VM_ASSERT(vm, VM_IS_GC_P(value));

  // TC_REF_STRING values are always in GC memory. If they were in flash, they'd
  // already be TC_REF_UNIQUE_STRING.
  char* str1Data = (char*)gc_deref(vm, value);
  uint16_t str1Header = vm_readHeaderWord(vm, value);
  int str1Size = vm_paramOfHeaderWord(str1Header);

  MVM_PROGMEM_P pBytecode = vm->pBytecode;

  // We start by searching the string table for unique strings that are baked
  // into the ROM. These are stored alphabetically, so we can perform a binary
  // search.

  BO_t stringTableOffset = VM_READ_BC_2_HEADER_FIELD(stringTableOffset, pBytecode);
  uint16_t stringTableSize = VM_READ_BC_2_HEADER_FIELD(stringTableSize, pBytecode);
  int strCount = stringTableSize / sizeof (Value);

  int first = 0;
  int last = strCount;
  int middle = (first + last) / 2;

  while (first <= last) {
    CODE_COVERAGE_UNTESTED(381); // Not hit
    BO_t str2Offset = stringTableOffset + middle * 2;
    Value str2Value = VM_READ_BC_2_AT(str2Offset, pBytecode);
    VM_ASSERT(vm, VM_IS_PGM_P(str2Value));
    uint16_t str2Header = vm_readHeaderWord(vm, str2Value);
    int str2Size = vm_paramOfHeaderWord(str2Header);
    MVM_PROGMEM_P str2Data = pgm_deref(vm, str2Value);
    int compareSize = str1Size < str2Size ? str1Size : str2Size;
    // TODO: this function can probably be simplified if it uses vm_memcmp
    int c = memcmp_pgm(str1Data, str2Data, compareSize);

    // If they compare equal for the range that they have in common, we check the length
    if (c == 0) {
      CODE_COVERAGE_UNTESTED(382); // Not hit
      if (str1Size < str2Size) {
        CODE_COVERAGE_UNTESTED(383); // Not hit
        c = -1;
      } else if (str1Size > str2Size) {
        CODE_COVERAGE_UNTESTED(384); // Not hit
        c = 1;
      } else {
        CODE_COVERAGE_UNTESTED(385); // Not hit
        // Exact match
        return str2Value;
      }
    }

    // c is > 0 if the string we're searching for comes after the middle point
    if (c > 0) {
      CODE_COVERAGE_UNTESTED(386); // Not hit
      first = middle + 1;
    } else {
      CODE_COVERAGE_UNTESTED(387); // Not hit
      last = middle - 1;
    }

    middle = (first + last) / 2;
  }

  // At this point, we haven't found the unique string in the bytecode. We need
  // to check in RAM. Now we're comparing an in-RAM string against other in-RAM
  // strings, so it's using gc_deref instead of pgm_deref, and memcmp instead of
  // memcmp_pgm. Also, we're looking for an exact match, not performing a binary
  // search with inequality comparison, since the linked list of unique strings
  // in RAM is not sorted.
  Pointer vpCell = vm->uniqueStrings;
  TsUniqueStringCell* pCell;
  while (vpCell != VM_VALUE_NULL) {
    CODE_COVERAGE_UNTESTED(388); // Not hit
    pCell = gc_deref(vm, vpCell);
    Value str2Value = pCell->str;
    uint16_t str2Header = vm_readHeaderWord(vm, str2Value);
    int str2Size = vm_paramOfHeaderWord(str2Header);
    MVM_PROGMEM_P str2Data = gc_deref(vm, str2Value);

    // The sizes have to match for the strings to be equal
    if (str2Size == str1Size) {
      CODE_COVERAGE_UNTESTED(389); // Not hit
      // Note: we use memcmp instead of strcmp because strings are allowed to
      // have embedded null terminators.
      int c = memcmp(str1Data, str2Data, str1Size);
      // Equal?
      if (c == 0) {
        CODE_COVERAGE_UNTESTED(390); // Not hit
        return str2Value;
      } else {
        CODE_COVERAGE_UNTESTED(391); // Not hit
      }
    }
    vpCell = pCell->next;
  }

  // If we get here, it means there was no matching unique string already
  // existing in ROM or RAM. We upgrade the current string to a
  // TC_REF_UNIQUE_STRING, since we now know it doesn't conflict with any existing
  // existing unique strings.
  str1Header = str1Size | (TC_REF_UNIQUE_STRING << 12);
  ((uint16_t*)str1Data)[-1] = str1Header; // Overwrite the header

  // Add the string to the linked list of unique strings
  int cellSize = sizeof (TsUniqueStringCell);
  vpCell = gc_allocateWithHeader(vm, cellSize, TC_REF_NONE, cellSize, (void**)&pCell);
  // Push onto linked list
  pCell->next = vm->uniqueStrings;
  pCell->str = value;
  vm->uniqueStrings = vpCell;

  return value;

  // TODO: We need the GC to collect unique strings from RAM
}

// Same semantics as [memcmp](http://www.cplusplus.com/reference/cstring/memcmp/)
// but the second argument is a program memory pointer
static int memcmp_pgm(void* p1, MVM_PROGMEM_P p2, size_t size) {
  CODE_COVERAGE_UNTESTED(52); // Not hit
  while (size) {
    CODE_COVERAGE_UNTESTED(392); // Not hit
    char c1 = *((uint8_t*)p1);
    char c2 = MVM_READ_PROGMEM_1(p2);
    p1 = (void*)((uint8_t*)p1 + 1);
    p2 = MVM_PROGMEM_P_ADD(p2, 1);
    size--;
    if (c1 == c2) {
      CODE_COVERAGE_UNTESTED(393); // Not hit
      continue;
    } else if (c1 < c2) {
      CODE_COVERAGE_UNTESTED(394); // Not hit
      return -1;
    } else {
      CODE_COVERAGE_UNTESTED(395); // Not hit
      return 1;
    }
  }
  // If it's got this far, then all the bytes are equal
  return 0;
}

// Same semantics as [memcmp](http://www.cplusplus.com/reference/cstring/memcmp/)
// but operates on just program memory pointers
static int memcmp_pgm2(MVM_PROGMEM_P p1, MVM_PROGMEM_P p2, size_t size) {
  CODE_COVERAGE_UNTESTED(466); // Not hit
  while (size) {
    CODE_COVERAGE_UNTESTED(467); // Not hit
    char c1 = MVM_READ_PROGMEM_1(p1);
    char c2 = MVM_READ_PROGMEM_1(p2);
    p1 = MVM_PROGMEM_P_ADD(p1, 1);
    p2 = MVM_PROGMEM_P_ADD(p2, 1);
    size--;
    if (c1 == c2) {
      CODE_COVERAGE_UNTESTED(468); // Not hit
      continue;
    } else if (c1 < c2) {
      CODE_COVERAGE_UNTESTED(469); // Not hit
      return -1;
    } else {
      CODE_COVERAGE_UNTESTED(470); // Not hit
      return 1;
    }
  }
  // If it's got this far, then all the bytes are equal
  return 0;
}

// Same semantics as [memcmp](http://www.cplusplus.com/reference/cstring/memcmp/)
// but for VM pointers
static int vm_memcmp(VM* vm, Pointer a, Pointer b, uint16_t size) {
  CODE_COVERAGE_UNTESTED(471); // Not hit
  if (VM_IS_PGM_P(a)) {
    CODE_COVERAGE_UNTESTED(472); // Not hit
    if (VM_IS_PGM_P(b)) {
      CODE_COVERAGE_UNTESTED(473); // Not hit
      return memcmp_pgm2(pgm_deref(vm, a), pgm_deref(vm, b), size);
    } else {
      CODE_COVERAGE_UNTESTED(474); // Not hit
      return -memcmp_pgm(vm_deref(vm, b), pgm_deref(vm, a), size);
    }
  } else {
    CODE_COVERAGE_UNTESTED(478); // Not hit
    if (VM_IS_PGM_P(b)) {
      CODE_COVERAGE_UNTESTED(479); // Not hit
      return memcmp_pgm(vm_deref(vm, a), pgm_deref(vm, b), size);
    } else {
      CODE_COVERAGE_UNTESTED(480); // Not hit
      return memcmp(vm_deref(vm, a), vm_deref(vm, a), size);
    }
  }
}

static MVM_PROGMEM_P pgm_deref(VM* vm, Pointer vp) {
  VM_ASSERT(vm, VM_IS_PGM_P(vp));
  return MVM_PROGMEM_P_ADD(vm->pBytecode, VM_VALUE_OF(vp));
}

/** Size of string excluding bonus null terminator */
static uint16_t vm_stringSizeUtf8(VM* vm, Value stringValue) {
  CODE_COVERAGE_UNTESTED(53); // Not hit
  vm_HeaderWord headerWord = vm_readHeaderWord(vm, stringValue);
  #if MVM_SAFE_MODE
    TeTypeCode typeCode = vm_typeCodeFromHeaderWord(headerWord);
    VM_ASSERT(vm, (typeCode == TC_REF_STRING) || (typeCode == TC_REF_UNIQUE_STRING));
  #endif
  return vm_paramOfHeaderWord(headerWord) - 1;
}

static Value uintToStr(VM* vm, uint16_t n) {
  CODE_COVERAGE_UNTESTED(54); // Not hit
  char buf[8];
  char* c = &buf[sizeof buf];
  // Null terminator
  c--; *c = 0;
  // Convert to string
  // TODO: Test this
  while (n) {
    CODE_COVERAGE_UNTESTED(396); // Not hit
    c--;
    *c = n % 10;
    n /= 10;
  }
  if (c < buf) {
    CODE_COVERAGE_UNTESTED(397); // Not hit
    VM_UNEXPECTED_INTERNAL_ERROR(vm);
  }

  uint8_t len = (uint8_t)(buf + sizeof buf - c);
  char* data;
  // Allocation includes the null terminator
  Value result = gc_allocateWithHeader(vm, len, TC_REF_STRING, len, (void**)&data);
  memcpy(data, c, len);

  return result;
}

/**
 * Checks if a string contains only decimal digits (and is not empty). May only
 * be called on TC_REF_STRING and only those in GC memory.
 */
static bool vm_stringIsNonNegativeInteger(VM* vm, Value str) {
  CODE_COVERAGE_UNTESTED(55); // Not hit
  VM_ASSERT(vm, deepTypeOf(vm, str) == TC_REF_STRING);
  VM_ASSERT(vm, VM_IS_GC_P(str));

  char* data = gc_deref(vm, str);
  // Length excluding bonus null terminator
  uint16_t len = (((uint16_t*)data)[-1] & 0xFFF) - 1;
  if (!len) return false;
  while (len--) {
    CODE_COVERAGE_UNTESTED(398); // Not hit
    if (!isdigit(*data++)) {
      CODE_COVERAGE_UNTESTED(399); // Not hit
      return false;
    } else {
      CODE_COVERAGE_UNTESTED(400); // Not hit
    }
  }
  return true;
}

TeError toInt32Internal(mvm_VM* vm, mvm_Value value, int32_t* out_result) {
  CODE_COVERAGE(56); // Hit
  // TODO: when the type codes are more stable, we should convert these to a table.
  *out_result = 0;
  TeTypeCode type = deepTypeOf(vm, value);
  MVM_SWITCH_CONTIGUOUS(type, TC_END - 1) {
    MVM_CASE_CONTIGUOUS(TC_VAL_INT14):
    MVM_CASE_CONTIGUOUS(TC_REF_INT32): {
      CODE_COVERAGE(401); // Hit
      *out_result = vm_readInt32(vm, type, value);
      return MVM_E_SUCCESS;
    }
    MVM_CASE_CONTIGUOUS(TC_REF_FLOAT64): {
      CODE_COVERAGE(402); // Hit
      return MVM_E_FLOAT64;
    }
    MVM_CASE_CONTIGUOUS(TC_REF_STRING): {
      CODE_COVERAGE_UNIMPLEMENTED(403); // Not hit
      VM_NOT_IMPLEMENTED(vm); break;
    }
    MVM_CASE_CONTIGUOUS(TC_REF_UNIQUE_STRING): {
      CODE_COVERAGE_UNIMPLEMENTED(404); // Not hit
      VM_NOT_IMPLEMENTED(vm); break;
    }
    MVM_CASE_CONTIGUOUS(TC_REF_PROPERTY_LIST): {
      CODE_COVERAGE_UNTESTED(405); // Not hit
      return MVM_E_NAN;
    }
    MVM_CASE_CONTIGUOUS(TC_REF_LIST): {
      CODE_COVERAGE_UNTESTED(406); // Not hit
      return MVM_E_NAN;
    }
    MVM_CASE_CONTIGUOUS(TC_REF_TUPLE): {
      CODE_COVERAGE_UNTESTED(407); // Not hit
      return MVM_E_NAN;
    }
    MVM_CASE_CONTIGUOUS(TC_REF_FUNCTION): {
      CODE_COVERAGE_UNTESTED(408); // Not hit
      return MVM_E_NAN;
    }
    MVM_CASE_CONTIGUOUS(TC_REF_HOST_FUNC): {
      CODE_COVERAGE_UNTESTED(409); // Not hit
      return MVM_E_NAN;
    }
    MVM_CASE_CONTIGUOUS(TC_REF_STRUCT): {
      CODE_COVERAGE_UNTESTED(410); // Not hit
      return MVM_E_NAN;
    }
    MVM_CASE_CONTIGUOUS(TC_REF_BIG_INT): {
      CODE_COVERAGE_UNTESTED(411); // Not hit
      VM_RESERVED(vm); break;
    }
    MVM_CASE_CONTIGUOUS(TC_REF_SYMBOL): {
      CODE_COVERAGE_UNTESTED(412); // Not hit
      return MVM_E_NAN;
    }
    MVM_CASE_CONTIGUOUS(TC_VAL_UNDEFINED): {
      CODE_COVERAGE_UNTESTED(413); // Not hit
      return MVM_E_NAN;
    }
    MVM_CASE_CONTIGUOUS(TC_VAL_NULL): {
      CODE_COVERAGE_UNTESTED(414); // Not hit
      break;
    }
    MVM_CASE_CONTIGUOUS(TC_VAL_TRUE): {
      CODE_COVERAGE_UNTESTED(415); // Not hit
      *out_result = 1; break;
    }
    MVM_CASE_CONTIGUOUS(TC_VAL_FALSE): {
      CODE_COVERAGE_UNTESTED(416); // Not hit
      break;
    }
    MVM_CASE_CONTIGUOUS(TC_VAL_NAN): {
      CODE_COVERAGE(417); // Hit
      return MVM_E_NAN;
    }
    MVM_CASE_CONTIGUOUS(TC_VAL_NEG_ZERO): {
      CODE_COVERAGE(418); // Hit
      return MVM_E_NEG_ZERO;
    }
    MVM_CASE_CONTIGUOUS(TC_VAL_DELETED): {
      CODE_COVERAGE_UNTESTED(419); // Not hit
      return MVM_E_NAN;
    }
  }
  return MVM_E_SUCCESS;
}

int32_t mvm_toInt32(mvm_VM* vm, mvm_Value value) {
  CODE_COVERAGE_UNTESTED(57); // Not hit
  int32_t result;
  TeError err = toInt32Internal(vm, value, &result);
  if (result == MVM_E_SUCCESS) {
    CODE_COVERAGE_UNTESTED(420); // Not hit
    return result;
  } else if (result == MVM_E_NAN) {
    CODE_COVERAGE_UNTESTED(421); // Not hit
    return 0;
  } else if (result == MVM_E_NEG_ZERO) {
    CODE_COVERAGE_UNTESTED(422); // Not hit
    return 0;
  } else {
    CODE_COVERAGE_UNTESTED(423); // Not hit
  }

  // Fall back to long conversion
  VM_ASSERT(vm, deepTypeOf(vm, value) == TC_REF_FLOAT64);
  MVM_FLOAT64 f;
  vm_readMem(vm, &f, value, sizeof f);
  return (int32_t)f;
}

MVM_FLOAT64 mvm_toFloat64(mvm_VM* vm, mvm_Value value) {
  CODE_COVERAGE(58); // Hit
  int32_t result;
  TeError err = toInt32Internal(vm, value, &result);
  if (err == MVM_E_SUCCESS) {
    CODE_COVERAGE(424); // Hit
    return result;
  } else if (err == MVM_E_NAN) {
    CODE_COVERAGE(425); // Hit
    return MVM_FLOAT64_NAN;
  } else if (err == MVM_E_NEG_ZERO) {
    CODE_COVERAGE(426); // Hit
    return -0.0;
  } else {
    CODE_COVERAGE(427); // Hit
  }

  VM_ASSERT(vm, deepTypeOf(vm, value) == TC_REF_FLOAT64);
  MVM_FLOAT64 f;
  vm_readMem(vm, &f, value, sizeof f);
  return f;
}

bool mvm_equal(mvm_VM* vm, mvm_Value a, mvm_Value b) {
  CODE_COVERAGE_UNTESTED(462); // Not hit

  // TODO: NaN and negative zero equality

  if (a == b) {
    CODE_COVERAGE_UNTESTED(463); // Not hit
    return true;
  }

  TeTypeCode aType = deepTypeOf(vm, a);
  TeTypeCode bType = deepTypeOf(vm, b);
  if (aType != bType) {
    CODE_COVERAGE_UNTESTED(464); // Not hit
    return false;
  }

  TABLE_COVERAGE(aType, TC_END, 465); // Not hit

  // Some types compare with value equality, so we do memory equality check
  if ((aType == TC_REF_INT32) || (aType == TC_REF_FLOAT64) || (aType == TC_REF_BIG_INT)) {
    CODE_COVERAGE_UNTESTED(475); // Not hit
    vm_HeaderWord aHeaderWord = vm_readHeaderWord(vm, a);
    vm_HeaderWord bHeaderWord = vm_readHeaderWord(vm, b);
    // If the header words are different, the sizes are different
    if (aHeaderWord != bHeaderWord) {
      CODE_COVERAGE_UNTESTED(476); // Not hit
      return false;
    }
    CODE_COVERAGE_UNTESTED(477); // Not hit
    uint16_t size = vm_paramOfHeaderWord(aHeaderWord);
    if (vm_memcmp(vm, a, b, size) == 0) {
      CODE_COVERAGE_UNTESTED(481); // Not hit
      return true;
    } else {
      CODE_COVERAGE_UNTESTED(482); // Not hit
      return false;
    }
  } else {
    // All other types compare with reference equality, which we've already checked
    return false;
  }
}