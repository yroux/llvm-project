//===- NVPTXUtilities.cpp - Utility Functions -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains miscellaneous utility functions
//
//===----------------------------------------------------------------------===//

#include "NVPTXUtilities.h"
#include "NVPTX.h"
#include "NVPTXTargetMachine.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/Mutex.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace llvm {

namespace {
typedef std::map<std::string, std::vector<unsigned> > key_val_pair_t;
typedef std::map<const GlobalValue *, key_val_pair_t> global_val_annot_t;

struct AnnotationCache {
  sys::Mutex Lock;
  std::map<const Module *, global_val_annot_t> Cache;
};

AnnotationCache &getAnnotationCache() {
  static AnnotationCache AC;
  return AC;
}
} // anonymous namespace

void clearAnnotationCache(const Module *Mod) {
  auto &AC = getAnnotationCache();
  std::lock_guard<sys::Mutex> Guard(AC.Lock);
  AC.Cache.erase(Mod);
}

static void cacheAnnotationFromMD(const MDNode *md, key_val_pair_t &retval) {
  auto &AC = getAnnotationCache();
  std::lock_guard<sys::Mutex> Guard(AC.Lock);
  assert(md && "Invalid mdnode for annotation");
  assert((md->getNumOperands() % 2) == 1 && "Invalid number of operands");
  // start index = 1, to skip the global variable key
  // increment = 2, to skip the value for each property-value pairs
  for (unsigned i = 1, e = md->getNumOperands(); i != e; i += 2) {
    // property
    const MDString *prop = dyn_cast<MDString>(md->getOperand(i));
    assert(prop && "Annotation property not a string");

    // value
    ConstantInt *Val = mdconst::dyn_extract<ConstantInt>(md->getOperand(i + 1));
    assert(Val && "Value operand not a constant int");

    std::string keyname = prop->getString().str();
    if (retval.find(keyname) != retval.end())
      retval[keyname].push_back(Val->getZExtValue());
    else {
      std::vector<unsigned> tmp;
      tmp.push_back(Val->getZExtValue());
      retval[keyname] = tmp;
    }
  }
}

static void cacheAnnotationFromMD(const Module *m, const GlobalValue *gv) {
  auto &AC = getAnnotationCache();
  std::lock_guard<sys::Mutex> Guard(AC.Lock);
  NamedMDNode *NMD = m->getNamedMetadata("nvvm.annotations");
  if (!NMD)
    return;
  key_val_pair_t tmp;
  for (unsigned i = 0, e = NMD->getNumOperands(); i != e; ++i) {
    const MDNode *elem = NMD->getOperand(i);

    GlobalValue *entity =
        mdconst::dyn_extract_or_null<GlobalValue>(elem->getOperand(0));
    // entity may be null due to DCE
    if (!entity)
      continue;
    if (entity != gv)
      continue;

    // accumulate annotations for entity in tmp
    cacheAnnotationFromMD(elem, tmp);
  }

  if (tmp.empty()) // no annotations for this gv
    return;

  if (AC.Cache.find(m) != AC.Cache.end())
    AC.Cache[m][gv] = std::move(tmp);
  else {
    global_val_annot_t tmp1;
    tmp1[gv] = std::move(tmp);
    AC.Cache[m] = std::move(tmp1);
  }
}

bool findOneNVVMAnnotation(const GlobalValue *gv, const std::string &prop,
                           unsigned &retval) {
  auto &AC = getAnnotationCache();
  std::lock_guard<sys::Mutex> Guard(AC.Lock);
  const Module *m = gv->getParent();
  if (AC.Cache.find(m) == AC.Cache.end())
    cacheAnnotationFromMD(m, gv);
  else if (AC.Cache[m].find(gv) == AC.Cache[m].end())
    cacheAnnotationFromMD(m, gv);
  if (AC.Cache[m][gv].find(prop) == AC.Cache[m][gv].end())
    return false;
  retval = AC.Cache[m][gv][prop][0];
  return true;
}

