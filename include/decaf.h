
#include <llvm/IR/InstVisitor.h>

#include <z3++.h>

#include <cassert>
#include <unordered_map>
#include <vector>

#include "macros.h"

namespace decaf {
class StackFrame {
public:
  std::unordered_map<llvm::Value *, z3::expr> variables;

  llvm::Function *function;
  llvm::BasicBlock *current_block;
  llvm::BasicBlock *prev_block;
  llvm::BasicBlock::iterator current;

  StackFrame(llvm::Function *function);

  /**
   * Change the instruction pointer to point at the start of the provided
   * block and update the previous block accordingly.
   *
   * Use this when implementing a jump instruction.
   */
  void jump_to(llvm::BasicBlock *block);

  /**
   * Insert a new value into the current stack frame. If that value
   * is already in the current stack frame then it overwrites it.
   */
  void insert(llvm::Value *value, const z3::expr &expr);
  /**
   * Lookup a value within the current stack frame.
   *
   * There are two main cases here:
   * 1. `value` is an existing variable
   * 2. `value` is a constant
   *
   * In the first case we just look up the variable the `variables` map
   * and then return it. In the second case we build a Z3 expression
   * that represents the constant and return that.
   *
   * This method should be preferred over directly interacting with
   * `variables` as it correctly handles constants.
   */
  z3::expr lookup(llvm::Value *value, z3::context &ctx) const;
}; // namespace decaf

class Context {
public:
  std::vector<StackFrame> stack;
  z3::solver solver;

private:
  Context(const Context &ctx, z3::solver &&solver);

public:
  Context(z3::context &z3, llvm::Function *function);

  /**
   * Create a new context that is independant from this
   * one but has the same state.
   */
  Context fork() const;

  /**
   * Get the top frame of the stack.
   *
   * This should be used instead of directly manipulating the stack
   * vector so that it continues to work when more advanced data
   * structures are implemented.
   */
  StackFrame &stack_top();

  /**
   * Check whether the current set of assertions + the given expression
   * is satisfiable.
   *
   * Does not modify the solver state. If this returns sat then you can
   * get the solver model as a test case.
   *
   * This will cause an assertion failure if expr is not either a boolean
   * or a 1-bit integer. 1-bit integers will be implicitly converted to
   * a boolean.
   */
  z3::check_result check(const z3::expr &expr);
  /**
   * Check whether the current set of assertions is satisifiable.
   *
   * If this returns sat then you can extract a model by calling
   * `solver.model()`.
   */
  z3::check_result check();

  /**
   * Add a new assertion to the solver.
   */
  void add(const z3::expr &assertion);
};

class FailureTracker {
protected:
  FailureTracker() = default;
  virtual ~FailureTracker() = default;

public:
  // The current execution has encountered a failure.
  virtual void add_failure(const Context &ctx, const z3::model &model) = 0;
};

// Default FailureTracker if none is provided
class PrintingFailureTracker : public FailureTracker {
public:
  PrintingFailureTracker() = default;

  void add_failure(const Context &ctx, const z3::model &model) override;

  static FailureTracker *default_instance();
};

class Executor {
private:
  std::vector<Context> contexts;

public:
  // The current context has forked and the fork needs to be added
  // to the queue.
  void add_context(Context &&ctx);

  // Get the next context to be executed.
  Context next_context();

  // Are there any contexts left?
  bool has_next() const;
};

enum class ExecutionResult { Continue, Stop };

class Interpreter : public llvm::InstVisitor<Interpreter, ExecutionResult> {
private:
  Context *ctx;
  Executor *queue;
  z3::context *z3;
  FailureTracker *tracker;

public:
  // Add some more parameters here
  Interpreter(Context *ctx, Executor *queue, z3::context *z3,
              FailureTracker *tracker = PrintingFailureTracker::default_instance());

  /**
   * Execute this interpreter's context until it finishes.
   *
   * Contexts from forks will be placed into the execution queue.
   * Failures resulting from this context will also be added to
   * the execution queue.
   */
  void execute();

  // Marks an unimplemented instruction.
  //
  // TODO: Better error message?
  ExecutionResult visitInstruction(llvm::Instruction &);

  // Replace this with implementation in cpp file as we go
  ExecutionResult visitAdd(llvm::BinaryOperator &op);
  ExecutionResult visitSub(llvm::BinaryOperator &op);
  ExecutionResult visitMul(llvm::BinaryOperator &op);
  ExecutionResult visitUDiv(llvm::BinaryOperator &op);
  ExecutionResult visitSDiv(llvm::BinaryOperator &op);
  ExecutionResult visitURem(llvm::BinaryOperator &op) {
    DECAF_UNIMPLEMENTED();
  }
  ExecutionResult visitSRem(llvm::BinaryOperator &op) {
    DECAF_UNIMPLEMENTED();
  }

  ExecutionResult visitICmpInst(llvm::ICmpInst &icmp);

  ExecutionResult visitPHINode(llvm::PHINode &node);
  ExecutionResult visitBranchInst(llvm::BranchInst &inst);
  ExecutionResult visitReturnInst(llvm::ReturnInst &inst);
  ExecutionResult visitCallInst(llvm::CallInst &inst);
  ExecutionResult visitSelectInst(llvm::SelectInst &inst);

private:
  ExecutionResult visitExternFunc(llvm::CallInst &inst);

  /**
   * Implements decaf_assume. Docs follow:
   *
   * Assume that a condition is true.
   *
   * This will silently remove any executions in which the condition
   * could evaluate to false.
   */
  ExecutionResult visitAssume(llvm::CallInst &inst);

  /**
   * Implements decaf_assert. Docs follow:
   *
   * Assert that the condition is true.
   *
   * In cases where the symbolic executor determines that the
   * condition could be false, it will produce a test case with
   * concrete inputs which reproduce the failure.
   */
  ExecutionResult visitAssert(llvm::CallInst &inst);
};

/**
 * Get the Z3 sort corresponding to the provided LLVM type.
 *
 * Only works for supported scalar values at the moment
 * (i.e. only integers).
 *
 * Invalid types will result in an assertion.
 */
z3::sort sort_for_type(z3::context &ctx, llvm::Type *type);

/**
 * Executes the given function symbolically.
 *
 * Currently this works by making all the function arguments symbolic.
 * Assertion failures during symbolic execution will result in the
 * model being printed to stdout.
 *
 * Note: This is probably good enough for the prototype but we should
 *       definitely refine the interface as we figure out the best API
 *       for this.
 */
void execute_symbolic(llvm::Function *function,
                      FailureTracker *tracker = PrintingFailureTracker::default_instance());

/**
 * Create a Z3 expression with the same value as the given constant.
 *
 * Currently only supports integers and will abort on any other LLVM
 * type.
 */
z3::expr evaluate_constant(z3::context &ctx, llvm::Constant *constant);

/**
 * Normalize a Z3 expression to represent 1-bit integers as booleans.
 * Doesn't affect any other expression type.
 *
 * Justification
 * =============
 * LLVM represents booleans using 1-bit integers and most of the time
 * they're being used as booleans so we need some conversion methods
 * for when we have one and need the other.
 */
z3::expr normalize_to_bool(const z3::expr &expr);
/**
 * Normalize a Z3 expression to represent booleans as integers.
 * Doesn't affect any other expression type.
 *
 * Justification
 * =============
 * LLVM represents booleans using 1-bit integers and most of the time
 * they're being used as booleans so we need some conversion methods
 * for when we have one and need the other.
 */
z3::expr normalize_to_int(const z3::expr &expr);
} // namespace decaf
