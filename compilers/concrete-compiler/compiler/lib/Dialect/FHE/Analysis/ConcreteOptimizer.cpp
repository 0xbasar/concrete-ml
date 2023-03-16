// Part of the Concrete Compiler Project, under the BSD3 License with Zama
// Exceptions. See
// https://github.com/zama-ai/concrete-compiler-internal/blob/main/LICENSE.txt
// for license information.

#include <chrono>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <vector>

#include "boost/outcome.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Pass/PassManager.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

#include "concrete-optimizer.hpp"

#include "concretelang/Common/Error.h"
#include "concretelang/Dialect/FHE/Analysis/ConcreteOptimizer.h"
#include "concretelang/Dialect/FHE/Analysis/utils.h"
#include "concretelang/Dialect/FHE/IR/FHEOps.h"
#include "concretelang/Dialect/FHE/IR/FHETypes.h"
#include "concretelang/Dialect/FHELinalg/IR/FHELinalgOps.h"
#include "concretelang/Dialect/Tracing/IR/TracingOps.h"
#include "concretelang/Support/V0Parameters.h"
#include "concretelang/Support/logging.h"

#define GEN_PASS_CLASSES
#include "concretelang/Dialect/FHE/Analysis/ConcreteOptimizer.h.inc"

namespace mlir {
namespace concretelang {
namespace optimizer {

namespace {

template <typename T> rust::Slice<const T> slice(const std::vector<T> &vec) {
  return rust::Slice<const T>(vec.data(), vec.size());
}

template <typename T> rust::Slice<const T> slice(const llvm::ArrayRef<T> &vec) {
  return rust::Slice<const T>(vec.data(), vec.size());
}

struct FunctionToDag {
  // Inputs of operators
  using Inputs = std::vector<concrete_optimizer::dag::OperatorIndex>;

  const double NEGLIGIBLE_COMPLEXITY = 0.0;

  mlir::func::FuncOp func;
  optimizer::Config config;
  llvm::DenseMap<mlir::Value, concrete_optimizer::dag::OperatorIndex> index;

  FunctionToDag(mlir::func::FuncOp func, optimizer::Config config)
      : func(func), config(config) {}

#define DEBUG(MSG)                                                             \
  if (mlir::concretelang::isVerbose()) {                                       \
    mlir::concretelang::log_verbose() << MSG << "\n";                          \
  }

  outcome::checked<std::optional<optimizer::Dag>,
                   ::concretelang::error::StringError>
  build() {
    auto dag = concrete_optimizer::dag::empty();
    // Converting arguments as Input
    for (auto &arg : func.getArguments()) {
      addArg(dag, arg);
    }
    // Converting ops
    for (auto &bb : func.getBody().getBlocks()) {
      for (auto &op : bb.getOperations()) {
        addOperation(dag, op);
      }
    }
    for (auto &bb : func.getBody().getBlocks()) {
      for (auto &op : bb.getOperations()) {
        op.removeAttr("SMANP");
      }
    }
    if (index.empty()) {
      // Dag is empty <=> classical function without encryption
      DEBUG("!!! concrete-optimizer: nothing to do in " << func.getName()
                                                        << "\n");
      return std::nullopt;
    };
    DEBUG(std::string(dag->dump()));
    return std::move(dag);
  }

  void addArg(optimizer::Dag &dag, mlir::Value &arg) {
    DEBUG("Arg " << arg << " " << arg.getType());
    if (!fhe::utils::isEncryptedValue(arg)) {
      return;
    }
    auto precision = fhe::utils::getEintPrecision(arg);
    auto shape = getShape(arg);
    auto opI = dag->add_input(precision, slice(shape));
    index[arg] = opI;
  }

  bool hasEncryptedResult(mlir::Operation &op) {
    for (auto val : op.getResults()) {
      if (fhe::utils::isEncryptedValue(val)) {
        return true;
      }
    }
    return false;
  }