bool findAllNVVMAnnotation(const GlobalValue *gv, const std::string &prop,
                           std::vector<unsigned> &retval) {
  auto &AC = getAnnotationCache();
  std::lock_guard<sys::Mutex> Guard(AC.Lock);
  const Module *m = gv->getParent();
  if (AC.Cache.find(m) == AC.Cache.end())
    cacheAnnotationFromMD(m, gv);
  else if (AC.Cache[m].find(gv) == AC.Cache[m].end())
    cacheAnnotationFromMD(m, gv);
  if (AC.Cache[m][gv].find(prop) == AC.Cache[m][gv].end())
    return false;
  retval = AC.Cache[m][gv][prop];
  return true;
}

bool isTexture(const Value &val) {
  if (const GlobalValue *gv = dyn_cast<GlobalValue>(&val)) {
    unsigned annot;
    if (findOneNVVMAnnotation(gv, "texture", annot)) {
      assert((annot == 1) && "Unexpected annotation on a texture symbol");
      return true;
    }
  }
  return false;
}

bool isSurface(const Value &val) {
  if (const GlobalValue *gv = dyn_cast<GlobalValue>(&val)) {
    unsigned annot;
    if (findOneNVVMAnnotation(gv, "surface", annot)) {
      assert((annot == 1) && "Unexpected annotation on a surface symbol");
      return true;
    }
  }
  return false;
}

bool isSampler(const Value &val) {
  const char *AnnotationName = "sampler";

  if (const GlobalValue *gv = dyn_cast<GlobalValue>(&val)) {
    unsigned annot;
    if (findOneNVVMAnnotation(gv, AnnotationName, annot)) {
      assert((annot == 1) && "Unexpected annotation on a sampler symbol");
      return true;
    }
  }
  if (const Argument *arg = dyn_cast<Argument>(&val)) {
    const Function *func = arg->getParent();
    std::vector<unsigned> annot;
    if (findAllNVVMAnnotation(func, AnnotationName, annot)) {
      if (is_contained(annot, arg->getArgNo()))
        return true;
    }
  }
  return false;
}

bool isImageReadOnly(const Value &val) {
  if (const Argument *arg = dyn_cast<Argument>(&val)) {
    const Function *func = arg->getParent();
    std::vector<unsigned> annot;
    if (findAllNVVMAnnotation(func, "rdoimage", annot)) {
      if (is_contained(annot, arg->getArgNo()))
        return true;
    }
  }
  return false;
}

bool isImageWriteOnly(const Value &val) {
  if (const Argument *arg = dyn_cast<Argument>(&val)) {
    const Function *func = arg->getParent();
    std::vector<unsigned> annot;
    if (findAllNVVMAnnotation(func, "wroimage", annot)) {
      if (is_contained(annot, arg->getArgNo()))
        return true;
    }
  }
  return false;
}

bool isImageReadWrite(const Value &val) {
  if (const Argument *arg = dyn_cast<Argument>(&val)) {
    const Function *func = arg->getParent();
    std::vector<unsigned> annot;
    if (findAllNVVMAnnotation(func, "rdwrimage", annot)) {
      if (is_contained(annot, arg->getArgNo()))
        return true;
    }
  }
  return false;
}

bool isImage(const Value &val) {
  return isImageReadOnly(val) || isImageWriteOnly(val) || isImageReadWrite(val);
}

bool isManaged(const Value &val) {
  if(const GlobalValue *gv = dyn_cast<GlobalValue>(&val)) {
    unsigned annot;
    if (findOneNVVMAnnotation(gv, "managed", annot)) {
      assert((annot == 1) && "Unexpected annotation on a managed symbol");
      return true;
    }
  }
  return false;
}

std::string getTextureName(const Value &val) {
  assert(val.hasName() && "Found texture variable with no name");
  return std::string(val.getName());
}

std::string getSurfaceName(const Value &val) {
  assert(val.hasName() && "Found surface variable with no name");
  return std::string(val.getName());
}

std::string getSamplerName(const Value &val) {
  assert(val.hasName() && "Found sampler variable with no name");
  return std::string(val.getName());
}

