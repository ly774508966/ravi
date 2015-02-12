#include "ravijit.h"
#include "ravillvm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define lvm_c
#define LUA_CORE

#include "lprefix.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lvm.h"

#ifdef __cplusplus
}
#endif

#include <iterator>
#include <type_traits>

namespace ravi {

static volatile int init = 0;

// All Lua types are gathered here
struct LuaLLVMTypes {

  LuaLLVMTypes(llvm::LLVMContext &context);
  void dump();

  llvm::Type *C_intptr_t;
  llvm::Type *C_size_t;
  llvm::Type *C_ptrdiff_t;

  llvm::Type *lua_NumberT;
  llvm::Type *lua_IntegerT;
  llvm::Type *lua_UnsignedT;
  llvm::Type *lua_KContextT;

  llvm::Type *lua_CFunctionT;
  llvm::Type *lua_KFunctionT;

  llvm::Type *l_memT;
  llvm::Type *lu_memT;

  llvm::Type *lu_byteT;
  llvm::Type *L_UmaxalignT;

  llvm::Type *C_intT;

  llvm::StructType *lua_StateT;
  llvm::PointerType *plua_StateT;

  llvm::StructType *GCObjectT;
  llvm::PointerType *pGCObjectT;

  llvm::StructType *ValueT;
  llvm::StructType *TValueT;

  llvm::StructType *TStringT;
  llvm::PointerType *pTStringT;

  llvm::StructType *UdataT;
  llvm::StructType *TableT;
  llvm::PointerType *pTableT;

  llvm::StructType *UpvaldescT;

