#include "impala/ast.h"

namespace impala {

const Param* Param::create(size_t var_handle, Symbol symbol, const Location& loc, const ASTType* fn_type) {
    auto param = new Param(var_handle);
    param->is_mut_ = false;
    param->symbol_ = symbol;
    param->ast_type_ = fn_type;
    param->set_loc(loc);
    return param;
}

const PrefixExpr* PrefixExpr::create_deref(const AutoPtr<const Expr>& dock) {
    auto deref = new PrefixExpr();
    deref->set_loc(dock->loc());
    deref->kind_ = PrefixExpr::MUL;
    deref->rhs_ = deref;
    swap(deref->rhs_, const_cast<AutoPtr<const Expr>&>(dock));
    return deref;
}

const char* Visibility::str() {
    if (visibility_ == Pub)  return "pub ";
    if (visibility_ == Priv) return "priv ";
    return "";
}

const FnASTType* FnASTType::ret_fn_type() const {
    if (!elems().empty()) {
        if (auto fn_type = elems().back()->isa<FnASTType>())
            return fn_type;
    }
    return nullptr;
}

PrimTypeKind LiteralExpr::literal2type() const {
    switch (kind()) {
#define IMPALA_LIT(itype, atype) \
        case LIT_##itype: return PrimType_##itype;
#include "impala/tokenlist.h"
        case LIT_bool:    return PrimType_bool;
        default: THORIN_UNREACHABLE;
    }
}

uint64_t LiteralExpr::get_u64() const { return thorin::bcast<uint64_t, thorin::Box>(box()); }

bool IfExpr::has_else() const {
    if (auto block = else_expr_->isa<BlockExpr>())
        return !block->empty();
    return true;
}

//------------------------------------------------------------------------------

/*
 * is_lvalue
 */

bool PathExpr::is_lvalue() const { 
    if (value_decl()) {
        value_decl()->is_written_ = true;
        return value_decl()->is_mut();
    }
    return false;
}

bool MapExpr::is_lvalue() const {
    return (lhs()->type().isa<ArrayType>() || lhs()->type().isa<TupleType>()) ?  lhs()->is_lvalue() : false;
}

bool PrefixExpr::is_lvalue() const {
    return kind() == MUL && rhs()->is_lvalue();
}

bool FieldExpr::is_lvalue() const {
    return lhs()->is_lvalue();
}

//------------------------------------------------------------------------------

}
