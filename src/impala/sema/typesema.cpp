#include <iostream>

#include "impala/ast.h"
#include "impala/dump.h"
#include "impala/sema/typetable.h"

namespace impala {

//------------------------------------------------------------------------------

class TypeSema : public TypeTable {
public:
    TypeSema(const bool nossa)
        : nossa_(nossa)
    {}

    bool nossa() const { return nossa_; }
    void push_impl(const Impl* i) { impls_.push_back(i); }
    void check_impls() {
        while (!impls_.empty()) {
            const Impl* i = impls_.back();
            impls_.pop_back();
            check(i);
        }
    }
    void expect_num(const Expr* exp);
    Type match_types(const Expr* pos, Type t1, Type t2);
    void expect_type(const Expr* found, Type expected, std::string typetype);
    Type create_return_type(const ASTNode* node, Type ret_func);
    Type check(const TypeDecl* type_decl) { 
        if (!type_decl->checked_) { 
            type_decl->checked_ = true; 
            type_decl->type_ = type_decl->check(*this); 
        }
        return type_decl->type();
    }
    void check(const Item* item) { 
        if (auto type_decl_item = item->isa<TypeDeclItem>())
            check(type_decl_item);
        else
            check(item->isa<MiscItem>());
    }
    Type check(const TypeDeclItem* type_decl_item) { return type_decl_item->type_ = type_decl_item->check(*this); }
    void check(const MiscItem* misc_item) { misc_item->check(*this); }
    Type check(const Expr* expr) { return expr->type_ = expr->check(*this); }
    Type check(const ASTType* ast_type) { return ast_type->type_ = ast_type->check(*this); }

private:
    bool nossa_;
    std::vector<const Impl*> impls_;
};

//------------------------------------------------------------------------------

void TypeSema::expect_num(const Expr* exp) {
    Type t = exp->type();

    if (t == type_error())
        return;

    if ((t != type_int()) && (t != type_int8()) && (t != type_int16()) && (t != type_int32()) &&
            (t != type_int64()) && (t != type_float()) && (t != type_double()))
        error(exp) << "expected number type but found " << t << "\n";
}

Type TypeSema::match_types(const Expr* pos, Type t1, Type t2) {
    if (t1 == type_error() || t2 == type_error())
        return type_error();

    if (t1 == t2) {
        return t1;
    } else {
        error(pos) << "types do not match: " << t1 << " != " << t2 << "\n";
        return type_error();
    }
}

void TypeSema::expect_type(const Expr* found, Type expected, std::string typetype) {
    if (found->type() == type_error() || expected == type_error())
        return;
    if (found->type() != expected)
        error(found) << "wrong " << typetype << " type; expected " << expected << " but found " << found->type() << "\n";
}

Type TypeSema::create_return_type(const ASTNode* node, Type ret_func) {
    if (ret_func.isa<FnType>()) {
        if (ret_func->size() == 1) {
            return ret_func->elem(0);
        } else {
            std::vector<Type> ret_types;
            for (auto t : ret_func->elems())
                ret_types.push_back(t);
            return tupletype(ret_types);
        }
    } else {
        error(node) << "last argument is not a continuation function\n";
        return type_error();
    }
}

//------------------------------------------------------------------------------

/*
 * ASTType::check
 */

void TypeParamList::check_type_params(TypeSema& sema) const {
    // check bounds
    for (const TypeParam* tp : type_params()) {
        for (const ASTType* b : tp->bounds()) {
            if (auto trait_inst = b->isa<ASTTypeApp>()) {
                tp->type_var(sema)->add_bound(trait_inst->to_trait(sema));
            } else {
                sema.error(tp) << "bounds must be trait instances, not types\n";
            }
        }
    }
}

TypeVar TypeParam::type_var(TypeSema& sema) const { return sema.check(this).as<TypeVar>(); }
Type ErrorASTType::check(TypeSema& sema) const { return sema.type_error(); }

Type PrimASTType::check(TypeSema& sema) const {
    switch (kind()) {
#define IMPALA_TYPE(itype, atype) case TYPE_##itype: return sema.primtype(PrimType_##itype);
#include "impala/tokenlist.h"
        default: THORIN_UNREACHABLE;
    }
}

Type PtrASTType::check(TypeSema& sema) const {
    return Type(); // FEATURE
}

Type IndefiniteArrayASTType::check(TypeSema& sema) const {
    return Type(); // FEATURE
}

Type DefiniteArrayASTType::check(TypeSema& sema) const {
    return Type(); // FEATURE
}

Type TupleASTType::check(TypeSema& sema) const {
    std::vector<Type> types;
    for (auto elem : elems())
        types.push_back(sema.check(elem));

    return sema.tupletype(types);
}

Type FnASTType::check(TypeSema& sema) const {
    check_type_params(sema);

    std::vector<Type> params;
    for (auto elem : elems())
        params.push_back(sema.check(elem));

    FnType fntype = sema.fntype(params);
    for (auto type_param : type_params())
        fntype->add_bound_var(type_param->type_var(sema));

    return fntype;
}

Type ASTTypeApp::check(TypeSema& sema) const {
    if (type_or_trait_decl()) {
        if (auto type_decl = type_or_trait_decl()->isa<TypeDecl>())
            return sema.check(type_decl);
        else
            sema.error(this) << '\'' << symbol() << "' does not name a type\n";
    }

    return sema.type_error();
}

Trait ASTTypeApp::to_trait(TypeSema& sema) const {
    if (type_or_trait_decl()) {
        if (auto trait_decl = type_or_trait_decl()->isa<TraitDecl>()) {
            Trait trait = trait_decl->to_trait(sema);
            if (elems().empty()) {
                return trait; // TODO design the API such that this check is not necessary
            } else {
                std::vector<Type> type_args;
                for (auto elem : elems())
                    type_args.push_back(sema.check(elem));
                return trait->instantiate(type_args);
            }
        } else
            sema.error(this) << '\'' << symbol() << "' does not name a trait\n";
    }
    return sema.trait_error();
}

//------------------------------------------------------------------------------

Type ValueDecl::calc_type(TypeSema& sema) const {
    if (!type())
        check(sema);
    return type();
}

// TraitDecl::to_trait

Trait TraitDecl::to_trait(TypeSema& sema) const {
    check(sema);
    return trait();
}

//------------------------------------------------------------------------------

/*
 * TypeDecl
 */

Type TypeParam::check(TypeSema& sema) const { return sema.typevar(); }

Type ModDecl::check(TypeSema& sema) const {
    if (mod_contents())
        mod_contents()->check(sema);
    return Type();
}

void ModContents::check(TypeSema& sema) const {
    for (auto item : items()) 
        sema.check(item);
}

Type ForeignMod::check(TypeSema& sema) const {
    return Type();
}

Type Typedef::check(TypeSema& sema) const {
    return Type();
}

Type EnumDecl::check(TypeSema& sema) const {
    return Type();
}

Type StructDecl::check(TypeSema& sema) const {
    return Type();
}

void FieldDecl::check(TypeSema&) const {
}

/*
 * MiscItem
 */

void FnDecl::check(TypeSema& sema) const {
    if (!type().empty())
        return;

    check_type_params(sema);
    std::vector<Type> types;
    for (const Param* p : fn().params()) { // TODO factor out
        Type pt = sema.check(p->ast_type());
        p->set_type(pt);
        types.push_back(pt);
    }
    // create FnType
    Type fn_type = sema.fntype(types);
    for (auto tp : type_params())
        fn_type->add_bound_var(tp->type_var(sema));

    sema.unify(fn_type); // TODO is this call necessary?
    set_type(fn_type);

    sema.check(fn().body());
    if (fn().body()->type() != sema.type_noreturn()) {
        Type ret_func = fn_type->elem(fn_type->size() - 1);
        sema.expect_type(fn().body(), sema.create_return_type(this, ret_func), "return");
    }
}

void StaticItem::check(TypeSema& sema) const {
}

void TraitDecl::check(TypeSema& sema) const {
    // did we already check this trait?
    if (!trait().empty())
        return;

    // FEATURE consider super traits and check methods
    trait_ = sema.trait(this);

    check_type_params(sema);
    for (auto tp : type_params()) {
        trait_->add_bound_var(tp->type_var(sema));
    }
}

void Impl::check(TypeSema& sema) const {
    check_type_params(sema);
    Type ftype = sema.check(for_type());

    if (trait() != nullptr) {
        if (auto t = trait()->isa<ASTTypeApp>()) {
            // create impl
            Trait tinst = t->to_trait(sema);
            TraitImpl impl = sema.implement_trait(this, tinst);
            for (auto tp : type_params()) {
                impl->add_bound_var(tp->type_var(sema));
            }

            // add impl to type
            if ((ftype != sema.type_error()) && (tinst != sema.trait_error()))
                ftype->add_implementation(impl);
        } else
            sema.error(trait()) << "expected trait instance.\n";
    }

    // FEATURE check that all methods are implemented
    for (auto fn : methods())
        sema.check(fn);
}

//------------------------------------------------------------------------------

/*
 * Expr::check
 */

Type EmptyExpr::check(TypeSema& sema) const { return sema.unit(); }

Type BlockExpr::check(TypeSema& sema) const {
    for (auto stmt : stmts())
        stmt->check(sema);

    sema.check(expr());
    return expr() ? expr()->type() : Type(sema.unit());
}

Type LiteralExpr::check(TypeSema& sema) const {
    return sema.primtype(literal2type());
}

Type FnExpr::check(TypeSema& sema) const {
    return Type();
}

Type PathExpr::check(TypeSema& sema) const {
    // FEATURE consider longer paths
    auto last_item = path()->path_items().back();
    if (value_decl()) { // TODO design the API such that this check is not necessary
        if (!last_item->args().empty()) {
            std::vector<Type> type_args;
            for (const ASTType* arg : last_item->args())
                type_args.push_back(sema.check(arg));

            return value_decl()->calc_type(sema)->instantiate(type_args);
        } else
            return value_decl()->calc_type(sema);
    } else
        return sema.type_error();
}

Type PrefixExpr::check(TypeSema& sema) const {
    return sema.check(rhs());
}

Type InfixExpr::check(TypeSema& sema) const {
    sema.check(lhs());
    sema.check(rhs());
    Type lhstype = lhs()->type();
    Type rhstype = rhs()->type();

    // FEATURE other cases
    switch (kind()) {
        case EQ:
        case NE:
            sema.match_types(this, lhstype, rhstype);
            return sema.type_bool();
        case ADD:
        case SUB:
        case MUL:
        case DIV:
        case REM:
            sema.expect_num(lhs());
            return sema.match_types(this, lhstype, rhstype);
        case ASGN:
            return sema.unit();
        default: THORIN_UNREACHABLE;
    }
}

Type PostfixExpr::check(TypeSema& sema) const {
    return sema.check(lhs());
}

Type FieldExpr::check(TypeSema& sema) const {
    return Type();
}

Type CastExpr::check(TypeSema& sema) const {
    return Type();
}

Type DefiniteArrayExpr::check(TypeSema& sema) const {
    return Type();
}

Type RepeatedDefiniteArrayExpr::check(TypeSema& sema) const {
    return Type();
}

Type IndefiniteArrayExpr::check(TypeSema& sema) const {
    return Type();
}

Type TupleExpr::check(TypeSema& sema) const {
    std::vector<Type> types;
    for (auto e : elems()) {
        sema.check(e);
        types.push_back(e->type());
    }
    return sema.tupletype(types);
}

Type StructExpr::check(TypeSema& sema) const {
    return Type();
}

Type MapExpr::check(TypeSema& sema) const {
    sema.check(lhs());
    if (auto fn = lhs()->type().isa<FnType>()) {
        bool no_cont = fn->size() == (args().size()+1); // true if this is a normal function call (no continuation)
        if (no_cont || (fn->size() == args().size())) {
            for (size_t i = 0; i < args().size(); ++i) {
                sema.check(arg(i));
                sema.expect_type(arg(i), fn->elem(i), "argument");
            }

            if (no_cont) // return type
                return sema.create_return_type(this, fn->elems().back()); 
            else        // same number of args as params -> continuation call
                return sema.type_noreturn();
        } else
            sema.error(this) << "wrong number of arguments\n";
    } else if (!lhs()->type().isa<TypeError>()) {
        // REMINDER new error message if not only fn-types are allowed
        sema.error(lhs()) << "expected function type but found " << lhs()->type() << "\n";
    }

    return sema.type_error();
}

Type IfExpr::check(TypeSema& sema) const {
    sema.check(cond());
    sema.expect_type(cond(), sema.type_bool(), "");
    sema.check(then_expr());
    sema.check(else_expr());

    // assert that both branches have the same type and set the type
    return sema.match_types(this, then_expr()->type(), else_expr()->type());
}

Type ForExpr::check(TypeSema& sema) const {
    return sema.unit();
}

//------------------------------------------------------------------------------

/*
 * Stmt::check
 */

void ExprStmt::check(TypeSema& sema) const {
    sema.check(expr());
}

void ItemStmt::check(TypeSema& sema) const {
    sema.check(item());
}

void LetStmt::check(TypeSema& sema) const {
    if (init())
        sema.check(init());
}

//------------------------------------------------------------------------------

void ValueDecl::check(TypeSema& sema) const { 
    set_type(sema.check(ast_type()));
}

//------------------------------------------------------------------------------

bool type_analysis(const ModContents* mod, bool nossa) {
    TypeSema sema(nossa);
    mod->check(sema);
#ifndef NDEBUG
    sema.verify();
#endif
    return sema.result();
}

}