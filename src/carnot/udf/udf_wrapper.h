#pragma once

#include <arrow/array.h>
#include <glog/logging.h>

#include <memory>
#include <string>
#include <vector>

#include "src/carnot/udf/arrow_adapter.h"
#include "src/carnot/udf/column_wrapper.h"
#include "src/carnot/udf/udf.h"
#include "src/common/error.h"
#include "src/common/statusor.h"

namespace pl {
namespace carnot {
namespace udf {

// This is the size we assume for an "average" string. If this is too large we waste
// memory, if too small we spend more time allocating. Since we use exponential doubling
// it's better to keep this number small.
const int kStringAssumedSizeHeuristic = 10;

// This function takes in a generic UDFBaseValue and then converts it to actual
// UDFValue type. This function is unsafe and will produce wrong results (or crash)
// if used incorrectly.
template <udf::UDFDataType TExecArgType>
constexpr auto CastToUDFValueType(const UDFBaseValue *arg) {
  // A sample transformation (for TExecArgType = UDFDataType::INT64) is:
  // return reinterpret_cast<Int64Value*>(arg);
  return reinterpret_cast<const typename UDFDataTypeTraits<TExecArgType>::udf_value_type *>(arg);
}
/**
 * This is the inner wrapper which expands the arguments an performs type casts
 * based on the type and arity of the input arguments.
 *
 * This function takes calls the Exec function of the UDF after type casting all the
 * input values. The function is called once for each row of the input batch.
 *
 * @return Status of execution.
 */
template <typename TUDF, typename TOutput, std::size_t... I>
Status ExecWrapper(TUDF *udf, FunctionContext *ctx, size_t count, TOutput *out,
                   const std::vector<const UDFBaseValue *> &args, std::index_sequence<I...>) {
  constexpr auto exec_argument_types = ScalarUDFTraits<TUDF>::ExecArguments();
  for (size_t idx = 0; idx < count; ++idx) {
    out[idx] = udf->Exec(ctx, CastToUDFValueType<exec_argument_types[I]>(args[I])[idx]...);
  }
  return Status::OK();
}

// The get value functions pluck out value at a specific index.
template <typename T>
inline auto GetValue(const T *arr, int64_t idx) {
  return arr->Value(idx);
}

// Specialization for string type.
template <>
inline auto GetValue<arrow::StringArray>(const arrow::StringArray *arr, int64_t idx) {
  return arr->GetString(idx);
}

// Returns the underlying data from a UDF value.
template <typename T>
inline auto UnWrap(const T &v) {
  return v.val;
}

template <>
inline auto UnWrap<StringValue>(const StringValue &s) {
  return s;
}

// This function takes in a generic arrow::Array and then converts it to actual
// specific arrow::Array subtype. This function is unsafe and will produce wrong results (or crash)
// if used incorrectly.
template <udf::UDFDataType TExecArgType>
constexpr auto GetValueFromArrowArray(const arrow::Array *arg, int64_t idx) {
  // A sample transformation (for TExecArgType = UDFDataType::INT64) is:
  // return GetValue(reinterpret_cast<arrow::Int64Array*>(arg), idx);
  using arrow_array_type = typename UDFDataTypeTraits<TExecArgType>::arrow_array_type;
  return GetValue(reinterpret_cast<const arrow_array_type *>(arg), idx);
}

/**
 * This is the inner wrapper for the arrow type.
 * This performs type casting and storing the data in the output builder.
 */
template <typename TUDF, typename TOutput, std::size_t... I>
Status ExecWrapperArrow(TUDF *udf, FunctionContext *ctx, size_t count, TOutput *out,
                        const std::vector<arrow::Array *> &args, std::index_sequence<I...>) {
  constexpr auto exec_argument_types = ScalarUDFTraits<TUDF>::ExecArguments();
  CHECK(out->Reserve(count).ok());
  size_t reserved = count * kStringAssumedSizeHeuristic;
  size_t total_size = 0;
  // If it's a string type we also need to allocate memory for the data.
  // This actually applies to all non-fixed data allocations.
  // PL_CARNOT_UPDATE_FOR_NEW_TYPES.
  if constexpr (std::is_same_v<arrow::StringBuilder, TOutput>) {
    CHECK(out->ReserveData(reserved).ok());
  }
  for (size_t idx = 0; idx < count; ++idx) {
    auto res =
        UnWrap(udf->Exec(ctx, GetValueFromArrowArray<exec_argument_types[I]>(args[I], idx)...));

    // We use doubling to make sure we minimize the number of allocations.
    // PL_CARNOT_UPDATE_FOR_NEW_TYPES.
    if constexpr (std::is_same_v<arrow::StringBuilder, TOutput>) {
      total_size += res.size();
      while (total_size >= reserved) {
        reserved *= 2;
        PL_RETURN_IF_ERROR(out->ReserveData(reserved));
      }
    }
    // This function is "safe" now because we manually allocated memory.
    out->UnsafeAppend(res);
  }
  return Status::OK();
}

/**
 * Checks types between column wrapper and array of UDFDataTypes.
 * @return true if all types match.
 */
template <std::size_t SIZE>
inline bool CheckTypes(const std::vector<const ColumnWrapper *> &args,
                       const std::array<UDFDataType, SIZE> &types) {
  if (args.size() != SIZE) {
    return false;
  }
  for (size_t idx = 0; idx < args.size(); ++idx) {
    if (args[idx]->DataType() != types[idx]) {
      return false;
    }
  }
  return true;
}

inline std::vector<const UDFBaseValue *> ConvertToBaseValue(
    const std::vector<const ColumnWrapper *> &args) {
  std::vector<const UDFBaseValue *> retval;
  for (const auto *col : args) {
    retval.push_back(col->UnsafeRawData());
  }
  return retval;
}

/**
 * This struct is used to provide a set of static methods that wrap the init, and
 * exec methods of ScalarUDFs.
 *
 * @tparam TUDF A Scalar UDF.
 */
template <typename TUDF>
struct ScalarUDFWrapper {
  static std::unique_ptr<ScalarUDF> Make() { return std::make_unique<TUDF>(); }