  llvm::Type *ravitype_tT;
  llvm::StructType *LocVarT;

};

LuaLLVMTypes::LuaLLVMTypes(llvm::LLVMContext &context) {

  static_assert(std::is_floating_point<lua_Number>::value &&
    sizeof(lua_Number) == sizeof(double),
    "lua_Number is not a double");
  lua_NumberT = llvm::Type::getDoubleTy(context);

  static_assert(std::is_integral<lua_Integer>::value, "lua_Integer is not an integer type");
  lua_IntegerT = llvm::Type::getIntNTy(context, sizeof(lua_Integer) * 8);

  static_assert(sizeof(lua_Integer) == sizeof(lua_Unsigned), "lua_Integer and lua_Unsigned are of different size");
  lua_UnsignedT = lua_IntegerT;

  C_intptr_t = llvm::Type::getIntNTy(context, sizeof(intptr_t) * 8);
  C_size_t = llvm::Type::getIntNTy(context, sizeof(size_t) * 8);
  C_ptrdiff_t = llvm::Type::getIntNTy(context, sizeof(ptrdiff_t) * 8);
  C_intT = llvm::Type::getIntNTy(context, sizeof(int) * 8);

  static_assert(sizeof(size_t) == sizeof(lu_mem), "lu_mem size is not same as size_t");
  lu_memT = C_size_t;

  static_assert(sizeof(ptrdiff_t) == sizeof(l_mem), "l_mem size is not same as ptrdiff_t");
  l_memT = C_ptrdiff_t;

  static_assert(sizeof(L_Umaxalign) == sizeof(double), "L_Umaxalign is not same size as double");
  L_UmaxalignT = llvm::Type::getDoubleTy(context);

  lu_byteT = llvm::Type::getInt8Ty(context);

  lua_StateT = llvm::StructType::create(context, "ravi.lua_State");
  plua_StateT = llvm::PointerType::get(lua_StateT, 0);

  std::vector<llvm::Type *> elements;

  // struct GCObject {
  //   GCObject *next; 
  //   lu_byte tt; 
  //   lu_byte marked
  // };
  GCObjectT = llvm::StructType::create(context, "ravi.GCObject");
  pGCObjectT = llvm::PointerType::get(GCObjectT, 0);
  elements.push_back(pGCObjectT);
  elements.push_back(lu_byteT);
  elements.push_back(lu_byteT);
  GCObjectT->setBody(elements);
  
  static_assert(sizeof(Value) == sizeof(lua_Number), "Value type is larger than lua_Number");
  // In LLVM unions should be set to the largest member
  // So in the case of a Value this is the double type
  // union Value {
  //   GCObject *gc;    /* collectable objects */
  //   void *p;         /* light userdata */
  //   int b;           /* booleans */
  //   lua_CFunction f; /* light C functions */
  //   lua_Integer i;   /* integer numbers */
  //   lua_Number n;    /* float numbers */
  // };
  ValueT = llvm::StructType::create(context, "ravi.Value");
  elements.clear();
  elements.push_back(lua_NumberT);
  ValueT->setBody(elements);

  // struct TValue {
  //   union Value value_;
  //   int tt_;
  // };
  TValueT = llvm::StructType::create(context, "ravi.TValue");
  elements.clear();
  elements.push_back(ValueT);
  elements.push_back(C_intT);
  TValueT->setBody(elements);

  ///*
  //** Header for string value; string bytes follow the end of this structure
  //  ** (aligned according to 'UTString'; see next).
  //  * /
  //  typedef struct TString {
  //   GCObject *next; 
  //   lu_byte tt; 
  //   lu_byte marked
  //   lu_byte extra;  /* reserved words for short strings; "has hash" for longs */
  //   unsigned int hash;
  //   size_t len;  /* number of characters in string */
  //   struct TString *hnext;  /* linked list for hash table */
  // } TString;

  ///*
  //** Ensures that address after this type is always fully aligned.
  //*/
  //typedef union UTString {
  //  L_Umaxalign dummy;  /* ensures maximum alignment for strings */
  //  TString tsv;
  //} UTString;
  TStringT = llvm::StructType::create(context, "ravi.TString");
  pTStringT = llvm::PointerType::get(TStringT, 0);
  elements.clear();
  elements.push_back(pGCObjectT);
  elements.push_back(lu_byteT);
  elements.push_back(lu_byteT);
  elements.push_back(lu_byteT); /* extra */
  elements.push_back(C_intT); /* hash */
  elements.push_back(C_size_t); /* len */
  elements.push_back(pTStringT); /* hnext */
  TStringT->setBody(elements);

  // Table
  TableT = llvm::StructType::create(context, "ravi.Table");
  pTableT = llvm::PointerType::get(TableT, 0);

  ///*
  //** Header for userdata; memory area follows the end of this structure
  //** (aligned according to 'UUdata'; see next).
  //*/
  //typedef struct Udata {
  //  GCObject *next; 
  //  lu_byte tt; 
  //  lu_byte marked
  //  lu_byte ttuv_;  /* user value's tag */
  //  struct Table *metatable;
  //  size_t len;  /* number of bytes */
  //  union Value user_;  /* user value */
  //} Udata;
  UdataT = llvm::StructType::create(context, "ravi.Udata");
  elements.clear();
  elements.push_back(pGCObjectT);
  elements.push_back(lu_byteT);
  elements.push_back(lu_byteT);
  elements.push_back(lu_byteT); /* ttuv_ */
  elements.push_back(pTableT); /* metatable */
  elements.push_back(C_size_t); /* len */
  elements.push_back(ValueT); /* user_ */
  UdataT->setBody(elements);

  ///*
  //** Description of an upvalue for function prototypes
  //*/
  //typedef struct Upvaldesc {
  //  TString *name;  /* upvalue name (for debug information) */
  //  lu_byte instack;  /* whether it is in stack */
  //  lu_byte idx;  /* index of upvalue (in stack or in outer function's list) */
  //}Upvaldesc;
  UpvaldescT = llvm::StructType::create(context, "ravi.Upvaldesc");
  elements.clear();
  elements.push_back(pTStringT);
  elements.push_back(lu_byteT);
  elements.push_back(lu_byteT);
  UpvaldescT->setBody(elements);

  ///*
  //** Description of a local variable for function prototypes
  //** (used for debug information)
  //*/
  //typedef struct LocVar {
  //  TString *varname;
  //  int startpc;  /* first point where variable is active */
  //  int endpc;    /* first point where variable is dead */
  //  ravitype_t ravi_type; /* RAVI type of the variable - RAVI_TANY if unknown */
  //} LocVar;
  ravitype_tT = llvm::Type::getIntNTy(context, sizeof(ravitype_t) * 8);
  LocVarT = llvm::StructType::create(context, "ravi.LocVar");
  elements.clear();
  elements.push_back(pTStringT); /* varname */
  elements.push_back(C_intT); /* startpc */
  elements.push_back(C_intT); /* endpc */
  elements.push_back(ravitype_tT); /* ravi_type */
  LocVarT->setBody(elements);


}

void LuaLLVMTypes::dump() {
  GCObjectT->dump(); fputs("\n", stdout);
  TValueT->dump(); fputs("\n", stdout);
  TStringT->dump(); fputs("\n", stdout);
  UdataT->dump(); fputs("\n", stdout);
  UpvaldescT->dump(); fputs("\n", stdout);
  LocVarT->dump(); fputs("\n", stdout);

}

class RAVI_API RaviJITStateImpl;


// Represents a JITed or JITable function
// Each function gets its own module and execution engine - this
// may change in future
// The advantage is that we can drop the function when the corresponding
// Lua object is garbage collected - with MCJIT this is not possible
// to do at function level
class RAVI_API RaviJITFunctionImpl : public RaviJITFunction {

