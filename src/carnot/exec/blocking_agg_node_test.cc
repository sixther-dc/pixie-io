#include <gtest/gtest.h>

#include <algorithm>

#include "src/carnot/exec/blocking_agg_node.h"
#include "src/carnot/exec/exec_node_mock.h"
#include "src/carnot/proto/test_proto.h"
#include "src/carnot/udf/arrow_adapter.h"

namespace pl {
namespace carnot {
namespace exec {

using testing::_;
using udf::FunctionContext;
using udf::Int64Value;

// Test UDA, takes the min of two arguments and then sums them.
// TODO(zasgar): move these all to a common file.
class MinSumUDA : public udf::UDA {
 public:
  void Update(udf::FunctionContext*, Int64Value arg1, Int64Value arg2) {
    sum_ = sum_.val + std::min(arg1.val, arg2.val);
  }
  void Merge(udf::FunctionContext*, const MinSumUDA& other) { sum_ = sum_.val + other.sum_.val; }
  Int64Value Finalize(udf::FunctionContext*) { return sum_; }

 protected:
  Int64Value sum_ = 0;
};

const char* kBlockingNoGroupAgg = R"(
op_type: BLOCKING_AGGREGATE_OPERATOR
blocking_agg_op {
  values {
    name: "minsum"
    args {
      column {
        node:0
        index: 0
      }
    }
    args {
      column {
        node:0
        index: 1
      }
    }
  }
  value_names: "value1"
})";

std::unique_ptr<ExecState> MakeTestExecState() {
  auto udf_registry = std::make_shared<udf::ScalarUDFRegistry>("test_registry");
  auto uda_registry = std::make_shared<udf::UDARegistry>("test_registry");
  auto table_store = std::make_shared<TableStore>();
  EXPECT_TRUE(uda_registry->Register<MinSumUDA>("minsum").ok());
  return std::make_unique<ExecState>(udf_registry, uda_registry, table_store);
}

std::unique_ptr<plan::Operator> PlanNodeFromPbtxt(const std::string& pbtxt) {
  carnotpb::Operator op_pb;
  EXPECT_TRUE(google::protobuf::TextFormat::MergeFromString(pbtxt, &op_pb));
  return plan::BlockingAggregateOperator::FromProto(op_pb, 1);
}

class BlockingAggNodeTest : public ::testing::Test {
 public:
  BlockingAggNodeTest() { exec_state_ = MakeTestExecState(); }
  RowBatch CreateInputRowBatch(const std::vector<udf::Int64Value>& in1,
                               const std::vector<udf::Int64Value>& in2) {
    RowDescriptor rd({udf::UDFDataType::INT64, udf::UDFDataType::INT64});
    RowBatch rb(rd, in1.size());
    PL_CHECK_OK(rb.AddColumn(ToArrow(in1, arrow::default_memory_pool())));
    PL_CHECK_OK(rb.AddColumn(ToArrow(in2, arrow::default_memory_pool())));
    return rb;
  }

 protected:
  std::unique_ptr<ExecState> exec_state_;
};

TEST_F(BlockingAggNodeTest, no_groups) {
  auto plan_node = PlanNodeFromPbtxt(kBlockingNoGroupAgg);
  RowDescriptor input_rd({udf::UDFDataType::INT64, udf::UDFDataType::INT64});

  RowDescriptor output_rd({udf::UDFDataType::INT64});
  BlockingAggNode aggn;
  MockExecNode mock_child_;
  aggn.AddChild(&mock_child_);

  PL_CHECK_OK(aggn.Init(*plan_node, output_rd, {input_rd}));
  PL_CHECK_OK(aggn.Prepare(exec_state_.get()));
  PL_CHECK_OK(aggn.Open(exec_state_.get()));

  auto rb1 = CreateInputRowBatch({1, 2, 3, 4}, {2, 5, 6, 8});
  auto rb2 = CreateInputRowBatch({5, 6, 3, 4}, {1, 5, 3, 8});
  rb2.set_eos(true);

  auto check_result_batch = [&](ExecState* exec_state, const RowBatch& child_rb) {
    EXPECT_EQ(exec_state_.get(), exec_state);
    ASSERT_EQ(1, child_rb.num_rows());
    ASSERT_EQ(1, child_rb.num_columns());
    auto output_col = child_rb.ColumnAt(0);
    auto casted = reinterpret_cast<arrow::Int64Array*>(output_col.get());
    EXPECT_EQ(23, casted->Value(0));
  };

  EXPECT_CALL(mock_child_, ConsumeNextImpl(_, _))
      .Times(1)
      .WillOnce(testing::DoAll(testing::Invoke(check_result_batch), testing::Return(Status::OK())));

  PL_CHECK_OK(aggn.ConsumeNext(exec_state_.get(), rb1));
  PL_CHECK_OK(aggn.ConsumeNext(exec_state_.get(), rb2));
  PL_CHECK_OK(aggn.Close(exec_state_.get()));
}

}  // namespace exec
}  // namespace carnot
}  // namespace pl