  void addOperation(optimizer::Dag &dag, mlir::Operation &op) {
    DEBUG("Instr " << op);

    if (isReturn(op)) {
      // This op has no result
      return;
    }

    auto encrypted_inputs = encryptedInputs(op);
    if (!hasEncryptedResult(op)) {
      // This op is unrelated to FHE
      assert(encrypted_inputs.empty() ||
             mlir::isa<mlir::concretelang::Tracing::TraceCiphertextOp>(op));
      return;
    }
    assert(op.getNumResults() == 1);
    auto val = op.getResult(0);
    auto precision = fhe::utils::getEintPrecision(val);
    if (isLut(op)) {
      addLut(dag, val, encrypted_inputs, precision);
      return;
    }
    if (isRound(op)) {
      addRound(dag, val, encrypted_inputs, precision);
      return;
    }
    if (auto dot = asDot(op)) {
      auto weightsOpt = dotWeights(dot);
      if (weightsOpt) {
        addDot(dag, val, encrypted_inputs, weightsOpt.value());
        return;
      }
      // If can't find weights return default leveled op
      DEBUG("Replace Dot by LevelledOp on " << op);
    }
    if (auto mul = asMul(op)) {
      addMul(dag, mul, encrypted_inputs, precision);
      return;
    }
    if (auto mul = asMulTensor(op)) {
      addMulTensor(dag, mul, encrypted_inputs, precision);
      return;
    }
    if (auto max = asMax(op)) {
      addMax(dag, max, encrypted_inputs, precision);
      return;
    }
    if (auto maxpool2d = asMaxpool2d(op)) {
      addMaxpool2d(dag, maxpool2d, encrypted_inputs, precision);
      return;
    }
    // default
    addLevelledOp(dag, op, encrypted_inputs);
  }

  void addLut(optimizer::Dag &dag, mlir::Value &val, Inputs &encrypted_inputs,
              int precision) {
    assert(encrypted_inputs.size() == 1);
    // No need to distinguish different lut kind until we do approximate
    // paradigm on outputs
    auto encrypted_input = encrypted_inputs[0];
    std::vector<std::uint64_t> unknowFunction;
    index[val] =
        dag->add_lut(encrypted_input, slice(unknowFunction), precision);
  }

  void addRound(optimizer::Dag &dag, mlir::Value &val, Inputs &encrypted_inputs,
                int rounded_precision) {
    assert(encrypted_inputs.size() == 1);
    // No need to distinguish different lut kind until we do approximate
    // paradigm on outputs
    auto encrypted_input = encrypted_inputs[0];
    index[val] = dag->add_round_op(encrypted_input, rounded_precision);
  }

  void addDot(optimizer::Dag &dag, mlir::Value &val, Inputs &encrypted_inputs,
              std::vector<std::int64_t> &weights_vector) {
    assert(encrypted_inputs.size() == 1);
    auto weights = concrete_optimizer::weights::vector(slice(weights_vector));
    index[val] = dag->add_dot(slice(encrypted_inputs), std::move(weights));
  }

  std::string loc_to_string(mlir::Location location) {
    std::string loc;
    llvm::raw_string_ostream loc_stream(loc);
    location.print(loc_stream);
    return loc;
  }

  void addLevelledOp(optimizer::Dag &dag, mlir::Operation &op, Inputs &inputs) {
    auto val = op.getResult(0);
    auto out_shape = getShape(val);
    if (inputs.empty()) {
      // Trivial encrypted constants encoding
      // There are converted to input + levelledop
      auto precision = fhe::utils::getEintPrecision(val);
      auto opI = dag->add_input(precision, slice(out_shape));
      inputs.push_back(opI);
    }
    // Default complexity is negligible
    double fixed_cost = NEGLIGIBLE_COMPLEXITY;
    double lwe_dim_cost_factor = NEGLIGIBLE_COMPLEXITY;
    auto smanp_int = op.getAttrOfType<mlir::IntegerAttr>("SMANP");
    auto loc = loc_to_string(op.getLoc());
    assert(smanp_int && "Missing manp value on a crypto operation");
    // TODO: use APIFloat.sqrt when it's available
    double manp = sqrt(smanp_int.getValue().roundToDouble());
    auto comment = std::string(op.getName().getStringRef()) + " " + loc;
    index[val] =
        dag->add_levelled_op(slice(inputs), lwe_dim_cost_factor, fixed_cost,
                             manp, slice(out_shape), comment);
  }

