// Part of the Concrete Compiler Project, under the BSD3 License with Zama
// Exceptions. See
// https://github.com/zama-ai/concrete-compiler-internal/blob/main/LICENSE.txt
// for license information.

#ifndef CONCRETELANG_SUPPORT_COMPILER_ENGINE_H
#define CONCRETELANG_SUPPORT_COMPILER_ENGINE_H

#include <concretelang/Conversion/Utils/GlobalFHEContext.h>
#include <concretelang/Support/V0ClientParameters.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/SourceMgr.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/MLIRContext.h>
#include <mlir/Pass/Pass.h>

namespace mlir {
namespace concretelang {

bool getEmitGPUOption();

/// Compilation context that acts as the root owner of LLVM and MLIR
/// data structures directly and indirectly referenced by artefacts
/// produced by the `CompilerEngine`.
class CompilationContext {
public:
  CompilationContext();
  ~CompilationContext();

  mlir::MLIRContext *getMLIRContext();
  llvm::LLVMContext *getLLVMContext();

  static std::shared_ptr<CompilationContext> createShared();

protected:
  mlir::MLIRContext *mlirContext;
  llvm::LLVMContext *llvmContext;
};

enum Backend {
  CPU,
  GPU,
};

/// Compilation options allows to configure the compilation pipeline.
struct CompilationOptions {
  llvm::Optional<mlir::concretelang::V0FHEConstraint> v0FHEConstraints;

  llvm::Optional<mlir::concretelang::V0Parameter> v0Parameter;

  /// largeIntegerParameter force the compiler engine to lower FHE.eint using
  /// the large integers strategy with the given parameters.
  llvm::Optional<mlir::concretelang::LargeIntegerParameter>
      largeIntegerParameter;

  bool verifyDiagnostics;

  bool autoParallelize;
  bool loopParallelize;
  bool batchConcreteOps;
  bool emitSDFGOps;
  bool unrollLoopsWithSDFGConvertibleOps;
  bool dataflowParallelize;
  bool optimizeConcrete;
  /// use GPU during execution by generating GPU operations if possible
  bool emitGPUOps;
  llvm::Optional<std::vector<int64_t>> fhelinalgTileSizes;

  llvm::Optional<std::string> clientParametersFuncName;

  optimizer::Config optimizerConfig;

  /// When decomposing big integers into chunks, chunkSize is the total number
  /// of bits used for the message, including the carry, while chunkWidth is
  /// only the number of bits used during encoding and decoding of a big integer
  unsigned int chunkSize;
  unsigned int chunkWidth;

  CompilationOptions()
      : v0FHEConstraints(llvm::None), verifyDiagnostics(false),
        autoParallelize(false), loopParallelize(false), batchConcreteOps(false),
        emitSDFGOps(false), unrollLoopsWithSDFGConvertibleOps(false),
        dataflowParallelize(false), optimizeConcrete(true), emitGPUOps(false),
        clientParametersFuncName(llvm::None),
        optimizerConfig(optimizer::DEFAULT_CONFIG), chunkSize(4),
        chunkWidth(2){};

  CompilationOptions(std::string funcname) : CompilationOptions() {
    clientParametersFuncName = funcname;
  }

  /// @brief Constructor for CompilationOptions with default parameters for a
  /// specific backend.
  /// @param funcname The name of the function to compile.
  /// @param backend The backend to target.
  CompilationOptions(std::string funcname, enum Backend backend)
      : CompilationOptions(funcname) {
    switch (backend) {
    case Backend::CPU:
      loopParallelize = true;
      break;
    case Backend::GPU:
      batchConcreteOps = true;
      emitGPUOps = true;
      break;
    }
  }
};

class CompilerEngine {
public:
  /// Result of an invocation of the `CompilerEngine` with optional
  /// fields for the results produced by different stages.
  class CompilationResult {
  public:
    CompilationResult(std::shared_ptr<CompilationContext> compilationContext =
                          CompilationContext::createShared())
        : compilationContext(compilationContext) {}

