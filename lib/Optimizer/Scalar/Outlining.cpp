/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
#define DEBUG_TYPE "outline"

#include "hermes/Optimizer/Scalar/Outlining.h"
#include "hermes/IR/IRBuilder.h"
#include "hermes/IR/Instrs.h"
#include "hermes/Optimizer/Scalar/InstructionEscapeAnalysis.h"
#include "hermes/Optimizer/Scalar/InstructionNumbering.h"
#include "hermes/Support/Statistic.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Debug.h"
#include "llvm_extra/Outliner.h"

#include <algorithm>
#include <utility>

using llvm::dbgs;
using llvm::dyn_cast;
using llvm::isa;
using llvm::outliner::Candidate;
using llvm::outliner::OutlinedFunction;
using llvm::outliner::OutlinerTarget;

STATISTIC(NumCandidatesOutlined, "Number of candidates outlined");
STATISTIC(NumFunctionsCreated, "Number of outlined functions created");
STATISTIC(NumInstructionsSaved, "Number of instructions saved by outlining");
STATISTIC(NumOutliningRounds, "Number of outlining rounds performed");

namespace hermes {

/// Flags for InstructionNumbering.
static constexpr InstructionNumbering::ExternalFlags NUMBERING_FLAGS =
    InstructionNumbering::Instructions | InstructionNumbering::Parameters;

// Name used for outlined functions. We rely on Module::deriveUniqueInternalName
// to get unique names for each function ("OUTLINED_FUNCTION 1#" and so on).
static constexpr const char *FUNCTION_NAME = "OUTLINED_FUNCTION";

/// \return A name for an outlined function's nth parameter in the module \p M.
static Identifier getParameterName(Module *M, unsigned n) {
  assert(n <= 999 && "Too many parameters!");
  llvm::SmallVector<char, 8> storage;
  // Note that getIdentifier copies the string, so no reference storage escapes.
  return M->getContext().getIdentifier(
      (Twine{'p'} + Twine{n}).toStringRef(storage));
}

namespace {

/// Hermes outliner target implementation. This provides the candidate selection
/// algorithm and cost model specific to Hermes IR.
class HermesOutlinerTarget : public OutlinerTarget {
  /// The outlining settings.
  const OutliningSettings settings_;

  /// List of instructions corresponding to elements in the suffix tree input.
  const llvm::ArrayRef<Instruction *> instructions_;

  /// Look up the basic block range for a substring of instructions_.
  ///
  /// \param startIdx Start index of the instructions_ substring.
  /// \param len Length of the instructions_ substring. Must be positive.
  /// \return The corresponding basic block range.
  BasicBlock::range getRange(unsigned startIdx, unsigned len) {
    assert(len > 0 && "Empty range!");
    auto begin = instructions_[startIdx]->getIterator();
    auto end = std::next(instructions_[startIdx + len - 1]->getIterator());
    return {begin, end};
  }

  /// Get the longest common prefix from two ranges that can be outlined.
  ///
  /// \param[out] expressions Expressions generated by InstructionNumbering for
  ///   the common prefix of the two ranges. Must be empty.
  /// \param[out] escapeAnalysis The EscapeAnalysis used to ensure that only one
  ///     value escapes from the common prefix.
  /// \param startIdx0 Start index of the first range.
  /// \param startIdx1 Start index of the second range.
  /// \param length Length of both ranges. Must be positive.
  void getOutlinableCommonPrefix(
      std::vector<InstructionNumbering::Expression> &expressions,
      InstructionEscapeAnalysis &escapeAnalysis,
      unsigned startIdx0,
      unsigned startIdx1,
      unsigned length) {
    assert(expressions.empty() && "Output vector not empty!");

    // Iterate over InstructionNumbering for both ranges until they don't match.
    InstructionNumbering numbering0(
        getRange(startIdx0, length), NUMBERING_FLAGS);
    InstructionNumbering numbering1(
        getRange(startIdx1, length), NUMBERING_FLAGS);
    for (auto iter0 = numbering0.begin(),
              end0 = numbering0.end(),
              iter1 = numbering1.begin(),
              end1 = numbering1.end();
         iter0 != end0 && iter1 != end1;
         ++iter0, ++iter1) {
      if (*iter0 != *iter1) {
        break;
      }
      expressions.push_back(*iter0);
    }

    // Shorten the common prefix so that at most one value escapes. But first
    // check the length, since addRange requires a nonempty range.
    const auto commonLength = expressions.size();
    if (commonLength > 0) {
      escapeAnalysis.addRange(getRange(startIdx0, commonLength));
      escapeAnalysis.addRange(getRange(startIdx1, commonLength));
      expressions.erase(
          expressions.begin() + escapeAnalysis.longestPrefix().length,
          expressions.end());
    }
  }