  // The function is tracked by RaviJITState so we need to
  // tell RaviJITState when this function dies
  RaviJITStateImpl *owner_;

  // Module within which the function will be defined
  llvm::Module *module_;

  // Unique name for the function
  std::string name_;

  // The execution engine responsible for compiling the
  // module
  llvm::ExecutionEngine *engine_;

  // The llvm Function definition
  llvm::Function *function_;

  // Pointer to compiled function - this is only set when
  // the function
  void *ptr_;

public:
  RaviJITFunctionImpl(RaviJITStateImpl *owner, llvm::Module *module,
    llvm::FunctionType *type,
    llvm::GlobalValue::LinkageTypes linkage,
    const std::string &name);
  virtual ~RaviJITFunctionImpl();

  // Compile the function if not already compiled and
  // return pointer to function
  virtual void *compile();

  // Add declaration for an extern function that is not
  // loaded dynamically - i.e., is part of the the executable
  // and therefore not visible at runtime by name
  virtual llvm::Constant *addExternFunction(llvm::FunctionType *type, void *address,
    const std::string &name);

  virtual const std::string &name() const { return name_; }
  virtual llvm::Function *function() const { return function_; }
  virtual llvm::Module *module() const { return module_; }
  virtual llvm::ExecutionEngine *engine() const { return engine_; }
  virtual RaviJITState *owner() const;
  virtual void dump();
};

// Ravi's JIT State
// All of the JIT information is held here
class RAVI_API RaviJITStateImpl : public RaviJITState {
  // map of names to functions
  std::map<std::string, RaviJITFunction *> functions_;
  llvm::LLVMContext &context_;
  // The triple represents the host target
  std::string triple_;
  LuaLLVMTypes *types_;

public:
  RaviJITStateImpl();
  virtual ~RaviJITStateImpl();

  // Create a function of specified type and linkage
  virtual RaviJITFunction *createFunction(llvm::FunctionType *type,
    llvm::GlobalValue::LinkageTypes linkage,
    const std::string &name);

  // Stop tracking the named function - note that
  // the function is assumed to be destroyed by the user
  void deleteFunction(const std::string &name);

  void addGlobalSymbol(const std::string &name, void *address);

  virtual void dump();
  virtual llvm::LLVMContext &context() { return context_; }
};

RaviJITState * RaviJITFunctionImpl::owner() const {
  return owner_;
}


RaviJITStateImpl::RaviJITStateImpl() : context_(llvm::getGlobalContext()) {
  // Unless following three lines are not executed then
  // ExecutionEngine cannot be created
  if (init == 0) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();
    init++;
  }
  triple_ = llvm::sys::getProcessTriple();
#ifdef _WIN32
  // On Windows we get error saying incompatible object format
  // Reading posts on mailining lists I found that the issue is that COEFF
  // format is not supported and therefore we need to set -elf as the object
  // format
  triple_ += "-elf";
#endif
  types_ = new LuaLLVMTypes(context_);
}

RaviJITStateImpl::~RaviJITStateImpl() {
  std::vector<RaviJITFunction *> todelete;
  for (auto &f = std::begin(functions_); f != std::end(functions_); f++) {
    todelete.push_back(f->second);
  }
  for (int i = 0; i < todelete.size(); i++) {
    delete todelete[i];
  }
  delete types_;
}

void RaviJITStateImpl::addGlobalSymbol(const std::string &name, void *address) {
  llvm::sys::DynamicLibrary::AddSymbol(name, address);
}

void RaviJITStateImpl::dump() {
  types_->dump();
  for (auto f : functions_) {
    f.second->dump();
  }
}

RaviJITFunction *
RaviJITStateImpl::createFunction(llvm::FunctionType *type,
                             llvm::GlobalValue::LinkageTypes linkage,
                             const std::string &name) {
  // MCJIT treats each module as a compilation unit
  // To enabe function level life cycle we create a
  // module per function
  std::string moduleName = "ravi_module_" + name;
  llvm::Module *module = new llvm::Module(moduleName, context_);
#if defined(_WIN32)
  // On Windows we get error saying incompatible object format
  // Reading posts on mailining lists I found that the issue is that COEFF
  // format is not supported and therefore we need to set
  // -elf as the object format
  module->setTargetTriple(triple_);
#endif
  RaviJITFunction *f = new RaviJITFunctionImpl(this, module, type, linkage, name);
  functions_[name] = f;
  return f;
}

void RaviJITStateImpl::deleteFunction(const std::string &name) {
  functions_.erase(name);
  // This is called when RaviJITFunction is deleted
}