    llvm::Optional<mlir::OwningOpRef<mlir::ModuleOp>> mlirModuleRef;
    llvm::Optional<mlir::concretelang::ClientParameters> clientParameters;
    llvm::Optional<CompilationFeedback> feedback;
    std::unique_ptr<llvm::Module> llvmModule;
    llvm::Optional<mlir::concretelang::V0FHEContext> fheContext;

  protected:
    std::shared_ptr<CompilationContext> compilationContext;
  };

  class Library {
    std::string outputDirPath;
    std::vector<std::string> objectsPath;
    std::vector<mlir::concretelang::ClientParameters> clientParametersList;
    std::vector<mlir::concretelang::CompilationFeedback>
        compilationFeedbackList;
    /// Path to the runtime library. Will be linked to the output library if set
    std::string runtimeLibraryPath;
    bool cleanUp;

  public:
    /// Create a library instance on which you can add compilation results.
    /// Then you can emit a library file with the given path.
    /// cleanUp at false keeps intermediate .obj files for later use.
    Library(std::string outputDirPath, std::string runtimeLibraryPath = "",
            bool cleanUp = true)
        : outputDirPath(outputDirPath), runtimeLibraryPath(runtimeLibraryPath),
          cleanUp(cleanUp) {}
    /// Add a compilation result to the library
    llvm::Expected<std::string> addCompilation(CompilationResult &compilation);
    /// Emit the library artifacts with the previously added compilation result
    llvm::Error emitArtifacts(bool sharedLib, bool staticLib,
                              bool clientParameters, bool compilationFeedback,
                              bool cppHeader);
    /// After a shared library has been emitted, its path is here
    std::string sharedLibraryPath;
    /// After a static library has been emitted, its path is here
    std::string staticLibraryPath;

    /// Returns the path of the shared library
    static std::string getSharedLibraryPath(std::string outputDirPath);

    /// Returns the path of the static library
    static std::string getStaticLibraryPath(std::string outputDirPath);

    /// Returns the path of the client parameters
    static std::string getClientParametersPath(std::string outputDirPath);

    /// Returns the path of the compilation feedback
    static std::string getCompilationFeedbackPath(std::string outputDirPath);

    // For advanced use
    const static std::string OBJECT_EXT, LINKER, LINKER_SHARED_OPT, AR,
        AR_STATIC_OPT, DOT_STATIC_LIB_EXT, DOT_SHARED_LIB_EXT;
    void addExtraObjectFilePath(std::string objectFilePath);
    llvm::Expected<std::string>
    emit(std::string path, std::string dotExt, std::string linker,
         llvm::Optional<std::vector<std::string>> extraArgs = {});
    ~Library();

  private:
    /// Emit a shared library with the previously added compilation result
    llvm::Expected<std::string> emitStatic();
    /// Emit a shared library with the previously added compilation result
    llvm::Expected<std::string> emitShared();
    /// Emit a json ClientParameters corresponding to library content
    llvm::Expected<std::string> emitClientParametersJSON();
    /// Emit a json CompilationFeedback corresponding to library content
    llvm::Expected<std::string> emitCompilationFeedbackJSON();
    /// Emit a client header file for this corresponding to library content
    llvm::Expected<std::string> emitCppHeader();
  };

  /// Specification of the exit stage of the compilation pipeline
  enum class Target {
    /// Only read sources and produce corresponding MLIR module
    ROUND_TRIP,

    /// Read sources and exit before any lowering
    FHE,

    /// Read sources and lower all the FHELinalg operations to FHE operations
    /// and scf loops
    FHE_NO_LINALG,

    /// Read sources and lower all FHE operations to TFHE
    /// operations
    TFHE,

    /// Read sources and lower all FHE and TFHE operations to Concrete
    /// operations
    CONCRETE,