  /// Check if a range matches the common prefix from getOutlinableCommonPrefix.
  ///
  /// The new range starts at \p startIdx and has length \p expressions.size().
  /// It matches the existing common prefix if it has the same expressions and
  /// the same escaping instruction offset (or none).
  ///
  /// \param expressions Expressions from getOutlinableCommonPrefix.
  /// \param escapeAnalysis EscapeAnalysis from getOutlinableCommonPrefix.
  /// \param startIdx Start index of the new range.
  /// \return True if the new range matches the existing common prefix, and can
  ///   be included in the same OutlinedFunction.
  bool matchesCommonPrefix(
      llvm::ArrayRef<InstructionNumbering::Expression> expressions,
      InstructionEscapeAnalysis &escapeAnalysis,
      unsigned startIdx) {
    const auto length = expressions.size();
    const auto range = getRange(startIdx, length);
    InstructionNumbering numbering(range, NUMBERING_FLAGS);
    if (std::equal(expressions.begin(), expressions.end(), numbering.begin())) {
      escapeAnalysis.addRange(range);
      if (escapeAnalysis.longestPrefix().length == length) {
        return true;
      }
      escapeAnalysis.removeLastRange();
    }
    return false;
  }

  /// \return The number of distinct External operands in \p expressions.
  static unsigned distinctExternalOperandCount(
      llvm::ArrayRef<InstructionNumbering::Expression> expressions) {
    // External operands are indexed sequentially starting from 0, so if the
    // highest index is N, then the number of distinct Externals is N + 1. If
    // there are no Externals at all, then it's 0.
    unsigned count = 0;
    for (const auto &expr : expressions) {
      for (const auto &operand : expr.operands) {
        if (operand.kind == InstructionNumbering::OperandKind::External) {
          count = std::max(count, operand.index + 1);
        }
      }
    }
    return count;
  }

 public:
  /// Create a HermesOutlinerTarget.
  ///
  /// \param settings The outlining settings.
  /// \param instructions The list of instructions corresponding to the suffix
  ///   tree input string.
  HermesOutlinerTarget(
      const OutliningSettings &settings,
      llvm::ArrayRef<Instruction *> instructions)
      : settings_(settings), instructions_(instructions) {}

  unsigned minCandidateLength() override {
    return settings_.minLength;
  }

  // Group potential outlining candidates into zero or more outlined functions.
  //
  // The potential candidates are sequences of instructions of equal length that
  // match according to InstructionValueInfo, but are not necessarily able to be
  // outlined. To produce an OutlinedFunction, each location must have code that
  // is structurally the same (verify using InstructionNumbering) with at most
  // one output (verify using InstructionEscapeAnalysis).
  //
  // This greedy algorithm works by taking the first two potential candidates
  // and finding the longest prefix of each that matches. If the prefix is long
  // enough, it creates a new OutlinedFunction. It continues doing this with
  // what remains of the two potential candidates until they are both consumed.
  // Each time it creates an OutlinedFunction, it attempts to include the same
  // section from all the other potential candidates besides the first two.
  void createOutlinedFunctions(
      std::vector<OutlinedFunction> &functions,
      llvm::ArrayRef<unsigned> startIndices,
      unsigned candidateLength) override {
    assert(startIndices.size() >= 2 && "Too few candidates!");
    assert(candidateLength >= settings_.minLength && "Candidates too small!");
    const unsigned maxOffset = candidateLength - settings_.minLength;

    std::vector<InstructionNumbering::Expression> expressions;
    for (unsigned offset = 0; offset <= maxOffset;
         // Advance an extra +1 to skip over the instruction that didn't match.
         offset += expressions.size() + 1) {
      // Get the longest common prefix starting from index0 and index1.
      expressions.clear();
      InstructionEscapeAnalysis escapeAnalysis;
      const auto index0 = startIndices[0] + offset;
      const auto index1 = startIndices[1] + offset;
      const auto remainingLength = candidateLength - offset;
      getOutlinableCommonPrefix(
          expressions, escapeAnalysis, index0, index1, remainingLength);
      const auto commonLength = expressions.size();
      if (commonLength < settings_.minLength) {
        continue;
      }

      // Each external operand represents a parameter to the outlined function.
      const auto numParameters = distinctExternalOperandCount(expressions);
      if (numParameters < settings_.minParameters ||
          numParameters > settings_.maxParameters) {
        continue;
      }

      // Rough cost model: the call overhead and frame overhead are linear
      // functions of the number of parameters.
      const auto callOverhead = 2 + numParameters;
      const auto frameOverhead = 5 + numParameters;
      std::vector<Candidate> candidates{
          Candidate(index0, commonLength, callOverhead),
          Candidate(index1, commonLength, callOverhead)};

      // Try to include other candidates besides 0 and 1.
      for (unsigned i = 2, e = startIndices.size(); i < e; ++i) {
        const auto startIdx = startIndices[i] + offset;
        if (matchesCommonPrefix(expressions, escapeAnalysis, startIdx)) {
          candidates.emplace_back(startIdx, commonLength, callOverhead);
        }
      }

      // Add the outlined function to the result.
      functions.emplace_back(
          std::move(candidates), commonLength, frameOverhead);
    }
  }
};

/// DenseMapInfo trait to compare Instruction pointers by the instruction
/// variety and literal operands (if any), rather than by pointer.
struct InstructionValueInfo : llvm::DenseMapInfo<Instruction *> {
  /// Vector used to store information about an instruction's literal operands.
  using LiteralVec = llvm::SmallVector<uintptr_t, 4>;