  /**
   * Provides a method that executes the tempalated UDF on a batch of inputs.
   * The input batches are represented as vector of arrow:array pointers.
   *
   * This expects all the inputs and outputs to be allocated and of the appropriate
   * type. This function is unsafe and will perform unsafe casts and using an incorrect
   * type will result in a crash!
   *
   * @note This function and underlying templates are fully expanded at compile time.
   *
   * @param udf a pointer to the UDF.
   * @param ctx The function context.
   * @param inputs A vector of arrow::array* of inputs to the udf.
   * @param output The output builder.
   * @param count The number of elements in the input and out (these need to be the same).
   * @return Status of execution.
   */
  static Status ExecBatchArrow(ScalarUDF *udf, FunctionContext *ctx,
                               const std::vector<arrow::Array *> &inputs,
                               arrow::ArrayBuilder *output, int count) {
    constexpr types::DataType return_type = ScalarUDFTraits<TUDF>::ReturnType();
    auto exec_argument_types = ScalarUDFTraits<TUDF>::ExecArguments();

    // Check that output is allocated.
    DCHECK(output != nullptr);
    // Check that the arity is correct.
    DCHECK(inputs.size() == ScalarUDFTraits<TUDF>::ExecArguments().size());

    // The outer wrapper just casts the output type and UDF type. We then pass in
    // the inputs with a sequence based on the number of arguments to iterate through and
    // cast the inputs.
    return ExecWrapperArrow<TUDF>(
        reinterpret_cast<TUDF *>(udf), ctx, count,
        reinterpret_cast<typename UDFDataTypeTraits<return_type>::arrow_builder_type *>(output),
        inputs, std::make_index_sequence<exec_argument_types.size()>{});
  }

  /**
   * Provides a method that executes the tempalated UDF on a batch of inputs.
   * The input batches are represented as vector of vectors to the inputs.
   *
   * This expects all the inputs and outputs to be allocated and of the appropriate
   * type. This function is unsafe and will perform unsafe casts and using an incorrect
   * type will result in a crash!
   *
   * @note This function and underlying templates are fully expanded at compile time.
   *
   * @param udf a pointer to the UDF.
   * @param ctx The function context.
   * @param inputs An array of inputs to the udf.
   * @param output Pointer to the start of the output.
   * @param count The number of elements in the input and out (these need to be the same).
   * @return Status of execution.
   */
  static Status ExecBatch(ScalarUDF *udf, FunctionContext *ctx,
                          const std::vector<const ColumnWrapper *> &inputs, ColumnWrapper *output,
                          int count) {
    // Check that output is allocated.
    DCHECK(output != nullptr);
    // Check that the arity is correct.
    DCHECK(inputs.size() == ScalarUDFTraits<TUDF>::ExecArguments().size());

    constexpr types::DataType return_type = ScalarUDFTraits<TUDF>::ReturnType();
    auto exec_argument_types = ScalarUDFTraits<TUDF>::ExecArguments();
    DCHECK(CheckTypes(inputs, exec_argument_types));
    auto input_as_base_value = ConvertToBaseValue(inputs);

    using output_type = typename UDFDataTypeTraits<return_type>::udf_value_type;
    auto *casted_output = reinterpret_cast<output_type *>(output->UnsafeRawData());
    // The outer wrapper just casts the output type and UDF type. We then pass in
    // the inputs with a sequence based on the number of arguments to iterate through and
    // cast the inputs.
    return ExecWrapper<TUDF>(reinterpret_cast<TUDF *>(udf), ctx, count, casted_output,
                             input_as_base_value,
                             std::make_index_sequence<exec_argument_types.size()>{});
  }
};

/**
 * Performs an update on a batch of records.
 * This is similar to the ExecBatch, except it does not store a return value.
 */
template <typename TUDA, std::size_t... I>
Status UpdateWrapper(TUDA *uda, FunctionContext *ctx, size_t count,
                     const std::vector<const UDFBaseValue *> &args, std::index_sequence<I...>) {
  constexpr auto update_argument_types = UDATraits<TUDA>::UpdateArgumentTypes();
  for (size_t idx = 0; idx < count; ++idx) {
    uda->Update(ctx, CastToUDFValueType<update_argument_types[I]>(args[I])[idx]...);
  }
  return Status::OK();
}

/**
 * Performs an update on a batch of records (arrow).
 * This is similar to the ExecBatch, except it does not store a return value.
 */
template <typename TUDA, std::size_t... I>
Status UpdateWrapperArrow(TUDA *uda, FunctionContext *ctx, size_t count,
                          const std::vector<const arrow::Array *> &args,
                          std::index_sequence<I...>) {
  constexpr auto update_argument_types = UDATraits<TUDA>::UpdateArgumentTypes();
  for (size_t idx = 0; idx < count; ++idx) {
    uda->Update(ctx, GetValueFromArrowArray<update_argument_types[I]>(args[I], idx)...);
  }
  return Status::OK();
}

/**
 * Provides a set of static methods that wrap UDAs and allow vectorized execution (for update).
 * @tparam TUDA The UDA class.
 */
template <typename TUDA>
struct UDAWrapper {
  static constexpr types::DataType return_type = UDATraits<TUDA>::FinalizeReturnType();