  void addMul(optimizer::Dag &dag, FHE::MulEintOp &mulOp, Inputs &inputs,
              int precision) {

    // x * y = ((x + y)^2 / 4) - ((x - y)^2 / 4) == tlu(x + y) - tlu(x - y)

    mlir::Value result = mulOp.getResult();
    const std::vector<uint64_t> resultShape = getShape(result);

    Operation *xOp = mulOp.getA().getDefiningOp();
    Operation *yOp = mulOp.getB().getDefiningOp();

    const double fixedCost = NEGLIGIBLE_COMPLEXITY;
    const double lweDimCostFactor = NEGLIGIBLE_COMPLEXITY;

    llvm::APInt xSmanp = llvm::APInt{1, 1, false};
    if (xOp != nullptr) {
      const auto xSmanpAttr = xOp->getAttrOfType<mlir::IntegerAttr>("SMANP");
      assert(xSmanpAttr && "Missing SMANP value on a crypto operation");
      xSmanp = xSmanpAttr.getValue();
    }

    llvm::APInt ySmanp = llvm::APInt{1, 1, false};
    if (yOp != nullptr) {
      const auto ySmanpAttr = yOp->getAttrOfType<mlir::IntegerAttr>("SMANP");
      assert(ySmanpAttr && "Missing SMANP value on a crypto operation");
      ySmanp = ySmanpAttr.getValue();
    }

    auto loc = loc_to_string(mulOp.getLoc());
    auto comment = std::string(mulOp->getName().getStringRef()) + " " + loc;

    // (x + y) and (x - y)
    const double addSubManp =
        sqrt(xSmanp.roundToDouble() + ySmanp.roundToDouble());

    // tlu(v)
    const double tluManp = 1;

    // tlu(v1) - tlu(v2)
    const double tluSubManp = sqrt(tluManp + tluManp);

    // for tlus
    const std::vector<std::uint64_t> unknownFunction;

    // tlu(x + y)
    auto addNode =
        dag->add_levelled_op(slice(inputs), lweDimCostFactor, fixedCost,
                             addSubManp, slice(resultShape), comment);
    auto lhsTluNode = dag->add_lut(addNode, slice(unknownFunction), precision);

    // tlu(x - y)
    auto subNode =
        dag->add_levelled_op(slice(inputs), lweDimCostFactor, fixedCost,
                             addSubManp, slice(resultShape), comment);
    auto rhsTluNode = dag->add_lut(subNode, slice(unknownFunction), precision);

    // tlu(x + y) - tlu(x - y)
    const std::vector<concrete_optimizer::dag::OperatorIndex> subInputs = {
        lhsTluNode, rhsTluNode};
    index[result] =
        dag->add_levelled_op(slice(subInputs), lweDimCostFactor, fixedCost,
                             tluSubManp, slice(resultShape), comment);
  }

