"""Where the JIT's time went, from perf samples inside its translations.

A sample of the CPU thread inside a translate span (or of bcore's worker
thread, inside the promotions it compiles) is put in the stage of the
leaf-most frame that names one (SelectionDAG, FastISel, register
allocation, MC emission, the lifter, ...), and in the LLVM pass of the
nearest pass frame. malloc and free get the stage that called them: an
allocator leaf says where memory was asked for, not what for.
"""
from __future__ import annotations

import re
from typing import Optional, Sequence, Tuple

# Leaf-most first: the first of a sample's frames (leaf to root) that matches
# any of these decides its stage, and the order here breaks ties within one
# frame.
STAGES = (
    ("selection DAG", re.compile(r"SelectionDAG::|DAGCombiner|SelectionDAGBuilder|ScheduleDAG|"
                                 r"SelectionDAGISel::(DoInstructionSelection|CodeGenAndEmitDAG|SelectCode|MorphNode|"
                                 r"LowerArguments)|X86DAGToDAGISel|DAGTypeLegalizer|SelectionDAGLegalize|InstrEmitter|"
                                 r"TargetLowering::|X86TargetLowering::(Lower|Perform|getRegFor|Emit)|SDNode|"
                                 r"FunctionLoweringInfo::set")),
    ("FastISel", re.compile(r"FastISel")),
    ("register allocation", re.compile(r"RegAllocFast|RAGreedy|RegAllocBase|LiveIntervals|LiveRange|VirtRegMap|"
                                       r"SplitEditor|LiveRegMatrix")),
    ("MC emission", re.compile(r"AsmPrinter|MCAssembler|MCObjectStreamer|ELFObjectWriter|MCELFStreamer|"
                               r"MCCodeEmitter|MCExpr|MCStreamer|MCContext|MCSection|MCFragment")),
    ("object loading", re.compile(r"RuntimeDyld|ObjectFile|ELFObjectFile|ArenaMemoryManager|CodeArena")),
    ("ORC", re.compile(r"orc::")),
    ("copy-and-patch", re.compile(r"CpTranslator")),
    ("lift", re.compile(r"LiftVisitor|BBTranslator|decodeInstruction|RecordingMemory|ReplayMemory")),
    ("guest memory lowering", re.compile(r"lower_guest_memory")),
    ("profiling", re.compile(r"bcore_profile|ProfileState|onCompile|hash_material|takeObjectShape|JITEventListener|"
                             r"notifyObjectLoaded")),
    ("module teardown", re.compile(r"LLVMContext::~|LLVMContextImpl::~|Module::~Module|~LLVMContext")),
    ("machine pass", re.compile(r"::runOnMachineFunction")),
    ("IR pass", re.compile(r"::runOnFunction|::runOnModule|PassModel<")),
)
OTHER = "other"
ALLOCATOR = re.compile(r"^(mi_|_mi_|malloc|_int_malloc|_int_free|free|cfree|operator new|operator delete|"
                       r"__libc_malloc|__libc_free)")
LEGACY_PASS = re.compile(r"(?:\(anonymous namespace\)::|llvm::)?([A-Za-z0-9_]+)::runOn(?:Machine)?Function")
NEW_PM_PASS = re.compile(r"PassModel<llvm::(?:Function|Module|Loop|LazyCallGraph::SCC)[^,]*, "
                         r"(?:llvm::)?(?:\(anonymous namespace\)::)?([A-Za-z0-9_:]+)")
# Pass managers and adaptors: not passes of their own.
NOT_PASSES = {"MachineFunctionPass", "FPPassManager"}
# A frame of bcore's worker thread (Core::set_tier_background).
WORKER_FRAME = "CompileWorker::"


def stage(symbols: Sequence[str]) -> str:
    for symbol in symbols:
        for name, pattern in STAGES:
            if pattern.search(symbol):
                return name
    return OTHER


def pass_name(symbols: Sequence[str]) -> Optional[str]:
    """The nearest pass: a legacy pass's runOn(Machine)Function frame, else a
    new pass manager's PassModel; None outside any."""
    for symbol in symbols:
        match = LEGACY_PASS.search(symbol)
        if match and match.group(1) not in NOT_PASSES:
            return match.group(1)
    for symbol in symbols:
        match = NEW_PM_PASS.search(symbol)
        if match:
            return match.group(1)
    return None


def classify(symbols: Sequence[str]) -> Tuple[str, str]:
    """(stage, pass) of one sample, its frames leaf first. An allocator leaf
    keeps the stage of its callers, as "malloc in <stage>"."""
    found = stage(symbols)
    if symbols and ALLOCATOR.match(symbols[0].strip()):
        found = f"malloc in {found}"
    return found, pass_name(symbols) or "(no pass)"


def is_worker(symbols: Sequence[str]) -> bool:
    return any(WORKER_FRAME in symbol for symbol in symbols)