  /// \return A vector containing an index and Literal pointer for each of the
  /// literal operands of \p inst.
  static LiteralVec getLiteralVec(const Instruction *const &inst) {
    LiteralVec vec;
    for (unsigned i = 0, e = inst->getNumOperands(); i != e; ++i) {
      Value *value = inst->getOperand(i);
      if (isa<Literal>(value)) {
        // Store the index so that instructions with the same sequence of
        // literal values at different positions don't have the same LiteralVec.
        vec.push_back(i);
        // We rely on the fact that all literals are interned in the module, so
        // we can store the Literal* instead of its underlying value.
        vec.push_back(reinterpret_cast<uintptr_t>(value));
      }
    }
    return vec;
  }

  static unsigned getHashValue(const Instruction *const &inst) {
    auto vec = getLiteralVec(inst);
    llvm::hash_code hash = llvm::hash_combine_range(vec.begin(), vec.end());
    hash = llvm::hash_combine(hash, inst->getVariety(), inst->getNumOperands());
    return static_cast<unsigned>(hash);
  }

  static bool isEqual(
      const Instruction *const &lhs,
      const Instruction *const &rhs) {
    if (rhs == getEmptyKey() || rhs == getTombstoneKey() ||
        lhs == getEmptyKey() || lhs == getTombstoneKey()) {
      return lhs == rhs;
    }
    if (lhs->getVariety() != rhs->getVariety() ||
        lhs->getNumOperands() != rhs->getNumOperands()) {
      return false;
    }
    return getLiteralVec(lhs) == getLiteralVec(rhs);
  }
};

} // anonymous namespace

/// \return True if \p inst is safe to extract into an outlined fuction.
static bool instructionIsLegalToOutline(Instruction *inst) {
  if (isa<PhiInst>(inst) || isa<TerminatorInst>(inst) ||
      isa<CreateArgumentsInst>(inst) || isa<AllocStackInst>(inst) ||
      isa<LoadStackInst>(inst) || isa<StoreStackInst>(inst)) {
    return false;
  }
  for (unsigned i = 0, e = inst->getNumOperands(); i < e; ++i) {
    if (isa<Variable>(inst->getOperand(i))) {
      return false;
    }
  }
  return true;
}

/// Convert \p M into a vector of unsigned integers.
///
/// Two instructions get assigned the same number if they are equivalent
/// according to InstrExpressionTrait. Basic block terminators and other
/// instructions that are illegal to outline are representing by unique numbers.
/// Blocks smaller than \p settings.minLength are not included in the result.
/// The resulting \p unsignedVec is suitable for constructing a suffix tree.
///
/// \param[out] unsignedVec The resulting string of numbers
/// \param[out] instructions A vector of the same length as \p unsignedVec that
///   stores the instructions corresponding to the numbers
/// \param M The module to convert
/// \param settings The outlining settings.
static void convertModuleToUnsignedVec(
    std::vector<unsigned> &unsignedVec,
    std::vector<Instruction *> &instructions,
    Module *M,
    const OutliningSettings &settings) {
  unsignedVec.clear();
  instructions.clear();

  // Build a map from instructions to integers using InstructionValueInfo. The
  // numbers start at 0 and count upward. We store them in the map so that
  // equivalent instructions get assigned to the same number.
  llvm::DenseMap<Instruction *, unsigned, InstructionValueInfo> map;
  unsigned legal = 0;

  // Illegal instructions are ones that cannot be outlined. We don't store them
  // in the map, so they all get assigned to unique numbers. The numbers start
  // near UINT_MAX and count downward.
  unsigned illegal = static_cast<unsigned>(-3);
  bool lastWasIllegal = true;

  // We set illegal to -3 to avoid conflicting with DenseMapInfo<unsigned>.
  // Although the DenseMap used in this function does not have unsigned keys,
  // the numbers that go into unsignedVec will later be used as map keys in
  // llvm::SuffixTree.
  assert(
      llvm::DenseMapInfo<unsigned>::getEmptyKey() ==
          static_cast<unsigned>(-1) &&
      "DenseMapInfo<unsigned> empty key is not -1");
  assert(
      llvm::DenseMapInfo<unsigned>::getTombstoneKey() ==
          static_cast<unsigned>(-2) &&
      "DenseMapInfo<unsigned> tombstone key is not -2");

  for (Function &F : *M) {
    for (BasicBlock &BB : F) {
      // Don't include the block if it's too small to be worth outlining.
      if (BB.size() < settings.minLength) {
        continue;
      }

      for (Instruction &inst : BB) {
        assert(legal < illegal && "Legal and illegal numbers collided!");
        if (instructionIsLegalToOutline(&inst)) {
          instructions.push_back(&inst);
          auto result = map.try_emplace(&inst, legal);
          if (result.second) {
            // The insertion was successful, so this is a new instruction.
            unsignedVec.push_back(legal);
            ++legal;
          } else {
            // The insertion failed: the instruction was already in the map.
            const unsigned existingNumber = result.first->second;
            unsignedVec.push_back(existingNumber);
          }
          lastWasIllegal = false;
        } else if (!lastWasIllegal) {
          instructions.push_back(&inst);
          unsignedVec.push_back(illegal);
          --illegal;
          // Remember that the last instruction was illegal so that we don't
          // waste space inserting multiple illegal numbers in a row.
          lastWasIllegal = true;
        }
      }
    }
  }

  assert(
      unsignedVec.size() == instructions.size() &&
      "Numbers and corresponding instructions are not the same size!");
}

/// Build an operand for an instruction in an outlined function.
///
/// \param operand An operand from an InstructionNumbering expression.
/// \param function The outlined function being built.
/// \param builder Builder to use for creating new parameters.
/// \param instructions Instructions created so far, used for Internal operands.
static Value *buildOutlinedOperand(
    const InstructionNumbering::Operand &operand,
    Function *function,
    IRBuilder &builder,
    llvm::ArrayRef<Instruction *> instructions) {
  switch (operand.kind) {
    // Internal operand: look up the instruction in the current block.
    case InstructionNumbering::OperandKind::Internal:
      assert(operand.index < instructions.size() && "Use before definition!");
      return instructions[operand.index];

    // External operand: look up the parameter, creating it if necessary.
    case InstructionNumbering::OperandKind::External:
      if (operand.index >= function->getParameters().size()) {
        assert(
            operand.index == function->getParameters().size() &&
            "External index skipped a number!");
        const auto name = getParameterName(builder.getModule(), operand.index);
        builder.createParameter(function, name);
      }
      return function->getParameters()[operand.index];

    // Value operand: just copy the Value pointer.
    case InstructionNumbering::OperandKind::Value:
      return operand.valuePtr;
  }
  llvm_unreachable("Invalid OperandKind!");
}

/// Build a Function for the given OutlinedFunction.
///
/// \param functionInfo Group of candidates to create a function for.
/// \param functionName Name of the new function.
/// \param M Module to add the function to.
/// \param instructions List of instructions corresponding to elements in the
///   suffix tree input.
/// \param settings The outlining settings.
/// \return The newly created function.
static Function *buildOutlinedFunction(
    const OutlinedFunction &functionInfo,
    Identifier functionName,
    Module *M,
    llvm::ArrayRef<Instruction *> instructions,
    const OutliningSettings &settings) {
  // Get the first candidate of this OutlinedFunction that isn't pruned.
  const Candidate *candidate = nullptr;
  for (const Candidate &c : functionInfo.Candidates) {
    if (!c.isDeleted()) {
      candidate = &c;
      break;
    }
  }
  assert(candidate != nullptr && "OutlinedFunction has no Candidate!");

  // Get the basic block range corresponding to the first candidate.
  auto firstIter = instructions[candidate->getStartIdx()]->getIterator();
  auto lastIter = instructions[candidate->getEndIdx()]->getIterator();
  BasicBlock::range range(firstIter, std::next(lastIter));

  // Perform escape analysis to find which instruction will be the return value.
  InstructionEscapeAnalysis escapeAnalysis;
  escapeAnalysis.addRange(range);
  const auto prefix = escapeAnalysis.longestPrefix();
  assert(
      prefix.length == candidate->getLength() &&
      "Candidate has more than one value escape!");

  // Use IRBuilder to create the function and its entry block.
  IRBuilder builder(M);
  auto *candidateFunction = firstIter->getParent()->getParent();
  const bool strictMode = candidateFunction->isStrictMode();
  auto *insertBefore = settings.placeNearCaller ? candidateFunction : nullptr;
  auto *function = builder.createFunction(
      functionName,
      Function::DefinitionKind::ES5Function,
      strictMode,
      SMRange{},
      false,
      insertBefore);
  auto *block = builder.createBasicBlock(function);
  builder.setInsertionBlock(block);

  // Clone instructions from the first candidate.
  std::vector<Instruction *> blockInstrs;
  blockInstrs.reserve(prefix.length);
  InstructionNumbering numbering(range, NUMBERING_FLAGS);
  for (auto it = numbering.begin(), end = numbering.end(); it != end; ++it) {
    llvm::SmallVector<Value *, 3> newOperands;
    for (auto &operand : it->operands) {
      newOperands.push_back(
          buildOutlinedOperand(operand, function, builder, blockInstrs));
    }
    // Clone the instruction and insert it at the end of the block.
    auto *newInst = builder.cloneInst(it.getInstruction(), newOperands);
    blockInstrs.push_back(newInst);
  }

  // Create the "this" parameter.
  builder.createParameter(function, "this");

  // Insert the return statement.
  Value *returnValue;
  if (prefix.offset.hasValue()) {
    returnValue = blockInstrs[prefix.offset.getValue()];
  } else {
    returnValue = builder.getLiteralUndefined();
  }
  builder.createReturnInst(returnValue);

  return function;
}

/// Try to replace an outlining candidate with a call to \p function.
///
/// \param candidate Candidate to outline.
/// \param function Function created for this candidate's OutlinedFunction.
/// \param instructions List of instructions corresponding to elements in the
///   suffix tree input.
/// \return True if \p candidate was outlined.
static bool outlineCandidate(
    const Candidate &candidate,
    Function *function,
    llvm::ArrayRef<Instruction *> instructions) {
  // Get the basic block range corresponding to the candidate.
  auto firstIter = instructions[candidate.getStartIdx()]->getIterator();
  auto lastIter = instructions[candidate.getEndIdx()]->getIterator();
  BasicBlock::range range(firstIter, std::next(lastIter));

  // Make sure the strict mode setting is compatible.
  if (firstIter->getParent()->getParent()->isStrictMode() !=
      function->isStrictMode()) {
    return false;
  }

  // Perform escape analysis to find which values after the function call to
  // replace with its return value.
  InstructionEscapeAnalysis escapeAnalysis;
  escapeAnalysis.addRange(range);
  const auto prefix = escapeAnalysis.longestPrefix();
  assert(
      prefix.length == candidate.getLength() &&
      "Candidate has more than one value escape!");

  // Collect the arguments to pass to the outlined function.
  llvm::SmallVector<Value *, 8> arguments;
  Instruction *escapeInst = nullptr;
  unsigned exprIndex = 0;
  InstructionNumbering numbering(range, NUMBERING_FLAGS);
  for (auto exprIt = numbering.begin(), exprEnd = numbering.end();
       exprIt != exprEnd;
       ++exprIt, ++exprIndex) {
    auto *inst = exprIt.getInstruction();
    unsigned opIndex = 0;
    for (auto opIt = exprIt->operands.begin(), opEnd = exprIt->operands.end();
         opIt != opEnd;
         ++opIt, ++opIndex) {
      if (opIt->kind == InstructionNumbering::OperandKind::External &&
          opIt->index >= arguments.size()) {
        assert(
            opIt->index == arguments.size() &&
            "External index skipped a number!");
        assert(
            opIndex <= inst->getNumOperands() &&
            "Operand index out of bounds!");
        arguments.push_back(inst->getOperand(opIndex));
      }
    }
    // Record the instruction that will be replaced with the return value.
    if (prefix.offset == InstructionEscapeAnalysis::EscapeOffset(exprIndex)) {
      escapeInst = inst;
    }
  }
  assert(
      bool(escapeInst) == prefix.offset.hasValue() &&
      "escapeInst inconsistent with prefix.offset");

  // Insert the call to the outlined function.
  IRBuilder builder(function);
  builder.setInsertionPoint(&*firstIter);
  auto *returnValue = builder.createHBCCallDirectInst(
      function, builder.getLiteralUndefined(), arguments);
  if (escapeInst) {
    escapeInst->replaceAllUsesWith(returnValue);
  }

  // Erase the candidate's instructions. Do it in reverse order so that all uses
  // of an instruction are removed before the instruction itself.
  Instruction *instToErase = &*lastIter;
  while (instToErase != returnValue) {
    assert(
        instToErase->getParent() == builder.getInsertionBlock() &&
        "Instructions should all be in the same block!");
    assert(
        instToErase->getNumUsers() == 0 &&
        "Instruction about to be erased should have no users!");
    Instruction *prev = instToErase->getPrevNode();
    instToErase->eraseFromParent();
    instToErase = prev;
  }

  return true;
}

/// Run one round of outlining on \p M.
/// \return True if it outlined anything.
static bool outlineModuleOnce(Module *M, const OutliningSettings &settings) {
  // Convert the module to a string of numbers and feed it to the LLVM outliner.
  std::vector<unsigned> unsignedVec;
  std::vector<Instruction *> instructions;
  convertModuleToUnsignedVec(unsignedVec, instructions, M, settings);
  HermesOutlinerTarget target(settings, instructions);
  std::vector<OutlinedFunction> functions;
  llvm::outliner::getFunctionsToOutline(functions, unsignedVec, &target);

  // Outline based on the results of getFunctionsToOutline.
  const Identifier functionName = M->getContext().getIdentifier(FUNCTION_NAME);
  bool changed = false;
  for (OutlinedFunction &functionInfo : functions) {
    // Don't outline if it's not beneficial.
    if (functionInfo.getBenefit() < 1) {
      continue;
    }

    Function *function = nullptr;
    unsigned numOutlined = 0;
    for (Candidate &candidate : functionInfo.Candidates) {
      // Skip candidates that were pruned.
      if (candidate.isDeleted()) {
        continue;
      }
      if (!function) {
        function = buildOutlinedFunction(
            functionInfo, functionName, M, instructions, settings);
        ++NumFunctionsCreated;
      }
      // Replace the candidate with a call to the new function.
      if (outlineCandidate(candidate, function, instructions)) {
        changed = true;
        ++numOutlined;
      }
    }
    NumCandidatesOutlined += numOutlined;
    NumInstructionsSaved += (numOutlined - 1) * functionInfo.SequenceSize;
  }

  return changed;
}

bool Outlining::runOnModule(Module *M) {
  if (!M->getContext().getOptimizationSettings().outlining) {
    return false;
  }

  const OutliningSettings &settings =
      M->getContext().getOptimizationSettings().outliningSettings;
  LLVM_DEBUG(
      dbgs() << "Outliner: Running on all functions"
             << "\nOutliner: placeNearCaller = " << settings.placeNearCaller
             << "\nOutliner: maxRounds = " << settings.maxRounds
             << "\nOutliner: minLength = " << settings.minLength
             << "\nOutliner: minParameters = " << settings.minParameters
             << "\nOutliner: maxParameters = " << settings.maxParameters
             << "\n");

  bool changed = false;
  for (unsigned i = 0; i < settings.maxRounds; ++i) {
    if (!outlineModuleOnce(M, settings)) {
      // If it didn't find anything to outline, neither will another round.
      break;
    }
    ++NumOutliningRounds;
    changed = true;
  }
  return changed;
}

Pass *createOutlining() {
  return new Outlining();
}

} // namespace hermes