bool getMaxNTIDx(const Function &F, unsigned &x) {
  return findOneNVVMAnnotation(&F, "maxntidx", x);
}

bool getMaxNTIDy(const Function &F, unsigned &y) {
  return findOneNVVMAnnotation(&F, "maxntidy", y);
}

bool getMaxNTIDz(const Function &F, unsigned &z) {
  return findOneNVVMAnnotation(&F, "maxntidz", z);
}

bool getMaxClusterRank(const Function &F, unsigned &x) {
  return findOneNVVMAnnotation(&F, "maxclusterrank", x);
}

bool getReqNTIDx(const Function &F, unsigned &x) {
  return findOneNVVMAnnotation(&F, "reqntidx", x);
}

bool getReqNTIDy(const Function &F, unsigned &y) {
  return findOneNVVMAnnotation(&F, "reqntidy", y);
}

bool getReqNTIDz(const Function &F, unsigned &z) {
  return findOneNVVMAnnotation(&F, "reqntidz", z);
}

bool getMinCTASm(const Function &F, unsigned &x) {
  return findOneNVVMAnnotation(&F, "minctasm", x);
}

bool getMaxNReg(const Function &F, unsigned &x) {
  return findOneNVVMAnnotation(&F, "maxnreg", x);
}

bool isKernelFunction(const Function &F) {
  unsigned x = 0;
  bool retval = findOneNVVMAnnotation(&F, "kernel", x);
  if (!retval) {
    // There is no NVVM metadata, check the calling convention
    return F.getCallingConv() == CallingConv::PTX_Kernel;
  }
  return (x == 1);
}

MaybeAlign getAlign(const Function &F, unsigned Index) {
  // First check the alignstack metadata
  if (MaybeAlign StackAlign =
          F.getAttributes().getAttributes(Index).getStackAlignment())
    return StackAlign;

  // If that is missing, check the legacy nvvm metadata
  std::vector<unsigned> Vs;
  bool retval = findAllNVVMAnnotation(&F, "align", Vs);
  if (!retval)
    return std::nullopt;
  for (unsigned V : Vs)
    if ((V >> 16) == Index)
      return Align(V & 0xFFFF);

  return std::nullopt;
}

MaybeAlign getAlign(const CallInst &I, unsigned Index) {
  // First check the alignstack metadata
  if (MaybeAlign StackAlign =
          I.getAttributes().getAttributes(Index).getStackAlignment())
    return StackAlign;

  // If that is missing, check the legacy nvvm metadata
  if (MDNode *alignNode = I.getMetadata("callalign")) {
    for (int i = 0, n = alignNode->getNumOperands(); i < n; i++) {
      if (const ConstantInt *CI =
              mdconst::dyn_extract<ConstantInt>(alignNode->getOperand(i))) {
        unsigned V = CI->getZExtValue();
        if ((V >> 16) == Index)
          return Align(V & 0xFFFF);
        if ((V >> 16) > Index)
          return std::nullopt;
      }
    }
  }
  return std::nullopt;
}

Function *getMaybeBitcastedCallee(const CallBase *CB) {
  return dyn_cast<Function>(CB->getCalledOperand()->stripPointerCasts());
}

bool shouldEmitPTXNoReturn(const Value *V, const TargetMachine &TM) {
  const auto &ST =
      *static_cast<const NVPTXTargetMachine &>(TM).getSubtargetImpl();
  if (!ST.hasNoReturn())
    return false;

  assert((isa<Function>(V) || isa<CallInst>(V)) &&
         "Expect either a call instruction or a function");

  if (const CallInst *CallI = dyn_cast<CallInst>(V))
    return CallI->doesNotReturn() &&
           CallI->getFunctionType()->getReturnType()->isVoidTy();

  const Function *F = cast<Function>(V);
  return F->doesNotReturn() &&
         F->getFunctionType()->getReturnType()->isVoidTy() &&
         !isKernelFunction(*F);
}

bool Isv2x16VT(EVT VT) {
  return (VT == MVT::v2f16 || VT == MVT::v2bf16 || VT == MVT::v2i16);
}

} // namespace llvm