  void addMulTensor(optimizer::Dag &dag, FHELinalg::MulEintOp &mulOp,
                    Inputs &inputs, int precision) {

    // x * y = ((x + y)^2 / 4) - ((x - y)^2 / 4) == tlu(x + y) - tlu(x - y)

    mlir::Value result = mulOp.getResult();
    const std::vector<uint64_t> resultShape = getShape(result);

    Operation *xOp = mulOp.getLhs().getDefiningOp();
    Operation *yOp = mulOp.getRhs().getDefiningOp();

    const double fixedCost = NEGLIGIBLE_COMPLEXITY;
    const double lweDimCostFactor = NEGLIGIBLE_COMPLEXITY;

    llvm::APInt xSmanp = llvm::APInt{1, 1, false};
    if (xOp != nullptr) {
      const auto xSmanpAttr = xOp->getAttrOfType<mlir::IntegerAttr>("SMANP");
      assert(xSmanpAttr && "Missing SMANP value on a crypto operation");
      xSmanp = xSmanpAttr.getValue();
    }

    llvm::APInt ySmanp = llvm::APInt{1, 1, false};
    if (yOp != nullptr) {
      const auto ySmanpAttr = yOp->getAttrOfType<mlir::IntegerAttr>("SMANP");
      assert(ySmanpAttr && "Missing SMANP value on a crypto operation");
      ySmanp = ySmanpAttr.getValue();
    }

    auto loc = loc_to_string(mulOp.getLoc());
    auto comment = std::string(mulOp->getName().getStringRef()) + " " + loc;

    // (x + y) and (x - y)
    const double addSubManp =
        sqrt(xSmanp.roundToDouble() + ySmanp.roundToDouble());

    // tlu(v)
    const double tluManp = 1;

    // tlu(v1) - tlu(v2)
    const double tluSubManp = sqrt(tluManp + tluManp);

    // for tlus
    const std::vector<std::uint64_t> unknownFunction;

    // tlu(x + y)
    auto addNode =
        dag->add_levelled_op(slice(inputs), lweDimCostFactor, fixedCost,
                             addSubManp, slice(resultShape), comment);
    auto lhsTluNode = dag->add_lut(addNode, slice(unknownFunction), precision);

    // tlu(x - y)
    auto subNode =
        dag->add_levelled_op(slice(inputs), lweDimCostFactor, fixedCost,
                             addSubManp, slice(resultShape), comment);
    auto rhsTluNode = dag->add_lut(subNode, slice(unknownFunction), precision);

    // tlu(x + y) - tlu(x - y)
    const std::vector<concrete_optimizer::dag::OperatorIndex> subInputs = {
        lhsTluNode, rhsTluNode};
    index[result] =
        dag->add_levelled_op(slice(subInputs), lweDimCostFactor, fixedCost,
                             tluSubManp, slice(resultShape), comment);
  }

  void addMax(optimizer::Dag &dag, FHE::MaxEintOp &maxOp, Inputs &inputs,
              int precision) {
    mlir::Value result = maxOp.getResult();
    const std::vector<uint64_t> resultShape = getShape(result);

    Operation *xOp = maxOp.getX().getDefiningOp();
    Operation *yOp = maxOp.getY().getDefiningOp();

    const double fixedCost = NEGLIGIBLE_COMPLEXITY;
    const double lweDimCostFactor = NEGLIGIBLE_COMPLEXITY;

    llvm::APInt xSmanp = llvm::APInt{1, 1, false};
    if (xOp != nullptr) {
      const auto xSmanpAttr = xOp->getAttrOfType<mlir::IntegerAttr>("SMANP");
      assert(xSmanpAttr && "Missing SMANP value on a crypto operation");
      xSmanp = xSmanpAttr.getValue();
    }

    llvm::APInt ySmanp = llvm::APInt{1, 1, false};
    if (yOp != nullptr) {
      const auto ySmanpAttr = yOp->getAttrOfType<mlir::IntegerAttr>("SMANP");
      assert(ySmanpAttr && "Missing SMANP value on a crypto operation");
      ySmanp = ySmanpAttr.getValue();
    }

    const double subManp =
        sqrt(xSmanp.roundToDouble() + ySmanp.roundToDouble());

    auto loc = loc_to_string(maxOp.getLoc());
    auto comment = std::string(maxOp->getName().getStringRef()) + " " + loc;

    auto subNode =
        dag->add_levelled_op(slice(inputs), lweDimCostFactor, fixedCost,
                             subManp, slice(resultShape), comment);

    const double tluNodeManp = 1;
    const std::vector<std::uint64_t> unknownFunction;
    auto tluNode = dag->add_lut(subNode, slice(unknownFunction), precision);

    const double addManp = sqrt(tluNodeManp + ySmanp.roundToDouble());
    const std::vector<concrete_optimizer::dag::OperatorIndex> addInputs = {
        tluNode, inputs[1]};
    index[result] =
        dag->add_levelled_op(slice(addInputs), lweDimCostFactor, fixedCost,
                             addManp, slice(resultShape), comment);
  }

