#include "error_descriptions.hh"

std::map<mvm_TeError, std::string> errorDescriptions = {
  { MVM_E_SUCCESS, "MVM_E_SUCCESS" },
  { MVM_E_UNEXPECTED, "MVM_E_UNEXPECTED" },
  { MVM_E_MALLOC_FAIL, "MVM_E_MALLOC_FAIL" },
  { MVM_E_ALLOCATION_TOO_LARGE, "MVM_E_ALLOCATION_TOO_LARGE" },
  { MVM_E_INVALID_ADDRESS, "MVM_E_INVALID_ADDRESS" },
  { MVM_E_COPY_ACROSS_BUCKET_BOUNDARY, "MVM_E_COPY_ACROSS_BUCKET_BOUNDARY" },
  { MVM_E_FUNCTION_NOT_FOUND, "MVM_E_FUNCTION_NOT_FOUND" },
  { MVM_E_INVALID_HANDLE, "MVM_E_INVALID_HANDLE" },
  { MVM_E_STACK_OVERFLOW, "MVM_E_STACK_OVERFLOW" },
  { MVM_E_UNRESOLVED_IMPORT, "MVM_E_UNRESOLVED_IMPORT" },
  { MVM_E_ATTEMPT_TO_WRITE_TO_ROM, "MVM_E_ATTEMPT_TO_WRITE_TO_ROM" },
  { MVM_E_INVALID_ARGUMENTS, "MVM_E_INVALID_ARGUMENTS" },
  { MVM_E_TYPE_ERROR, "MVM_E_TYPE_ERROR" },
  { MVM_E_TARGET_NOT_CALLABLE, "MVM_E_TARGET_NOT_CALLABLE" },
  { MVM_E_HOST_ERROR, "MVM_E_HOST_ERROR" },
  { MVM_E_NOT_IMPLEMENTED, "MVM_E_NOT_IMPLEMENTED" },
  { MVM_E_HOST_RETURNED_INVALID_VALUE, "MVM_E_HOST_RETURNED_INVALID_VALUE" },
  { MVM_E_ASSERTION_FAILED, "MVM_E_ASSERTION_FAILED" },
  { MVM_E_INVALID_BYTECODE, "MVM_E_INVALID_BYTECODE" },
};