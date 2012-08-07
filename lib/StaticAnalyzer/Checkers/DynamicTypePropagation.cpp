//== DynamicTypePropagation.cpp ----------------------------------- -*- C++ -*--=//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This checker defines the rules for dynamic type gathering and propagation.
//
//===----------------------------------------------------------------------===//

#include "ClangSACheckers.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/Basic/Builtins.h"

using namespace clang;
using namespace ento;

namespace {
class DynamicTypePropagation:
    public Checker< check::PostCall,
                    check::PostStmt<ImplicitCastExpr> > {
  const ObjCObjectType *getObjectTypeForAllocAndNew(const ObjCMessageExpr *MsgE,
                                                    CheckerContext &C) const;

  /// \brief Return a better dynamic type if one can be derived from the cast.
  const ObjCObjectPointerType *getBetterObjCType(const Expr *CastE,
                                                 CheckerContext &C) const;
public:
  void checkPostCall(const CallEvent &Call, CheckerContext &C) const;
  void checkPostStmt(const ImplicitCastExpr *CastE, CheckerContext &C) const;
};
}

void DynamicTypePropagation::checkPostCall(const CallEvent &Call,
                                           CheckerContext &C) const {
  // We can obtain perfect type info for return values from some calls.
  if (const ObjCMethodCall *Msg = dyn_cast<ObjCMethodCall>(&Call)) {

    // Get the returned value if it's a region.
    SVal Result = C.getSVal(Call.getOriginExpr());
    const MemRegion *RetReg = Result.getAsRegion();
    if (!RetReg)
      return;

    ProgramStateRef State = C.getState();

    switch (Msg->getMethodFamily()) {
    default:
      break;

    // We assume that the type of the object returned by alloc and new are the
    // pointer to the object of the class specified in the receiver of the
    // message.
    case OMF_alloc:
    case OMF_new: {
      // Get the type of object that will get created.
      const ObjCMessageExpr *MsgE = Msg->getOriginExpr();
      const ObjCObjectType *ObjTy = getObjectTypeForAllocAndNew(MsgE, C);
      if (!ObjTy)
        return;
      QualType DynResTy =
                 C.getASTContext().getObjCObjectPointerType(QualType(ObjTy, 0));
      C.addTransition(State->addDynamicTypeInfo(RetReg, DynResTy));
      break;
    }
    case OMF_init: {
      // Assume, the result of the init method has the same dynamic type as
      // the receiver and propagate the dynamic type info.
      const MemRegion *RecReg = Msg->getReceiverSVal().getAsRegion();
      assert(RecReg);
      DynamicTypeInfo RecDynType = State->getDynamicTypeInfo(RecReg);
      C.addTransition(State->addDynamicTypeInfo(RetReg, RecDynType));
      break;
    }
    }
  }
}

void DynamicTypePropagation::checkPostStmt(const ImplicitCastExpr *CastE,
                                           CheckerContext &C) const {
  // We only track dynamic type info for regions.
  const MemRegion *ToR = C.getSVal(CastE).getAsRegion();
  if (!ToR)
    return;

  switch (CastE->getCastKind()) {
  default:
    break;
  case CK_BitCast:
    // Only handle ObjCObjects for now.
    if (const Type *NewTy = getBetterObjCType(CastE, C))
      C.addTransition(C.getState()->addDynamicTypeInfo(ToR, QualType(NewTy,0)));
    break;
  }
  return;
}

const ObjCObjectType *
DynamicTypePropagation::getObjectTypeForAllocAndNew(const ObjCMessageExpr *MsgE,
                                                    CheckerContext &C) const {
  if (MsgE->getReceiverKind() == ObjCMessageExpr::Class) {
    if (const ObjCObjectType *ObjTy
          = MsgE->getClassReceiver()->getAs<ObjCObjectType>())
    return ObjTy;
  }

  if (MsgE->getReceiverKind() == ObjCMessageExpr::SuperClass) {
    if (const ObjCObjectType *ObjTy
          = MsgE->getSuperType()->getAs<ObjCObjectType>())
      return ObjTy;
  }

  const Expr *RecE = MsgE->getInstanceReceiver();
  if (!RecE)
    return 0;

  RecE= RecE->IgnoreParenImpCasts();
  if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(RecE)) {
    const StackFrameContext *SFCtx = C.getCurrentStackFrame();
    // Are we calling [self alloc]? If this is self, get the type of the
    // enclosing ObjC class.
    if (DRE->getDecl() == SFCtx->getSelfDecl()) {
      if (const ObjCMethodDecl *MD = dyn_cast<ObjCMethodDecl>(SFCtx->getDecl()))
        if (const ObjCObjectType *ObjTy =
            dyn_cast<ObjCObjectType>(MD->getClassInterface()->getTypeForDecl()))
          return ObjTy;
    }
  }
  return 0;
}

// Return a better dynamic type if one can be derived from the cast.
// Compare the current dynamic type of the region and the new type to which we
// are casting. If the new type is lower in the inheritance hierarchy, pick it.
const ObjCObjectPointerType *
DynamicTypePropagation::getBetterObjCType(const Expr *CastE,
                                          CheckerContext &C) const {
  const MemRegion *ToR = C.getSVal(CastE).getAsRegion();
  assert(ToR);

  // Get the old and new types.
  const ObjCObjectPointerType *NewTy =
      CastE->getType()->getAs<ObjCObjectPointerType>();
  if (!NewTy)
    return 0;
  QualType OldDTy = C.getState()->getDynamicTypeInfo(ToR).getType();
  if (OldDTy.isNull()) {
    return NewTy;
  }
  const ObjCObjectPointerType *OldTy =
    OldDTy->getAs<ObjCObjectPointerType>();
  if (!OldTy)
    return 0;

  // Id the old type is 'id', the new one is more precise.
  if (OldTy->isObjCIdType() && !NewTy->isObjCIdType())
    return NewTy;

  // Return new if it's a subclass of old.
  const ObjCInterfaceDecl *ToI = NewTy->getInterfaceDecl();
  const ObjCInterfaceDecl *FromI = OldTy->getInterfaceDecl();
  if (ToI && FromI && FromI->isSuperClassOf(ToI))
    return NewTy;

  return 0;
}

void ento::registerDynamicTypePropagation(CheckerManager &mgr) {
  mgr.registerChecker<DynamicTypePropagation>();
}