  void addMaxpool2d(optimizer::Dag &dag, FHELinalg::Maxpool2dOp &maxpool2dOp,
                    Inputs &inputs, int precision) {
    mlir::Value result = maxpool2dOp.getResult();
    const std::vector<uint64_t> resultShape = getShape(result);

    // all TLUs are flattened into a dimension
    // to create a single TLU node in optimizer dag
    std::vector<uint64_t> fakeShape = resultShape;

    uint64_t numberOfComparisons = 1;
    for (auto dimensionSize :
         maxpool2dOp.getKernelShape().getValues<int64_t>()) {
      numberOfComparisons *= dimensionSize;
    }
    fakeShape.push_back(numberOfComparisons);

    Operation *inputOp = maxpool2dOp.getInput().getDefiningOp();

    const double fixedCost = NEGLIGIBLE_COMPLEXITY;
    const double lweDimCostFactor = NEGLIGIBLE_COMPLEXITY;

    llvm::APInt inputSmanp = llvm::APInt{1, 1, false};
    if (inputOp != nullptr) {
      const auto inputSmanpAttr =
          inputOp->getAttrOfType<mlir::IntegerAttr>("SMANP");
      assert(inputSmanpAttr && "Missing SMANP value on a crypto operation");
      inputSmanp = inputSmanpAttr.getValue();
    }

    const double subManp = sqrt(2 * inputSmanp.roundToDouble() + 1);

    auto loc = loc_to_string(maxpool2dOp.getLoc());
    auto comment =
        std::string(maxpool2dOp->getName().getStringRef()) + " " + loc;

    auto subNode =
        dag->add_levelled_op(slice(inputs), lweDimCostFactor, fixedCost,
                             subManp, slice(fakeShape), comment);

    const std::vector<std::uint64_t> unknownFunction;
    auto tluNode = dag->add_lut(subNode, slice(unknownFunction), precision);

    const double addManp = sqrt(inputSmanp.roundToDouble() + 1);
    const std::vector<concrete_optimizer::dag::OperatorIndex> addInputs = {
        tluNode, inputs[0]};

    index[result] =
        dag->add_levelled_op(slice(addInputs), lweDimCostFactor, fixedCost,
                             addManp, slice(resultShape), comment);
  }

  Inputs encryptedInputs(mlir::Operation &op) {
    Inputs inputs;
    for (auto operand : op.getOperands()) {
      auto entry = index.find(operand);
      if (entry == index.end()) {
        assert(!fhe::utils::isEncryptedValue(operand));
        DEBUG("Ignoring as input " << operand);
        continue;
      }
      inputs.push_back(entry->getSecond());
    }
    return inputs;
  }

  bool isLut(mlir::Operation &op) {
    return llvm::isa<
        mlir::concretelang::FHE::ApplyLookupTableEintOp,
        mlir::concretelang::FHELinalg::ApplyLookupTableEintOp,
        mlir::concretelang::FHELinalg::ApplyMultiLookupTableEintOp,
        mlir::concretelang::FHELinalg::ApplyMappedLookupTableEintOp>(op);
  }

  bool isRound(mlir::Operation &op) {
    return llvm::isa<mlir::concretelang::FHE::RoundEintOp>(op);
  }

  mlir::concretelang::FHELinalg::Dot asDot(mlir::Operation &op) {
    return llvm::dyn_cast<mlir::concretelang::FHELinalg::Dot>(op);
  }

  mlir::concretelang::FHE::MulEintOp asMul(mlir::Operation &op) {
    return llvm::dyn_cast<mlir::concretelang::FHE::MulEintOp>(op);
  }

  mlir::concretelang::FHELinalg::MulEintOp asMulTensor(mlir::Operation &op) {
    return llvm::dyn_cast<mlir::concretelang::FHELinalg::MulEintOp>(op);
  }