RaviJITFunctionImpl::RaviJITFunctionImpl(RaviJITStateImpl *owner, llvm::Module *module,
                                 llvm::FunctionType *type,
                                 llvm::GlobalValue::LinkageTypes linkage,
                                 const std::string &name)
    : owner_(owner), module_(module), name_(name), engine_(nullptr),
      function_(nullptr), ptr_(nullptr) {
  function_ = llvm::Function::Create(type, linkage, name, module);
  std::string errStr;
  engine_ = llvm::EngineBuilder(module)
                .setEngineKind(llvm::EngineKind::JIT)
                .setUseMCJIT(true)
                .setErrorStr(&errStr)
                .create();
  if (!engine_) {
    fprintf(stderr, "Could not create ExecutionEngine: %s\n", errStr.c_str());
    return;
  }
}

RaviJITFunctionImpl::~RaviJITFunctionImpl() {
  // Remove this function from parent
  owner_->deleteFunction(name_);
  if (engine_)
    delete engine_;
  else if (module_)
    delete module_;
  // Check - we assume here that deleting engine deletes the module
  // and function, and deleting module deletes function
}

void *RaviJITFunctionImpl::compile() {

#if 0
  // Create a function pass manager for this engine
  llvm::FunctionPassManager *FPM = new llvm::FunctionPassManager(OpenModule);

  // Set up the optimizer pipeline.  Start with registering info about how the
  // target lays out data structures.
  OpenModule->setDataLayout(NewEngine->getDataLayout());
  FPM->add(new llvm::DataLayoutPass());
  // Provide basic AliasAnalysis support for GVN.
  FPM->add(llvm::createBasicAliasAnalysisPass());
  // Promote allocas to registers.
  FPM->add(llvm::createPromoteMemoryToRegisterPass());
  // Do simple "peephole" optimizations and bit-twiddling optzns.
  FPM->add(llvm::createInstructionCombiningPass());
  // Reassociate expressions.
  FPM->add(llvm::createReassociatePass());
  // Eliminate Common SubExpressions.
  FPM->add(llvm::createGVNPass());
  // Simplify the control flow graph (deleting unreachable blocks, etc).
  FPM->add(llvm::createCFGSimplificationPass());
  FPM->doInitialization();

  // For each function in the module
  llvm::Module::iterator it;
  llvm::Module::iterator end = OpenModule->end();
  for (it = OpenModule->begin(); it != end; ++it) {
    // Run the FPM on this function
    FPM->run(*it);
  }

  // We don't need this anymore
  delete FPM;
#endif

  if (ptr_)
    return ptr_;
  if (!function_ || !engine_)
    return NULL;

  // Upon creation, MCJIT holds a pointer to the Module object
  // that it received from EngineBuilder but it does not immediately
  // generate code for this module. Code generation is deferred
  // until either the MCJIT::finalizeObject method is called
  // explicitly or a function such as MCJIT::getPointerToFunction
  // is called which requires the code to have been generated.
  engine_->finalizeObject();
  ptr_ = engine_->getPointerToFunction(function_);
  return ptr_;
}

llvm::Constant *RaviJITFunctionImpl::addExternFunction(llvm::FunctionType *type,
                                                   void *address,
                                                   const std::string &name) {
  llvm::Function *f = llvm::Function::Create(
      type, llvm::Function::ExternalLinkage, name, module_);
  // We should have been able to call
  // engine_->addGlobalMapping() but this doesn't work
  // See http://lists.cs.uiuc.edu/pipermail/llvmdev/2014-April/071856.html
  // See bug report http://llvm.org/bugs/show_bug.cgi?id=20656
  // following will call DynamicLibrary::AddSymbol
  owner_->addGlobalSymbol(name, address);
  return f;
}

void RaviJITFunctionImpl::dump() { module_->dump(); }

std::unique_ptr<RaviJITState> RaviJITStateFactory::newJITState() {
  return std::unique_ptr<RaviJITState>(new RaviJITStateImpl());
}

}

#ifdef __cplusplus
extern "C" {
#endif

struct ravi_State {
  ravi::RaviJITState *jit;
};

int raviV_initjit(struct lua_State *L) {
  global_State *G = G(L);
  if (G->ravi_state != NULL)
    return -1;
  ravi_State *jit = (ravi_State *)calloc(1, sizeof(ravi_State));
  jit->jit = new ravi::RaviJITStateImpl();
  G->ravi_state = jit;
  return 0;
}

void raviV_close(struct lua_State *L) {
  global_State *G = G(L);
  if (G->ravi_state == NULL)
    return;
  delete G->ravi_state->jit;
  free(G->ravi_state);
}

#ifdef __cplusplus
}
#endif