    /// Read sources and lower all FHE, TFHE and Concrete operations to
    /// BConcrete operations
    BCONCRETE,

    /// Read sources and lower all FHE, TFHE and Concrete operations to
    /// BConcrete, then extract SDFG operations
    SDFG,

    /// Read sources and lower all FHE, TFHE and Concrete
    /// operations to canonical MLIR dialects. Cryptographic operations
    /// are lowered to invocations of the concrete library.
    STD,

    /// Read sources and lower all FHE, TFHE and Concrete
    /// operations to operations from the LLVM dialect. Cryptographic
    /// operations are lowered to invocations of the concrete library.
    LLVM,

    /// Same as `LLVM`, but lowers to actual LLVM IR instead of the
    /// LLVM dialect
    LLVM_IR,

    /// Same as `LLVM_IR`, but invokes the LLVM optimization pipeline
    /// to produce optimized LLVM IR
    OPTIMIZED_LLVM_IR,

    /// Same as `OPTIMIZED_LLVM_IR`, but compiles and add an object file to a
    /// futur library
    LIBRARY
  };

  CompilerEngine(std::shared_ptr<CompilationContext> compilationContext)
      : overrideMaxEintPrecision(), overrideMaxMANP(), compilerOptions(),
        generateClientParameters(
            compilerOptions.clientParametersFuncName.hasValue()),
        enablePass([](mlir::Pass *pass) { return true; }),
        compilationContext(compilationContext) {}

  llvm::Expected<CompilationResult>
  compile(llvm::StringRef s, Target target,
          llvm::Optional<std::shared_ptr<Library>> lib = {});

  llvm::Expected<CompilationResult>
  compile(std::unique_ptr<llvm::MemoryBuffer> buffer, Target target,
          llvm::Optional<std::shared_ptr<Library>> lib = {});

  llvm::Expected<CompilationResult>
  compile(llvm::SourceMgr &sm, Target target,
          llvm::Optional<std::shared_ptr<Library>> lib = {});

  llvm::Expected<CompilerEngine::Library>
  compile(std::vector<std::string> inputs, std::string outputDirPath,
          std::string runtimeLibraryPath = "", bool generateSharedLib = true,
          bool generateStaticLib = true, bool generateClientParameters = true,
          bool generateCompilationFeedback = true,
          bool generateCppHeader = true);

  /// Compile and emit artifact to the given outputDirPath from an LLVM source
  /// manager.
  llvm::Expected<CompilerEngine::Library>
  compile(llvm::SourceMgr &sm, std::string outputDirPath,
          std::string runtimeLibraryPath = "", bool generateSharedLib = true,
          bool generateStaticLib = true, bool generateClientParameters = true,
          bool generateCompilationFeedback = true,
          bool generateCppHeader = true);

  void setCompilationOptions(CompilationOptions &options) {
    compilerOptions = options;
    if (options.v0FHEConstraints.hasValue()) {
      setFHEConstraints(*options.v0FHEConstraints);
    }

    if (options.clientParametersFuncName.hasValue()) {
      setGenerateClientParameters(true);
    }
  }

  void setFHEConstraints(const mlir::concretelang::V0FHEConstraint &c);
  void setMaxEintPrecision(size_t v);
  void setMaxMANP(size_t v);
  void setGenerateClientParameters(bool v);
  void setEnablePass(std::function<bool(mlir::Pass *)> enablePass);

protected:
  llvm::Optional<size_t> overrideMaxEintPrecision;
  llvm::Optional<size_t> overrideMaxMANP;
  CompilationOptions compilerOptions;
  bool generateClientParameters;
  std::function<bool(mlir::Pass *)> enablePass;

  std::shared_ptr<CompilationContext> compilationContext;

private:
  llvm::Expected<llvm::Optional<optimizer::Description>>
  getConcreteOptimizerDescription(CompilationResult &res);
  llvm::Error determineFHEParameters(CompilationResult &res);
};

} // namespace concretelang
} // namespace mlir

#endif
