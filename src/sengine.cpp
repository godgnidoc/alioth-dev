#ifndef __sengine_cpp__
#define __sengine_cpp__

#include "sengine.hpp"
#include "manager.hpp"
#include "xengine.hpp"
#include "methodimpl.hpp"
#include "insblockimpl.hpp"
#include "constructimpl.hpp"
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/FileSystem.h>
#include "llvm/IR/GlobalVariable.h"
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/ADT/Optional.h>
#include <llvm/Support/Host.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cctype>

namespace alioth {

bool Sengine::performDefinitionSemanticValidation( $modesc desc ) {

    if( mrepo.count(desc) == 0 ) return false;
    auto& mod = mrepo[desc];
    bool fine = true;

    for( auto& dep : desc->deps ) {
        //[条件不应成立] if( mrepo.count(dep->dest) != 1 ) return false;

        if( dep->alias.is(VT::iTHIS) ) {
            auto& dest = mrepo[dep->dest];
            mod->external += dest->internal;
            mod->extmeta += dest->metadefs;
        }
    }

    for( auto& def : mod->internal ) {
        if( auto cdef = ($ClassDef)def; cdef ) 
            fine = performDefinitionSemanticValidation(cdef) and fine;
        /*else if( auto edef = ($EnumDef)def; edef )
            fine = performDefinitionSemanticValidation(edef) and fine;
        */
    }

    vector<Type*> members;
    for( auto def : mod->metadefs ) {
        if( auto adef = ($AttrDef)def; adef ) {
            if( auto mty = performDefinitionSemanticValidation(adef); mty ) members.push_back(mty);
            else fine = false;
        } else if( auto mdef = ($MethodDef)def; mdef ) {
            fine = performDefinitionSemanticValidation(mdef) and fine;
        }
    }

    auto symbol = generateGlobalUniqueName(($node)mod,Meta);
    /*auto& modT = mmetaT[mod] =*/ 
    if( mnamedT.count(symbol) == 0 ) mnamedT[symbol] = StructType::create(mctx,symbol);
    auto lty = (StructType*)mnamedT[symbol];
    lty->setBody(members);

    return fine;
}

bool Sengine::performDefinitionSemanticValidation( $ClassDef clas ) {

    bool fine = true;

    auto perform = [&]( auto all, auto symbol, Type*& slot ) {
        if( !slot ) slot = StructType::create(mctx,symbol);
        map<string,$definition> nameT;
        vector<Type*> members;
        for( auto def : all ) {
            if( auto adef = ($AttrDef)def; adef ) {
                if( nameT.count((string)def->name) ) {
                    auto prev = nameT[(string)def->name];
                    mlogrepo(def->getDocPath())(Lengine::E2001,def->name,prev->getDocPath(),prev->name);
                    fine = false;
                } else if( auto mty = performDefinitionSemanticValidation(adef); mty ) {
                    nameT[(string)def->name] = def;
                    members.push_back(mty);
                } else {
                    fine = false;
                }
            }  else if( auto mdef = ($MethodDef)def; mdef ) {
                fine = performDefinitionSemanticValidation(mdef) and fine;
            }
        }
        ((StructType*)slot)->setBody(members);
    };

    auto instS = generateGlobalUniqueName(($node)clas);
    auto metaS = generateGlobalUniqueName(($node)clas,Meta);

    perform( clas->metadefs, metaS, mnamedT[metaS] );
    perform( clas->instdefs, instS, mnamedT[instS] );

    for( auto def : clas->internal ) {
        if( auto cdef = ($ClassDef)def; cdef ) fine = performDefinitionSemanticValidation(cdef) and fine;
        /*else if( auto edef = ($EnumDef)def; edef ) fine = performDefinitionSemanticValidation(edef) and fine;*/
    }
    return fine;
}

bool Sengine::performImplementationSemanticValidation( $ClassDef clas ) {
    auto tsymbol = generateGlobalUniqueName( ($node)clas, Meta );
    auto esymbol = generateGlobalUniqueName( ($node)clas, Entity );
    auto ty = (StructType*)mnamedT[tsymbol];
    if( !ty ) return false;
    if( ty->getNumElements() == 0 ) return true;

    new GlobalVariable(*mcurmod,ty,false,GlobalValue::ExternalLinkage,ConstantStruct::getNullValue(ty),esymbol);

    return true;
}

bool Sengine::performDefinitionSemanticValidation( $MethodDef method ) {
    vector<Type*> pts;
    auto rtp = generateTypeUsageAsReturnValue(method->rproto);
    auto tss = generateGlobalUniqueName(method->getScope(),method->meta?Meta:None);
    if( mnamedT.count(tss) == 0 ) mnamedT[tss] = StructType::create(mctx,tss);
    pts.push_back(mnamedT[tss]->getPointerTo());
    for( auto par : *method ) pts.push_back( generateTypeUsageAsParameter(par->proto) );
    auto ft = FunctionType::get(rtp,pts,false);
    auto fs = generateGlobalUniqueName(($node)method);
    mnamedT[fs] = ft;
    return ft != nullptr;
}

bool Sengine::performImplementationSemanticValidation( $MethodImpl method ) {
    
    auto fs = generateGlobalUniqueName(($node)method);
    auto ft = (FunctionType*)mnamedT[fs];
    auto fp = mcurmod->getFunction(fs);
    if( !fp ) fp = Function::Create(ft,GlobalValue::ExternalLinkage,fs,mcurmod.get());

    auto def = requestPrototype(($implementation)method);
    if( def->entry ) {
        auto start = Function::Create(
            FunctionType::get(
                Type::getInt32Ty(mctx),
                {Type::getInt32Ty(mctx),Type::getInt8PtrTy(mctx)->getPointerTo()},
                false
            ),
            GlobalValue::ExternalLinkage,
            "start",
            mcurmod.get()
        );
        auto ebb = BasicBlock::Create(mctx,"",start);
        auto builder = IRBuilder<>(ebb);
        vector<Value*> args;
        args.push_back(Constant::getNullValue(fp->arg_begin()->getType()));
        args.push_back(start->arg_begin());
        args.push_back(start->arg_begin()+1);
        auto ret = builder.CreateCall(fp,args);
        builder.CreateRet( ret );
    }

    auto ebb = BasicBlock::Create(mctx,"",fp);
    auto builder = IRBuilder<>(ebb);
    auto arg = fp->arg_begin();

    for( auto par : *method ) {
        arg += 1;
        arg->setName( (string)par->name );
        if( par->proto->elmt == VAR and par->proto->dtyp->cate == NAMED ) {
            mlocalV[par] = arg;
        } else {
            auto addr = builder.CreateAlloca(arg->getType());
            builder.CreateStore(arg,addr);
            mlocalV[par] = addr;
        }
    }

    flag_terminate = false;
    return performImplementationSemanticValidation( method->body, builder );
}

bool Sengine::performImplementationSemanticValidation( $implementation impl, IRBuilder<>& builder ) {
    if( flag_terminate ) return false;
    bool ret = true;
    if( auto ct = ($FlowCtrlImpl)impl; ct ) ret = performImplementationSemanticValidation( ct, builder);
    else if( auto ex = ($ExpressionImpl)impl; ex ) ret = performImplementationSemanticValidation( ex, builder);
    else if( auto el = ($ConstructImpl)impl; el ) ret = performImplementationSemanticValidation( el, builder);
    else if( auto br = ($BranchImpl)impl; br ) ret = performImplementationSemanticValidation( br, builder);
    else if( auto lp = ($LoopImpl)impl; lp ) ret = performImplementationSemanticValidation( lp, builder);
    else if( auto bk = ($InsBlockImpl)impl; bk ) ret = performImplementationSemanticValidation( bk, builder );
    return ret;
}

bool Sengine::performImplementationSemanticValidation( $InsBlockImpl  impl ,IRBuilder<>& builder ){
    if( flag_terminate ) return false;
    bool ret = true;

    for( auto imp : impl->impls )
        ret = performImplementationSemanticValidation( imp, builder ) and ret;

    return ret;
}

bool Sengine::performImplementationSemanticValidation( $FlowCtrlImpl impl, llvm::IRBuilder<>& builder ) {
    if( flag_terminate ) return false;
    switch( impl->action ) {
        case RETURN: {
            if( impl->expr ) {
                auto v = performImplementationSemanticValidation( impl->expr, builder );
                //[TODO] : generateBackendIR for <leave> method
                builder.CreateRet(v->asobject(builder));
                flag_terminate = true;
            } else {
                builder.CreateRetVoid();
                flag_terminate = true;
            }
            return true;
        } break;
        //[TODO] : case BREAK: generateBackendIR for <leave> block
        default:
            return true;
    }
}

$imm Sengine::performImplementationSemanticValidation( $ExpressionImpl impl, IRBuilder<>& builder ) {
    if( !impl or flag_terminate ) return nullptr;
    switch( impl->type ) {
        default: return nullptr;
        case ExpressionImpl::NAMEUSAGE: if( auto s = processNameusageExpression( impl, builder ); s.size() ) return s[0]; else return nullptr;
        case ExpressionImpl::MEMBER: if( auto s = processMemberExpression( impl, builder ); s.size() ) return s[0]; else return nullptr;
        case ExpressionImpl::VALUE: return processValueExpression( impl, builder );
        case ExpressionImpl::INFIX: return processCalcExpression( impl, builder );
        case ExpressionImpl::SUFFIX: return processCalcExpression( impl, builder );
        case ExpressionImpl::PREFIX: return processCalcExpression( impl, builder );
        case ExpressionImpl::CALL: return processCallExpression( impl, builder );
    }
}

imms Sengine::processNameusageExpression( $ExpressionImpl impl, IRBuilder<>& builder ) {
    if( impl->type != ExpressionImpl::NAMEUSAGE ) return {};

    imms ret;
    
    auto eve = request( impl->name, NormalClass );
    if( eve.size() == 0 ) {
        mlogrepo(impl->getDocPath())(Lengine::E2004,impl->name[-1].name);  
        return {};
    }
    for( auto e : eve ) {
        if( auto ce = ($ClassDef)e; ce ) {
            auto symbolE = generateGlobalUniqueName(($node)e,Entity);
            auto symbolT = generateGlobalUniqueName(($node)e,Meta);
            auto gt = (StructType*)mnamedT[symbolT];
            auto gv = mcurmod->getOrInsertGlobal(symbolE, gt);
            ret << imm::entity( gv, ce );
        } else if( auto cm = ($ConstructImpl)e; cm ) {
            if( mlocalV.count(cm) ) ret << imm::address( mlocalV[cm], cm->proto );
        } else if( auto ad = ($AttrDef)e; ad ) {
            auto sc = ($ClassDef)ad->getScope();
            auto symbolT = generateGlobalUniqueName(($node)sc,ad->meta?Meta:None);
            Type* stt = mnamedT[symbolT];
            Value* gep = nullptr;
            int index = 0;
            if( ad->meta ) {
                auto symbolE = generateGlobalUniqueName(($node)sc,Entity);
                auto symbolT = generateGlobalUniqueName(($node)sc,Meta);
                gep = mcurmod->getOrInsertGlobal(symbolE,mnamedT[symbolT]);
                while( sc->metadefs[index] != ad ) index++;  //[ATTENTION] : 危险
            } else {
                auto mdef = requestPrototype(($implementation)impl);
                gep = requestThis(($implementation)impl);
                while( sc->instdefs[index] != ad ) index++;   //[ATTENTION] : 危险
                /** if( mdef and !mdef->meta and gep ) continue; */
            }
            gep = builder.CreateStructGEP( stt, gep, index );
            if( gep ) ret << imm::address(gep,ad->proto);
        } else if( auto mt = ($MethodDef)e; mt ) {
            auto fs = generateGlobalUniqueName(($node)mt);
            auto gv = mcurmod->getFunction(fs);
            if( !gv ) gv = Function::Create((FunctionType*)mnamedT[fs],GlobalValue::ExternalLinkage,fs,mcurmod.get());
            ret << imm::function( gv, mt );
        }
    }

    if( ret.size() == 0 ) return {}; //保证返回结果不是空集,因为驱动函数要直接使用结果
    return ret;
}

imms Sengine::processMemberExpression( $ExpressionImpl impl, IRBuilder<>& builder ) {
    if( impl->type != ExpressionImpl::MEMBER ) return {};

    imms ret;
    auto host = performImplementationSemanticValidation( impl->sub[0], builder );
    Value* vhost = host->asaddress(builder);
    if( !vhost ) {
        mlogrepo(impl->getDocPath())(Lengine::E1010,impl->name[-1].name);  
        return {};
    }
    definitions search;

    if( auto def = host->metacls(); def ) {
        search = def->metadefs;
    } else if( auto proto = host->eproto(); proto ) {
        auto eve = request(proto->dtyp->name,NormalClass);
        if( eve.size() != 1 ) {
            mlogrepo(impl->getDocPath())(Lengine::E2004,impl->name[-1].name);  
            return {};
        }
        def = ($ClassDef)eve[0];
        if( def ) search = def->instdefs;
    }
    for( auto d : search )
        if( (string)d->name == (string)impl->mean ) {
            if( auto ad = ($AttrDef)d; ad ) {
                int index = 0; while( search[index] != d ) index ++;
                auto gep = builder.CreateStructGEP( mnamedT[generateGlobalUniqueName(($node)d,Meta)], vhost, index );
                ret << imm::address(gep,ad->proto,host);
            } else if( auto md = ($MethodDef)d; md ) {
                auto fs = generateGlobalUniqueName(($node)md);
                auto fp = mcurmod->getFunction(fs);
                if( !fp ) fp = Function::Create( (FunctionType*)mnamedT[fs], GlobalValue::ExternalLinkage, fs, mcurmod.get() );
                ret << imm::function(fp,md,host);
            }
        }

    
    return ret;
}

$imm Sengine::processValueExpression( $ExpressionImpl impl, IRBuilder<>& builder ) {
    if( impl->type != ExpressionImpl::VALUE ) return nullptr;
    switch( impl->mean.id ) {
        case VT::iINTEGERn: { // 限制10进制数字只能32位
            auto i = std::stoll(impl->mean);
            if( i > INT32_MAX or i < INT32_MIN ) return nullptr;
            return imm::object( 
                builder.getInt32(i),
                eproto::MakeUp(
                    impl->getScope(),
                    VAR,
                    dtype::MakeUp(impl->getScope(), token(VT::INT32) )
                )
            );
        } 
        case VT::iSTRING: {
            return imm::object(
                builder.CreateGlobalStringPtr( Xengine::extractText(impl->mean) ),
                eproto::MakeUp(
                    impl->getScope(),
                    PTR,
                    dtype::MakeUp(impl->getScope(), token(VT::INT8), {false} )
                )
            );
        }
        default : return nullptr;
    }
}

$imm Sengine::processCallExpression( $ExpressionImpl impl, llvm::IRBuilder<>& builder ) {
    if( impl->type != ExpressionImpl::CALL ) return nullptr;

    std::vector<Value*> args;
    auto ait = impl->sub.begin();
    auto fp = performImplementationSemanticValidation( *(ait++), builder );
    
    if( impl->sub[0]->type == ExpressionImpl::NAMEUSAGE ) {
        args.push_back(requestThis(($implementation)impl));
    } else if( impl->sub[0]->type == ExpressionImpl::MEMBER ) {
        args.push_back(fp->h->asaddress(builder));
    }
    
    while( ait != impl->sub.end() ) {
        auto p = performImplementationSemanticValidation( *(ait++), builder );
        if( !p ) return nullptr;
        args.push_back( p->asparameter(builder) );
    }

    return imm::object(builder.CreateCall(fp->asfunction(),args),fp->prototype()->rproto);  //[FIXME]object存疑
}

$imm Sengine::processCalcExpression( $ExpressionImpl impl, llvm::IRBuilder<>& builder ) {

    switch( impl->type ) {
        case ExpressionImpl::INFIX: {
            auto left = performImplementationSemanticValidation(impl->sub[0],builder);
            auto right = performImplementationSemanticValidation(impl->sub[1],builder);
            if( !left or !right ) return nullptr;
            Value* rv;
            $eproto proto;
            switch( impl->mean.id ) {
                default: break;
                case VT::GT: rv = builder.CreateICmpSGE(left->asobject(builder),right->asobject(builder));proto = eproto::MakeUp(impl->getScope(),VAR,dtype::MakeUp(impl->getScope(), token(VT::BOOL)));break;
                case VT::LT: rv = builder.CreateICmpSLT(left->asobject(builder),right->asobject(builder));proto = eproto::MakeUp(impl->getScope(),VAR,dtype::MakeUp(impl->getScope(), token(VT::BOOL)));break;
                case VT::LE: rv = builder.CreateICmpSLT(left->asobject(builder),right->asobject(builder));proto = eproto::MakeUp(impl->getScope(),VAR,dtype::MakeUp(impl->getScope(), token(VT::BOOL)));break;
                case VT::GE: rv = builder.CreateICmpSGE(left->asobject(builder),right->asobject(builder));proto = eproto::MakeUp(impl->getScope(),VAR,dtype::MakeUp(impl->getScope(), token(VT::BOOL)));break;
                case VT::EQ: rv = builder.CreateICmpEQ(left->asobject(builder),right->asobject(builder)); proto = eproto::MakeUp(impl->getScope(),VAR,dtype::MakeUp(impl->getScope(), token(VT::BOOL)));break;
                case VT::NE: rv = builder.CreateICmpNE(left->asobject(builder),right->asobject(builder)); proto = eproto::MakeUp(impl->getScope(),VAR,dtype::MakeUp(impl->getScope(), token(VT::BOOL)));break;

                case VT::AND: rv = builder.CreateAnd(left->asobject(builder),right->asobject(builder)); proto = eproto::MakeUp(impl->getScope(),VAR,dtype::MakeUp(impl->getScope(), token(VT::BOOL)));break;
                case VT::OR:  rv = builder.CreateOr(left->asobject(builder),right->asobject(builder)); proto = eproto::MakeUp(impl->getScope(),VAR,dtype::MakeUp(impl->getScope(), token(VT::BOOL)));break;

                case VT::PLUS:  rv = builder.CreateAdd(left->asobject(builder),right->asobject(builder)); proto = eproto::MakeUp(impl->getScope(), VAR, dtype::MakeUp(impl->getScope(), token(VT::INT32)));break;
                case VT::MINUS: rv = builder.CreateSub(left->asobject(builder),right->asobject(builder)); proto = eproto::MakeUp(impl->getScope(), VAR, dtype::MakeUp(impl->getScope(), token(VT::INT32)));break;
                case VT::MUL:   rv = builder.CreateMul(left->asobject(builder),right->asobject(builder)); proto = eproto::MakeUp(impl->getScope(), VAR, dtype::MakeUp(impl->getScope(), token(VT::INT32)));break;
                case VT::DIV:   rv = builder.CreateSDiv(left->asobject(builder),right->asobject(builder)); proto = eproto::MakeUp(impl->getScope(), VAR, dtype::MakeUp(impl->getScope(), token(VT::INT32)));break;
                case VT::MOL:   rv = builder.CreateSRem(left->asobject(builder),right->asobject(builder)); proto = eproto::MakeUp(impl->getScope(), VAR, dtype::MakeUp(impl->getScope(), token(VT::INT32)));break;
                case VT::bAND:  rv = builder.CreateAnd(left->asobject(builder),right->asobject(builder)); proto = eproto::MakeUp(impl->getScope(), VAR, dtype::MakeUp(impl->getScope(), token(VT::INT32)));break;
                case VT::bOR:   rv = builder.CreateOr(left->asobject(builder),right->asobject(builder)); proto = eproto::MakeUp(impl->getScope(), VAR, dtype::MakeUp(impl->getScope(), token(VT::INT32)));break;
                case VT::bXOR:  rv = builder.CreateXor(left->asobject(builder),right->asobject(builder)); proto = eproto::MakeUp(impl->getScope(), VAR, dtype::MakeUp(impl->getScope(), token(VT::INT32)));break;

                case VT::ASSIGN: builder.CreateStore(right->asobject(builder),left->asaddress(builder));return left;
                case VT::ASSIGN_PLUS:rv = builder.CreateAdd(left->asobject(builder),right->asobject(builder));builder.CreateStore(rv, left->asaddress(builder));return left;
                case VT::ASSIGN_MINUS:rv = builder.CreateSub(left->asobject(builder),right->asobject(builder));builder.CreateStore(rv, left->asaddress(builder));return left;
                case VT::ASSIGN_MUL:rv = builder.CreateMul(left->asobject(builder),right->asobject(builder));builder.CreateStore(rv, left->asaddress(builder));return left;
                case VT::ASSIGN_DIV:rv = builder.CreateSDiv(left->asobject(builder),right->asobject(builder));builder.CreateStore(rv, left->asaddress(builder));return left;
                case VT::ASSIGN_MOL:rv = builder.CreateSRem(left->asobject(builder),right->asobject(builder));builder.CreateStore(rv, left->asaddress(builder));return left;
                case VT::ASSIGN_SHL:rv = builder.CreateShl(left->asobject(builder),right->asobject(builder));builder.CreateStore(rv, left->asaddress(builder));return left;
                case VT::ASSIGN_SHR:rv = builder.CreateAShr(left->asobject(builder),right->asobject(builder));builder.CreateStore(rv, left->asaddress(builder));return left;
                case VT::ASSIGN_bAND:rv = builder.CreateAnd(left->asobject(builder),right->asobject(builder));builder.CreateStore(rv, left->asaddress(builder));return left;
                case VT::ASSIGN_bOR:rv = builder.CreateOr(left->asobject(builder),right->asobject(builder));builder.CreateStore(rv, left->asaddress(builder));return left;
                case VT::ASSIGN_bXOR:rv = builder.CreateXor(left->asobject(builder),right->asobject(builder));builder.CreateStore(rv, left->asaddress(builder));return left;
            }

            return imm::object( rv, proto );
        } break;
        case ExpressionImpl::SUFFIX: {
            auto operand = performImplementationSemanticValidation(impl->sub[0],builder);
            if( !operand ) return nullptr;
            Value* rv = operand->asobject(builder);
            switch( impl->mean.id ) {
                case VT::INCRESS: builder.CreateStore(builder.CreateAdd(rv,builder.getInt32(1)),operand->asaddress(builder));break;
                case VT::DECRESS: builder.CreateStore(builder.CreateSub(rv,builder.getInt32(1)),operand->asaddress(builder));break;
                case VT::OPENL: {
                    auto ind = performImplementationSemanticValidation(impl->sub[1],builder);
                    if( !ind ) return nullptr;
                    rv = builder.CreateGEP(rv,ind->asobject(builder));
                    rv = builder.CreateLoad(rv);
                    auto proto = operand->eproto();
                    proto->dtyp = proto->dtyp->load();
                    if( !proto->dtyp->cons.size() ) proto->elmt = VAR;
                    return imm::object(rv,proto);
                }
            }
            return imm::object(rv,operand->eproto());
        } break;
        case ExpressionImpl::PREFIX: {
            auto right = performImplementationSemanticValidation(impl->sub[0],builder);
            if( !right ) return nullptr;
            Value* rv = nullptr;
            switch( impl->mean.id ) {
                default: break;
                case VT::bAND: {
                    auto proto = right->eproto();
                    proto->dtyp = proto->dtyp->store();
                    proto->elmt = PTR;
                    return imm::object(right->asaddress(builder),proto);
                }
                case VT::MUL: {
                    auto proto = right->eproto();
                    proto->dtyp = proto->dtyp->load();
                    if( !proto->dtyp->cons.size() ) proto->elmt = VAR;
                    return imm::object(builder.CreateLoad(right->asobject(builder)),proto);
                }
                case VT::NOT: {
                    auto proto = right->eproto();
                    rv = builder.CreateNot(right->asobject(builder));
                    return imm::object(rv,proto);
                }
                case VT::INCRESS: {
                    builder.CreateStore(builder.CreateAdd(right->asobject(builder),builder.getInt32(1)), right->asaddress(builder) );
                    return right;
                }
                case VT::DECRESS: {
                    builder.CreateStore(builder.CreateSub(right->asobject(builder),builder.getInt32(1)), right->asaddress(builder) );
                    return right;
                }
            }
        }break;
        default : return nullptr;
    }
    return nullptr;
}

bool Sengine::performImplementationSemanticValidation( $ConstructImpl impl, llvm::IRBuilder<>& builder ) {
    if( flag_terminate ) return false;
    $imm inm = nullptr;
    Value* inv = nullptr;
    if( impl->init ) {
        inm = performImplementationSemanticValidation( impl->init, builder );
        inv = inm->asobject(builder);
    }

    if( impl->proto->dtyp->cate == UNKNOWN ) {
        if( inm ) impl->proto->dtyp = inm->eproto()->dtyp;
        else return false;
    }
    auto tp = generateTypeUsageAsAttribute(impl->proto);
    Value* addr = builder.CreateAlloca(tp, nullptr, (string)impl->name);
    if( inv ) builder.CreateStore(inv,addr);

    mlocalV[impl] = addr;
    return addr;
}

bool Sengine::performImplementationSemanticValidation( $BranchImpl impl, IRBuilder<>& builder ) {
    if( flag_terminate ) return false;
    auto bb2 = BasicBlock::Create(mctx,"",builder.GetInsertBlock()->getParent());
    auto bb3 = BasicBlock::Create(mctx,"",builder.GetInsertBlock()->getParent());
    auto bb4 = BasicBlock::Create(mctx,"",builder.GetInsertBlock()->getParent());
    auto bd = IRBuilder<>(mctx);
    auto cond = performImplementationSemanticValidation( impl->exp, builder );
    bool ret = false;
    builder.CreateCondBr(cond->asobject(builder),bb2,bb3);
    builder.SetInsertPoint(bb4);

    bd.SetInsertPoint(bb2);
    ret = performImplementationSemanticValidation( impl->first ,bd);
    if( ret == false ) return ret;
    if( flag_terminate ) flag_terminate = false;
    else bd.CreateBr(bb4);

    bd.SetInsertPoint(bb3);
    ret = performImplementationSemanticValidation( impl->secnd ,bd);
    if( ret == false ) return ret;
    if( flag_terminate ) flag_terminate = false;
    else bd.CreateBr(bb4);

    return ret;
}

bool Sengine::performImplementationSemanticValidation( $LoopImpl impl ,IRBuilder<>& builder) {
    if( flag_terminate ) return false;
    auto bb1 = BasicBlock::Create(mctx,"",builder.GetInsertBlock()->getParent());
    auto bb3 = BasicBlock::Create(mctx,"",builder.GetInsertBlock()->getParent());
    auto bd = IRBuilder<>(mctx);

    builder.CreateBr(bb1);
    builder.SetInsertPoint(bb3);

    bd.SetInsertPoint(bb1);
    if( impl->cond ){
        auto cond = performImplementationSemanticValidation( impl->cond, bd );
        auto bb2 = BasicBlock::Create(mctx,"",builder.GetInsertBlock()->getParent());
        bd.CreateCondBr(cond->asobject(builder),bb2,bb3);
        bd.SetInsertPoint(bb2);
    }

    bool fine = performImplementationSemanticValidation( impl->imp,bd );
    if( flag_terminate ) flag_terminate = false;
    else bd.CreateBr(bb1);
   
    return fine;
}

Type* Sengine::performDefinitionSemanticValidation( $AttrDef attr ) {
    /** 语法阶段好像对此有所检查,记不清了 if( attr->proto->elmt == VAL ) { ... } */
    return generateTypeUsageAsAttribute(attr->proto);
}

Type* Sengine::generateTypeUsageAsParameter( $eproto proto ) {
    if( !proto ) return nullptr;

    Type* ty = generateTypeUsage(proto->dtyp);
    if( !ty ) return nullptr;
    if( proto->elmt == VAL or proto->elmt == REF or (proto->elmt == VAR and proto->dtyp->cate == NAMED) ) {
        ty = ty->getPointerTo();
    }

    return ty;
}

Type* Sengine::generateTypeUsageAsAttribute( $eproto proto ) {
    if( !proto ) return nullptr;

    Type* ty = generateTypeUsage(proto->dtyp);
    if( !ty ) return nullptr;
    if( proto->elmt == VAL or proto->elmt == REF )
        ty = ty->getPointerTo();

    return ty;
}

Type* Sengine::generateTypeUsageAsReturnValue( $eproto proto ) {
    if( !proto ) return Type::getVoidTy(mctx);
    //[TODO]: 这里的算法待优化,尚不完善
    Type* ty = generateTypeUsage(proto->dtyp);
    if( !ty ) return nullptr;
    if( ty->isStructTy() ) return Type::getInt64Ty(mctx);
    else return ty;
}

Type* Sengine::generateTypeUsage( $dtype type ) {
    if( !type ) return nullptr;
    if( type->cate == cdtype::UNKNOWN ) return nullptr;
    
    Type* ltype = nullptr;
    if( type->cate == cdtype::BASIC ) {
        if( !type->basc.is(CT::BASIC_TYPE) ) return nullptr;
        switch( type->basc.id ) {
            default : return nullptr;
            case VT::BOOL                    :  ltype = Type::getInt1Ty(mctx);    break;
            case VT::INT8:   case VT::UINT8  :  ltype = Type::getInt8Ty(mctx);    break;
            case VT::INT16 : case VT::UINT16 :  ltype = Type::getInt16Ty(mctx);   break;
            case VT::INT32 : case VT::UINT32 :  ltype = Type::getInt32Ty(mctx);   break;
            case VT::INT64 : case VT::UINT64 :  ltype = Type::getInt64Ty(mctx);   break;
            case VT::FLOAT32                 :  ltype = Type::getFloatTy(mctx);   break;
            case VT::FLOAT64                 :  ltype = Type::getDoubleTy(mctx);  break;
        }
    } else {
        auto eve = request( type->name, NormalClass );
        if( eve.size() != 1 ) {
            mlogrepo(type->name.getScope()->getDocPath())(Lengine::E2004,type->name.phrase);
            return nullptr;
        }
        if( auto cdef = ($ClassDef)eve[0]; cdef ) {
            auto symbol = generateGlobalUniqueName(($node)cdef);
            ltype = mnamedT[symbol];
            if( !ltype ) ltype = mnamedT[symbol] = StructType::create(mctx,symbol);
        } else {
            mlogrepo(type->name.getScope()->getDocPath())(Lengine::E2004,type->name.phrase);
            return nullptr;
        }
    }

    if( ltype ) for( size_t i = 0; i < type->cons.size(); i++ ) ltype = ltype->getPointerTo();
    return ltype;
}

std::string Sengine::generateGlobalUniqueName( $node n, Decorate dec ) {
    string prefix;
    string suffix;
    string domain;

    if( auto list = dynamic_cast<morpheme::plist*>((node*)n); list ) for( auto pd : *list ) {
        suffix += ".";
        if( pd->proto->cons ) suffix += "C";
        switch( pd->proto->elmt ) {
            case VAR : suffix += "V";break;
            case PTR : suffix += "P";break;
            case REF : suffix += "R";break;
            case VAL : suffix += "L";break;
            case UDF : suffix += "X";break;
        }
        
        if( pd->proto->dtyp->cons.size() ) {
            int mask = 0;
            for( size_t i = 0; i < pd->proto->dtyp->cons.size(); i++ ) {
                if( pd->proto->dtyp->cons[i] ) mask |= 1 << i;
            }
            suffix += to_string(mask);
        }

        switch( pd->proto->dtyp->cate ) {
            case UNKNOWN: suffix += "U";break;
            case BASIC: switch( pd->proto->dtyp->basc.id ) {
                case VT::BOOL:      suffix += "b";break;
                case VT::INT8:      suffix += "i8";break;
                case VT::UINT8:     suffix += "u8";break;
                case VT::INT16 :    suffix += "i16";break;
                case VT::UINT16:    suffix += "u16";break;
                case VT::INT32 :    suffix += "i32";break;
                case VT::UINT32:    suffix += "u32";break;
                case VT::INT64 :    suffix += "i64";break;
                case VT::UINT64:    suffix += "u64";break;
                case VT::FLOAT32:   suffix += "f32";break;
                case VT::FLOAT64:   suffix += "f64";break;
                default : suffix += "bad";break;
            } break;
            case NAMED: {
                for( auto i = 0; i < pd->proto->dtyp->name.size(); i++ )
                    suffix += "." + (string)pd->proto->dtyp->name[i].name;
            }
        }
    }

    if( auto impl = ($MethodImpl)n; impl ) {
        for( int i = impl->funame.size()-1; i >= 0; i-- ) domain = "." + (string)impl->funame[i].name + domain;
    }
    for( auto def = ($definition)n; def; def = def->getScope() ) domain = "." + (string)def->name + domain;
    
    if( n->is(METHODDEF) ) prefix = "method";
    else if( n->is(METHODIMPL) ) prefix = "method";
    else if( n->is(DEFINITION) ) prefix = "class";

    switch( dec ) {
        case None:break;
        case Meta: suffix += ".meta"; break;
        case Entity: suffix += ".entity"; break;
    }

    return prefix + domain + suffix;
}

everything Sengine::request( const nameuc& name, Len len, $scope sc ) {

    if( !sc ) sc = name.getScope();
    if( !sc or name.size() == 0 ) return {};
    auto sname = (string)name[0].name;
    everything res;

    function<everything(const nameuc&,$scope)> lookupInternal = [&]( const nameuc& fn, $scope fsc ) -> everything {
        auto fsname = (string)fn[0].name;
        if( auto mdef = ($module)fsc; mdef ) {
            for( auto in : mdef->internal + mdef->external ) if( (string)in->name == sname ) {
                if( fn.size() == 1 ) res << (anything)in;
                else return lookupInternal( fn%1, in );
            }
        } else if( auto cdef = ($ClassDef)fsc; cdef ) {
            for( auto in : cdef->internal ) if( (string)in->name == sname ) {
                if( fn.size() == 1 ) res << (anything)in;
                else return lookupInternal( fn%1, in );
            }
        } else {
            return {};
        }
        return res;
    };
    
    if( auto bimpl = ($InsBlockImpl)sc; bimpl and name.size() == 1 ) {
    /** 在执行块中搜索已经被翻译的构建语句,若没有结果则上行传递搜索 */
        for( auto im : bimpl->impls ) if( auto cimpl = ($ConstructImpl)im; cimpl ) 
            if( (string)cimpl->name == sname and mlocalV.count(cimpl) ) res << (anything)cimpl;
        if( res.size() == 0 ) {
            if( auto scsc = sc->getScope(); !scsc ) return {};
            else return request( name, len, scsc );
        }
    } else if( auto mimpl = ($MethodImpl)sc; mimpl and name.size() == 1 ) {
    /** 在方法实现中搜索参数,若没有结果,加ThisClass滤镜上行传递搜索 */
        for( auto im : *mimpl ) if( (string)im->name == sname )
            res << (anything)im;
        if( res.size() == 0 ) {
            if( auto org = requestThisClass(($implementation)mimpl); !org ) return {};
            else return request( name, ThisClass, org );
        }
    } else if( auto mdef = ($module)sc; mdef ) {
    /**
     * 在模块层
     */
        if( sname == mdef->desc->name ) {
            if( name.size() == 1 ) res << (anything)mdef;
            else return lookupInternal( name%1, mdef );
        } else {
            if( name.size() == 1 ) {
                for( auto meta : mdef->metadefs + mdef->extmeta ) if( sname == (string)meta->name ) res << (anything)meta;
            }
            for( auto idef : mdef->internal + mdef->external ) if( (string)idef->name == sname ) {
                if( name.size() == 1 ) res << (anything)idef;
                else return lookupInternal( name%1, idef );
            }
            for( auto ddef : mdef->desc->deps ) if( (string)ddef->literal() == sname ) {
                if( name.size() == 1 ) res << (anything)mrepo[ddef->dest];
                else return lookupInternal( name%1, mrepo[ddef->dest] );
            }
        }
    } else if( auto cdef = ($ClassDef)sc; cdef ) {
    /**
     * ThisClass : 搜索全部能搜索的内容,实例定义,元定义,内部定义,基类, 若查无所获,上行到作用域继续搜索
     * SuperClass : 搜索实例定义,元定义,不论是否查无所获,都上行至基类继续搜索
     * NormalClass : 搜索内部定义,若查无所获,上行至作用域继续搜索
     */
        if( name.size() == 1 ) {
            if( len == ThisClass or len == SuperClass ) {
                for( auto idef : cdef->instdefs + cdef->metadefs ) if( (string)idef->name == sname ) res << (anything)idef;
                if( res.size() == 0 ) for( const auto& super : cdef->supers ) {
                    auto eve = request(super,NormalClass);
                    if( eve.size() != 1 ) continue;
                    if( auto sdef = ($ClassDef)eve[0]; sdef ) res += request(name,SuperClass,sdef);
                }
            }
            if( res.size() == 0 and len != SuperClass ) for( auto ndef : cdef->internal ) 
                if( (string)ndef->name == sname ) res << (anything)ndef;
        } else if( len != NormalClass and name.size() == 2 ) { /** 对于基类和当前类,两层名称可能指向基类的成员定义 */
            for( const auto& super : cdef->supers ) {
                auto eve = request(super,NormalClass);
                if( eve.size() != 1 ) continue;
                if( auto sdef = ($ClassDef)eve[0]; sdef ) {
                    if( (string)super[-1].name == sname ) {
                        auto sname2 = (string)name[1].name;
                        for( auto idef : sdef->instdefs + sdef->metadefs ) if( (string)idef->name == sname2 ) res << (anything)idef;
                    } else {
                        res += request( name, SuperClass, sdef );
                    }
                }
            }
        }
        if( len != SuperClass and res.size() == 0 ) { /** 对于普通类和当前类,若尚且查无所获,查询内部定义 */
                res = lookupInternal(name, sc);
        }
        if( res.size() == 0 and len != SuperClass ) {/** 若仍然查无所获,上行继续搜索 */
            if( auto scsc = sc->getScope(); !scsc ) return {};
            else return request( name, NormalClass, scsc );
        }
    } else {
    /**
     * 其他语法结构直接上行传递搜索
     */
        if( auto scsc = sc->getScope(); !scsc ) return {};
        else return request( name, len, scsc );
    }
    return res;
}

$ClassDef Sengine::requestThisClass( $implementation impl ) {
    while( impl and !impl->is(METHODIMPL) ) impl = impl->getScope();
    if( !impl ) return nullptr;
    auto method = ($MethodImpl)impl;
    if( mmethodP.count(method) ) return mmethodP[method]->getScope();

    auto eve = request(method->funame/1, NormalClass);
    if( eve.size() != 1 ) return nullptr;

    return ($ClassDef)eve[0];
}

$MethodDef Sengine::requestPrototype( $implementation impl ) {
    while( impl and !impl->is(METHODIMPL) ) impl = impl->getScope();
    if( !impl ) return nullptr;
    auto met = ($MethodImpl)impl;

    if( mmethodP.count(met) ) return mmethodP[met];
    
    auto scope = requestThisClass(($implementation)met);
    if( !scope ) return nullptr;
    auto sname = (string)met->funame[-1].name;

    for( auto def : scope->instdefs + scope->metadefs ) if( auto mdef = ($MethodDef)def; mdef and (string)mdef->name == sname ) {
        auto arg = met->begin();
        bool found = true;
        for( auto par : *mdef ) {
            found = checkEquivalent((*arg++)->proto, par->proto ) and found;
            if( !found ) break;
        }
        if( found ) return mmethodP[met] = mdef;
    }

    return nullptr;
}

Value* Sengine::requestThis( $implementation impl ) {
    while( impl and !impl->is(METHODIMPL) ) impl = impl->getScope();
    if( !impl ) return nullptr;

    auto fp = mcurmod->getFunction(generateGlobalUniqueName(($node)impl));
    if( !fp ) return nullptr;

    return fp->arg_begin();
}

bool Sengine::checkEquivalent( $eproto a, $eproto b ) {
    if( !a or !b ) return false;
    if( (bool)a->cons xor (bool)b->cons ) return false;
    if( a->elmt != b->elmt ) return false;
    return checkEquivalent( a->dtyp, b->dtyp );
}

bool Sengine::checkEquivalent( $dtype a, $dtype b ) {
    if( !a or !b ) return false;
    if( a->cate != b->cate ) return false;
    if( a->cons.size() != b->cons.size() ) return false;
    
    auto ib = b->cons.begin();
    for( auto i : a->cons ) if( i != *(ib++) ) return false;

    switch( a->cate ) {
        case UNKNOWN :
            return false;
        case BASIC:
            return a->basc.id == b->basc.id;
        case NAMED: {
            auto eve = request(a->name, NormalClass);
            if( eve.size() != 1 ) return false;
            auto ad = ($ClassDef)eve[0];
            if( !ad ) return false;
            eve = request(b->name, NormalClass);
            if( eve.size() != 1 ) return false;
            auto bd = ($ClassDef)eve[0];
            if( !bd ) return false;
            return ad == bd;
        }
    }
    return false;
}

Sengine::Sengine() {
    using namespace sys;
    TargetOptions opt;
    std::string Error;
    auto CPU = "generic";
    auto Features = "";

    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    //LLVMInitializeX86AsmParser();
    LLVMInitializeX86AsmPrinter();

    mttraiple = getDefaultTargetTriple();
    auto target = TargetRegistry::lookupTarget(mttraiple, Error);
    auto RM = Optional<Reloc::Model>();
    mtmachine = target->createTargetMachine( mttraiple, CPU, Features, opt, RM);
}

bool Sengine::loadModuleDefinition( $modesc mod ) {

    if( mrepo.count(mod) ) return true;
    auto& root = mrepo[mod] = new module;
    root->desc = mod;
    root->name = mod->name;
    $ClassDef tpclass = nullptr;
    mod->deps.clear();

    for( auto syn : mod->syntrees ) {

        syn->setScope(root);
        mod->deps += syn->signature->deps;
        for( auto d : syn->signature->deps ) {
            d->self = mod;
            mod->manager->closeUpDependency(d);
        }
        
        for( auto& d : syn->defs ) {
            if( auto trans = ($ClassDef)d; trans and (string)d->name == mod->name ) {
                if( tpclass ) {
                    mlogrepo(d->getDocPath())(Lengine::E2001,d->name,tpclass->getDocPath(),tpclass->name);
                    return false;
                } else {
                    if( trans->abstract ) {
                        mlogrepo(trans->getDocPath())(Lengine::E2027,trans->phrase);
                        return false;
                    } else if( trans->alias ) {
                        mlogrepo(trans->getDocPath())(Lengine::E2027,trans->phrase);
                        return false;
                    } else if( trans->tmpls.size() ) {
                        mlogrepo(trans->getDocPath())(Lengine::E2018,trans->phrase);
                        return false;
                    }
                    for( auto inst : trans->instdefs ) {
                        if( auto m = ($MethodDef)inst; m and !m->meta ) m->meta = token(VT::META);
                        else if( auto a = ($AttrDef)inst; a and !a->meta ) a->meta = token(VT::META);
                    }
                    root->internal += trans->internal;
                    trans->metadefs = root->metadefs += trans->metadefs + trans->instdefs;
                    trans->instdefs.clear();
                    tpclass = trans;
                } 
            } else {
                root->internal << d;
            }
        }

        root->impls += syn->impls;
    }

    return true;
}

bool Sengine::performDefinitionSemanticValidation() {

    bool fine = true;

    for( auto& [desc,mod] : mrepo ) {
        mcurmod = mtrepo[desc] = std::make_shared<Module>(desc->name,mctx);
        fine = performDefinitionSemanticValidation(desc) and fine;
    }

    return fine;
}

Sengine::ModuleTrnsUnit Sengine::performImplementationSemanticValidation( $modesc desc, Dengine& dengine ) {
    if( mrepo.count(desc) != 1 ) return nullptr;

    auto mod = mrepo[desc];
    mcurmod = mtrepo[desc];
    bool fine = true;

    string src;
    for( auto g : desc->syntrees ) {
        src += dengine.getPath(g->document) + "; ";
    }
    mcurmod->setSourceFileName(src);

    fine = performImplementationSemanticValidation( ($ClassDef)mod );
    for( auto def : mod->internal ) if( auto cdef = ($ClassDef)def; cdef ) {
        fine = performImplementationSemanticValidation(cdef) and fine;
    }
    for( auto im : mod->impls ) if( auto mim = ($MethodImpl)im; mim ) {
        fine = performImplementationSemanticValidation(mim) and fine;
    }

    if( !fine ) mcurmod = nullptr;
    return mcurmod;
}

bool Sengine::triggerBackendTranslation( ModuleTrnsUnit unit, Dengine::vfdm fd, Dengine& dengine ) {
    auto lfd = fd, wfd = fd, efd = fd;
    wfd.name += ".w";
    lfd.name += ".log";
    efd.name += ".ll";
    raw_fd_ostream ldest = raw_fd_ostream(dengine.getOfd(lfd),true);
    raw_fd_ostream dest = raw_fd_ostream(dengine.getOfd(fd),true);
    raw_fd_ostream wdest = raw_fd_ostream(dengine.getOfd(wfd),true);
    legacy::PassManager pass;

    unit->setTargetTriple(mttraiple);
    unit->setDataLayout(mtmachine->createDataLayout());
    auto efdi = dengine.getOfd(efd);
    dup2(efdi,2);
    unit->dump();
    close(efdi);
    mtmachine->addPassesToEmitFile(pass,dest,&wdest,TargetMachine::CGFT_ObjectFile);
    for( auto& fun : unit->getFunctionList() ) {
        string title = "\n--------------------- in function : ";
        title += fun.getName();
        title += "\n";
        ldest.write(title.data(),title.size());
        if( verifyFunction(fun,&ldest) ) return false;
    }
    if( verifyModule(*unit,&ldest) ) return false;
    pass.run(*unit);
    dest.flush();
    unit->print(ldest,nullptr);
    return true;

}

Lengine::logr Sengine::getLog() {
    return mlogrepo;
}

}

#endif