  /**
   * Create a new UDA.
   * @return A unique_ptr to the UDA instance.
   */
  static std::unique_ptr<UDA> Make() { return std::make_unique<TUDA>(); }

  /**
   * Perform a batch update of the passed in UDA based in the inputs.
   * @param uda The UDA instances.
   * @param ctx The function context.
   * @param inputs A vector of pointers to ColumnWrappers.
   * @return Status of update.
   */
  static Status ExecBatchUpdate(UDA *uda, FunctionContext *ctx,
                                const std::vector<const ColumnWrapper *> &inputs) {
    constexpr auto update_argument_types = UDATraits<TUDA>::UpdateArgumentTypes();
    DCHECK(CheckTypes(inputs, update_argument_types));
    DCHECK(inputs.size() == update_argument_types.size());

    auto input_as_base_value = ConvertToBaseValue(inputs);

    size_t num_records = inputs[0]->Size();
    return UpdateWrapper<TUDA>(reinterpret_cast<TUDA *>(uda), ctx, num_records, input_as_base_value,
                               std::make_index_sequence<update_argument_types.size()>{});
  }

  /**
   * Perform a batch update of the passed in UDA based in the inputs.
   * @param uda The UDA instances.
   * @param ctx The function context.
   * @param inputs A vector of pointers to ColumnWrappers.
   * @return Status of update.
   */
  static Status ExecBatchUpdateArrow(UDA *uda, FunctionContext *ctx,
                                     const std::vector<const arrow::Array *> &inputs) {
    constexpr auto update_argument_types = UDATraits<TUDA>::UpdateArgumentTypes();
    DCHECK(inputs.size() == update_argument_types.size());

    size_t num_records = inputs[0]->length();
    return UpdateWrapperArrow<TUDA>(reinterpret_cast<TUDA *>(uda), ctx, num_records, inputs,
                                    std::make_index_sequence<update_argument_types.size()>{});
  }

  /**
   * Merges uda2 into uda1 based on the UDA merge function.
   * Both UDAs must be the same time, undefined behavior (or crash) if they are different types
   * or not the same type as the templated UDA.
   * @return Status of Merge.
   */
  static Status Merge(UDA *uda1, UDA *uda2, FunctionContext *ctx) {
    reinterpret_cast<TUDA *>(uda1)->Merge(ctx, *reinterpret_cast<TUDA *>(uda2));
    return Status::OK();
  }

  /**
   * Finalize the UDA into an arrow builder. The arrow builder needs to be correct type
   * for the finalize return type.
   * @return Status of the finalize.
   */
  static Status FinalizeArrow(UDA *uda, FunctionContext *ctx, arrow::ArrayBuilder *output) {
    DCHECK(output != nullptr);
    auto *casted_builder =
        reinterpret_cast<typename UDFDataTypeTraits<return_type>::arrow_builder_type *>(output);
    auto *casted_uda = reinterpret_cast<TUDA *>(uda);
    PL_RETURN_IF_ERROR(casted_builder->Append(UnWrap(casted_uda->Finalize(ctx))));
    return Status::OK();
  }

  /**
   * Finalize the UDA into an UDA value type.
   * Unsafe casts are performed, so the passed in output UDA value must be of the correct
   * finalize return type.
   *
   * @return Status of the Finalize.
   */
  static Status FinalizeValue(UDA *uda, FunctionContext *ctx, UDFBaseValue *output) {
    using output_type = typename UDFDataTypeTraits<return_type>::udf_value_type;

    DCHECK(output != nullptr);
    auto *casted_output = reinterpret_cast<output_type *>(output);
    auto *casted_uda = reinterpret_cast<TUDA *>(uda);
    *casted_output = casted_uda->Finalize(ctx);
    return Status::OK();
  }
};

}  // namespace udf
}  // namespace carnot
}  // namespace pl