  mlir::concretelang::FHE::MaxEintOp asMax(mlir::Operation &op) {
    return llvm::dyn_cast<mlir::concretelang::FHE::MaxEintOp>(op);
  }

  mlir::concretelang::FHELinalg::Maxpool2dOp asMaxpool2d(mlir::Operation &op) {
    return llvm::dyn_cast<mlir::concretelang::FHELinalg::Maxpool2dOp>(op);
  }

  bool isReturn(mlir::Operation &op) {
    return llvm::isa<mlir::func::ReturnOp>(op);
  }

  bool isConst(mlir::Operation &op) {
    return llvm::isa<mlir::arith::ConstantOp>(op);
  }

  bool isArg(const mlir::Value &value) {
    return value.isa<mlir::BlockArgument>();
  }

  std::optional<std::vector<std::int64_t>>
  resolveConstantVectorWeights(mlir::arith::ConstantOp &cstOp) {
    std::vector<std::int64_t> values;
    mlir::DenseIntElementsAttr denseVals =
        cstOp->getAttrOfType<mlir::DenseIntElementsAttr>("value");

    for (llvm::APInt val : denseVals.getValues<llvm::APInt>()) {
      if (val.getActiveBits() > 64) {
        return std::nullopt;
      }
      values.push_back(val.getSExtValue());
    }
    return values;
  }

  std::optional<std::vector<std::int64_t>>
  resolveConstantWeights(mlir::Value &value) {
    if (auto cstOp = llvm::dyn_cast_or_null<mlir::arith::ConstantOp>(
            value.getDefiningOp())) {
      auto shape = getShape(value);
      switch (shape.size()) {
      case 1:
        return resolveConstantVectorWeights(cstOp);
      default:
        DEBUG("High-Rank tensor: rely on MANP and levelledOp");
        return std::nullopt;
      }
    } else {
      DEBUG("Dynamic Weights: rely on MANP and levelledOp");
      return std::nullopt;
    }
  }

  std::optional<std::vector<std::int64_t>>
  dotWeights(mlir::concretelang::FHELinalg::Dot &dot) {
    if (dot.getOperands().size() != 2) {
      return std::nullopt;
    }
    auto weights = dot.getOperands()[1];
    return resolveConstantWeights(weights);
  }

  std::vector<std::uint64_t> getShape(mlir::Value &value) {
    return getShape(value.getType());
  }

  std::vector<std::uint64_t> getShape(mlir::Type type_) {
    if (auto ranked_tensor = type_.dyn_cast_or_null<mlir::RankedTensorType>()) {
      std::vector<std::uint64_t> shape;
      for (auto v : ranked_tensor.getShape()) {
        shape.push_back(v);
      }
      return shape;
    } else {
      return {};
    }
  }
};

} // namespace

struct DagPass : ConcreteOptimizerBase<DagPass> {
  optimizer::Config config;
  optimizer::FunctionsDag &dags;

  void runOnOperation() override {
    mlir::func::FuncOp func = getOperation();
    auto name = std::string(func.getName());
    DEBUG("ConcreteOptimizer Dag: " << name);
    auto dag = FunctionToDag(func, config).build();
    if (dag) {
      dags.insert(
          optimizer::FunctionsDag::value_type(name, std::move(dag.value())));
    } else {
      this->signalPassFailure();
    }
  }

  DagPass() = delete;
  DagPass(optimizer::Config config, optimizer::FunctionsDag &dags)
      : config(config), dags(dags) {}
};

// Create an instance of the ConcreteOptimizerPass pass.
// A global pass result is communicated using `dags`.
// If `debug` is true, for each operation, the pass emits a
// remark containing the squared Minimal Arithmetic Noise Padding of
// the equivalent dot operation.
std::unique_ptr<mlir::Pass> createDagPass(optimizer::Config config,
                                          optimizer::FunctionsDag &dags) {
  return std::make_unique<optimizer::DagPass>(config, dags);
}

} // namespace optimizer
} // namespace concretelang
} // namespace mlir