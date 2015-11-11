#include <sstream>

#include "thorin/util/push.h"

#include "impala/ast.h"
#include "impala/dump.h"
#include "impala/impala.h"
#include "impala/sema/typetable.h"

using namespace thorin;

namespace impala {

//------------------------------------------------------------------------------

class TypeSema {
public:
    TypeSema(bool nossa)
        : nossa_(nossa)
    {}

    bool nossa() const { return nossa_; }
    thorin::u8 char_value(const Location& loc, const char*& p);

    // error handling

    bool expect_lvalue(const Expr* expr, const char* context = nullptr) {
        if (!expr->is_lvalue()) {
            error(expr) << "lvalue required " << (context ? context : "in assignment") << '\n';
            return  false;
        }
        return true;
    }
    bool expect_int(const Expr*);
    bool expect_int_or_bool(const Expr*);
    void expect_num(const Expr*);
    Type expect_type(const Expr* expr, Type found, Type expected, const char* context = nullptr);
    Type expect_type(const Expr* expr, Type expected, const char* context = nullptr) { return expect_type(expr, expr->type(), expected, context); }

    // helpers

#if 0
    Type comparison_result(const Expr* expr) {
        if (auto simd = expr->type().isa<SimdType>())
            return simd_type(type_bool(), simd->size());
        return type_bool();
    }

    int take_addr_space(const PrefixExpr* prefix) {
        if (prefix->kind() == PrefixExpr::MUL) {
            auto type = check(prefix->rhs());
            if (auto ptr = type.isa<PtrType>()) {
                return ptr->addr_space();
            }
        }
        return 0;
    }
#endif

    // check wrappers

    Type check_call(const MapExpr* expr, FnType fn_poly, const ASTTypes& type_args, std::vector<Type>& inferred_args, ArrayRef<const Expr*> args, Type expected);
    bool check_bounds(const Location& loc, Uni unifiable, ArrayRef<Type> types);

    static Type turn_cast_inside_out(const Expr* expr) {
        assert(expr->needs_cast());
        expr->type_.clear();
        expr->type_ = expr->actual_type_;
        expr->actual_type_.clear();
        return expr->type();
    }

private:
    bool nossa_;

public:
    const BlockExprBase* cur_block_ = nullptr;
    const Fn* cur_fn_ = nullptr;
};

void type_analysis(const ModContents* mod, bool nossa) {
    TypeSema sema(nossa);
    mod->check(sema);
}

//------------------------------------------------------------------------------

// TODO factor code with expect_num
// TODO maybe have variant which also checks expr
bool TypeSema::expect_int(const Expr* expr) {
    Type e = expr->type(), t;
    if (auto simd = e.isa<SimdType>()) {
        t = simd->elem_type();
    } else {
        t = e;
    }

    if (!t->is_error() &&
        !t->is_i8() && !t->is_i16() && !t->is_i32() && !t->is_i64() &&
        !t->is_u8() && !t->is_u16() && !t->is_u32() && !t->is_u64()) { // TODO factor this test out
        error(expr) << "expected integer type but found " << t << "\n";
        return false;
    }
    return true;
}

bool TypeSema::expect_int_or_bool(const Expr* expr) {
    Type e = expr->type(), t;
    if (auto simd = e.isa<SimdType>()) {
        t = simd->elem_type();
    } else {
        t = e;
    }

    if (!t->is_error() &&
        !t->is_bool() &&
        !t->is_i8() && !t->is_i16() && !t->is_i32() && !t->is_i64() &&
        !t->is_u8() && !t->is_u16() && !t->is_u32() && !t->is_u64()) { // TODO factor this test out
        error(expr) << "expected integer or boolean type but found " << t << "\n";
        return false;
    }
    return true;
}

void TypeSema::expect_num(const Expr* expr) {
    Type e = expr->type(), t;
    if (auto simd = e.isa<SimdType>()) {
        t = simd->elem_type();
    } else {
        t = e;
    }

    if (!t->is_error() &&
        !t->is_i8() && !t->is_i16() && !t->is_i32() && !t->is_i64() &&
        !t->is_u8() && !t->is_u16() && !t->is_u32() && !t->is_u64() &&
        !t->is_f32() && !t->is_f64()) // TODO factor this test out
        error(expr) << "expected number type but found " << t << "\n";
}

Type TypeSema::expect_type(const Expr* expr, Type found_type, Type expected, const char* context) {
    if (found_type == expected)
        return found_type;
    if (found_type <= expected) {
        expr->actual_type_ = found_type;
        return expected;
    }

    // TODO noret
    //if (expected.noret() && (found_type == type_noret()))
        //return found_type;

    if (found_type->is_polymorphic()) { // try to infer instantiations for this polymorphic type
        std::vector<Type> type_args;
        Type inst = instantiate_unknown(found_type, type_args);
        if (inst == expected) {
            check_bounds(expr->loc(), *found_type, type_args);
            return expected;
        }
    }

    error(expr->loc()) << "mismatched types: expected '" << expected << "' but found '" << found_type << (context ? std::string("' as ") + context : "'" ) << "\n";
    return expected;
}

bool TypeSema::check_bounds(const Location& loc, Uni unifiable, ArrayRef<Type> type_args) {
    SpecializeMap map = specialize_map(unifiable, type_args);
    assert(map.size() == type_args.size());
    bool result = true;

    for (size_t i = 0, e = type_args.size(); i != e; ++i) {
        auto type_var = unifiable->type_var(i);
        Type arg = type_args[i];
        assert(map.contains(*type_var));
        assert(map.find(*type_var)->second == *arg);

        for (auto bound : type_var->bounds()) {
            // TODO do we need this copy?
            SpecializeMap bound_map(map); // copy the map per type var
            auto spec_bound = bound->specialize(bound_map);
            if (!arg->is_error() && !spec_bound->is_error()) {

                // TODO
                //check_impls(); // first we need to check all implementations to be up-to-date
                if (!arg->implements(spec_bound, bound_map)) {
                    error(loc) << "'" << arg << "' (instance for '" << type_var << "') does not implement bound '" << spec_bound << "'\n";
                    result = false;
                }
            }
        }
    }

    return result;
}

//------------------------------------------------------------------------------

/*
 * AST types
 */

#if 0
void TypeParamList::check_type_params(TypeSema& sema) const {
    for (auto type_param : type_params()) {
        auto type_var = type_param->check(sema);
        for (auto bound : type_param->bounds()) {
            if (auto type_app = bound->isa<ASTTypeApp>()) {
                type_var->add_bound(type_app->trait_app(sema, type_var));
            } else {
                error(type_param) << "bounds must be trait instances, not types\n";
            }
        }
    }
}
#endif

TypeVar TypeParam::check(TypeSema&) const { return type_var(); }
void ErrorASTType::check(TypeSema& ) const {}
void PrimASTType::check(TypeSema&) const {}
void PtrASTType::check(TypeSema& sema) const { referenced_type()->check(sema); }
void IndefiniteArrayASTType::check(TypeSema& sema) const { return elem_type()->check(sema); }
void   DefiniteArrayASTType::check(TypeSema& sema) const { return elem_type()->check(sema); }

void TupleASTType::check(TypeSema& sema) const {
    for (auto arg : args())
        arg->check(sema);
}

void FnASTType::check(TypeSema& sema) const {
    check_type_params(sema);
    for (auto arg : args())
        arg->check(sema);
}

void ASTTypeApp::check(TypeSema&) const {
    if (decl()) {
        if (decl()->isa<TypeDecl>())
            return; // OK
    }

    error(identifier()) << '\'' << symbol() << "' does not name a type\n";
}

void Typeof::check(TypeSema& sema) const { expr()->check(sema); }

#if 0
TraitApp ASTTypeApp::trait_app(TypeSema& sema, Type self) const {
    if (decl()) {
        if (!decl()->isa<TraitDecl>())
            error(this) << '\'' << symbol() << "' does not name a trait\n";
    }
}
#endif

void SimdASTType::check(TypeSema& sema) const {
    elem_type()->check(sema);
    if (!type().isa<PrimType>())
        error(this) << "non primitive types forbidden in simd type\n";
}

//------------------------------------------------------------------------------

Type LocalDecl::check(TypeSema& sema) const {
    ast_type()->check(sema);
    return type();
}

void Fn::check_body(TypeSema& sema, FnType fn_type) const {
    auto return_type = fn_type->return_type();
    auto body_type = body()->check(sema);
    if (body_type != return_type)
        error(body()) << "TODO\n";

    for (auto param : params()) {
        if (param->is_mut() && !param->is_written())
            warn(param) << "parameter '" << param->symbol() << "' declared mutable but parameter is never written to\n";
    }
}

//------------------------------------------------------------------------------

/*
 * items
 */

void ModDecl::check(TypeSema& sema) const {
    if (mod_contents())
        mod_contents()->check(sema);
}

void ModContents::check(TypeSema& sema) const {
    for (auto item : items())
        item->check(sema);
}

void ExternBlock::check(TypeSema& sema) const {
    if (!abi().empty()) {
        if (abi() != "\"C\"" && abi() != "\"device\"" && abi() != "\"thorin\"")
            error(this) << "unknown extern specification\n";  // TODO: better location
    }

    for (auto fn : fns())
        fn->check(sema);
}

void Typedef::check(TypeSema& sema) const {
    check_type_params(sema);
    ast_type()->check(sema);
}

void EnumDecl::check(TypeSema&) const { /*TODO*/ }

void StructDecl::check(TypeSema& sema) const {
    check_type_params(sema);
    auto struct_type = type_.empty() ? sema.struct_abs_type(this) : type().as<StructAbsType>();

    for (auto field : field_decls()) {
        if (auto field_type = field->type())
            struct_type->set(field->index(), field_type);
    }

    for (auto type_param : type_params())
        struct_type->bind(type_param->check(sema));
}

Type FieldDecl::check(TypeSema& sema) const {
    return sema.check(ast_type());
}

void FnDecl::check(TypeSema& sema) const {
    THORIN_PUSH(sema.cur_fn_, this);

    check_type_params(sema);
    std::vector<Type> types; // TODO use thorin::Array
    for (auto param : params())
        types.push_back(param->check(sema, sema.unknown_type() /*TODO: infer*/));

    auto fn_type = sema.fn_type(types);
    for (auto type_param : type_params())
        fn_type->bind(type_param->check(sema));
    type_ = fn_type;

    if (body() != nullptr)
        check_body(sema, fn_type);
}

void StaticItem::check(TypeSema& sema) const {
    if (init())
        type_ = sema.check(init());
}

void TraitDecl::check(TypeSema& sema) const {
    TypeVar self_var = self_param()->check(sema);
    trait_abs_ = sema.trait_abs(this);
    trait_abs_->bind(self_var);

    check_type_params(sema);
    for (auto type_param : type_params())
        trait_abs_->bind(type_param->check(sema));

    for (auto type_app : super_traits()) {
        if (!trait_abs_->add_super_trait(type_app->trait_app(sema, self_var)))
            error(type_app) << "duplicate super trait '" << type_app << "' for trait '" << symbol() << "'\n";
    }

    for (auto method : methods())
        method->check(sema);
}

void ImplItem::check(TypeSema& sema) const {
    check_type_params(sema);
    Type for_type = sema.check(this->ast_type());

    TraitApp trait_app;
    if (trait() != nullptr) {
        if (auto type_app = trait()->isa<ASTTypeApp>()) {
            trait_app = type_app->trait_app(sema, for_type);
            auto impl = sema.impl(this, trait_app, for_type);
            for (auto type_param : type_params())
                impl->bind(type_param->check(sema));

            if (!for_type->is_error() && !trait_app->is_error()) {
                for_type.as<KnownType>()->add_impl(impl);
                trait_app->trait()->add_impl(impl);
            }
        } else
            error(trait()) << "expected trait instance\n";
    }

    thorin::HashSet<Symbol> implemented_methods;
    for (auto method : methods()) {
        method->check(sema);
        Type fn_type = method->type();

        if (trait() != nullptr) {
            assert(!trait_app.empty());

            Symbol meth_name = method->symbol();
            if (auto method_type = trait_app->find_method(meth_name)) {
                // remember name for check if all methods were implemented
                const auto& p = implemented_methods.insert(meth_name);
                assert(p.second && "there should be no such name in the set"); // else name analysis failed

                // check that the types match
                if (fn_type != method_type)
                    error(method) << "method '" << trait() << "." << meth_name << "' should have type '" << method_type << "', but implementation has type '" << fn_type << "'\n";
            }
        }
    }

    // TODO
#if 0
    // check that all methods are implemented
    if (!bound.empty()) {
        if (implemented_methods.size() != bound->num_methods()) {
            assert(implemented_methods.size() < bound->num_methods());
            for (const auto& p : bound->all_methods()) {
                if (!implemented_methods.contains(p.first))
                    error(this) << "must implement method '" << p.first << "'\n";
            }
        }
    }
#endif
}

//------------------------------------------------------------------------------

/*
 * expressions
 */

Type EmptyExpr::check(TypeSema& sema, Type) const { return sema.unit(); }

Type SizeofExpr::check(TypeSema& sema, Type) const {
    sema.check(ast_type());
    return sema.type_u32();
}

Type LiteralExpr::check(TypeSema& sema, Type) const {
    // FEATURE we could enhance this using the expected type (e.g. 4 could be interpreted as int8 if needed)
    return sema.type(literal2type());
}

thorin::u8 TypeSema::char_value(const Location& loc, const char*& p) {
    thorin::u8 value = 0;
    if (*p++ == '\\') {
        switch (*p++) {
            case '0':  value = '\0'; break;
            case 'n':  value = '\n'; break;
            case 't':  value = '\t'; break;
            case '\'': value = '\''; break;
            case '\"': value = '\"'; break;
            case '\\': value = '\\'; break;
            default:
                error(loc) << "unknown escape sequence '\\" << *(p-1) << "'\n";
        }
    } else
        value = thorin::u8(*(p-1));

    return value;
}

Type CharExpr::check(TypeSema& sema, Type) const {
    const char* p = symbol().str();
    assert(*p == '\'');
    ++p;
    if (*p != '\'') {
        value_ = sema.char_value(loc(), p);

        if (*p++ != '\'')
            error(this) << "multi-character character constant\n";
        else
            assert(*p == '\0');
    } else
        error(this) << "empty character constant\n";

    return sema.type_u8();
}

Type StrExpr::check(TypeSema& sema, Type expected) const {
    for (auto symbol : symbols()) {
        const char* p = symbol.str();
        assert(*p == '"');
        ++p;
        while (*p != '"')
            values_.push_back(sema.char_value(loc(), p));
        assert(p[1] == '\0');
    }
    values_.push_back('\0');

    auto result = sema.definite_array_type(sema.type_u8(), values_.size());
    if (auto ptr = expected.isa<BorrowedPtrType>()) {
        if (auto array = ptr->referenced_type().isa<ArrayType>()) {
            if (array->elem_type()->is_u8()) {
                is_used_as_global_ = true;
                return sema.borrowd_ptr_type(result);
            }
        }
    }
    return result;
}

Type FnExpr::check(TypeSema& sema, Type expected) const {
    THORIN_PUSH(sema.cur_fn_, this);
    assert(type_params().empty());

    FnType fn_type;
    if (FnType exp_fn = expected.isa<FnType>()) {
        if (!is_continuation() && exp_fn->num_args() == num_params()+1) { // add return param to infer type
            const Location& loc = body()->pos1();
            const_cast<FnExpr*>(this)->params_.push_back(Param::create(ret_var_handle_, new Identifier("return", body()->pos1()), loc, nullptr));
        } else if (exp_fn->num_args() != num_params())
            error(this) << "expected function with " << exp_fn->num_args() << " parameters, but found lambda expression with " << num_params() << " parameters\n";

        for (size_t i = 0; i < num_params() && i < exp_fn->num_args(); ++i)
            param(i)->check(sema, exp_fn->arg(i));

        fn_type = exp_fn;
    } else {
        std::vector<Type> param_types; // TODO use thorin::Array
        for (auto param : params())
            param_types.push_back(param->check(sema, sema.unknown_type()));

        fn_type = sema.fn_type(param_types);
    }

    assert(body() != nullptr);
    check_body(sema, fn_type);

    return fn_type;
}

Type PathExpr::check(TypeSema& sema, Type) const {
    // FEATURE consider longer paths
    //auto* last = path()->path_args().back();
    if (value_decl()) {
        if (auto local = value_decl()->isa<LocalDecl>()) {
            // if local lies in an outer function go through memory to implement closure
            if (local->is_mut() && (sema.nossa() || local->fn() != sema.cur_fn_))
                local->take_address();
        }
        return value_decl()->type();
    }
    return sema.type_error();
}

Type PrefixExpr::check(TypeSema& sema, Type expected) const {
    switch (kind()) {
        case AND: {
            Type rtype;
            if (auto ptr = expected.isa<PtrType>()) {
                rtype = sema.check(rhs(), ptr->referenced_type());
            } else
                rtype = sema.check(rhs());
            sema.expect_lvalue(rhs(), "as unary '&' operand");
            rhs()->take_address();
            if (rhs()->needs_cast()) {
                rtype.clear();
                rtype = TypeSema::turn_cast_inside_out(rhs());
            }

            // Keep the address space of the original pointer, if possible
            int addr_space = 0;
            if (auto map = rhs()->isa<MapExpr>()) {
                if (auto prefix = map->lhs()->isa<PrefixExpr>())
                    addr_space = sema.take_addr_space(prefix);
            } else if (auto field = rhs()->isa<FieldExpr>()) {
                if (auto prefix = field->lhs()->isa<PrefixExpr>())
                    addr_space = sema.take_addr_space(prefix);
            } else if (auto prefix = rhs()->isa<PrefixExpr>()) {
                addr_space = sema.take_addr_space(prefix);
            }

            return sema.borrowd_ptr_type(rtype, addr_space);
        }
        case TILDE:
            if (auto pty = expected.isa<PtrType>()) {
                return sema.owned_ptr_type(sema.check(rhs(), pty->referenced_type()));
            } else {
                return sema.owned_ptr_type(sema.check(rhs()));
            }
        case MUL: {
            auto type = sema.check(rhs());
            // 'type' must be a pointer type (with any address space)
            // and must reference the expected type.
            if (auto ptr = type.isa<PtrType>()) {
                sema.expect_type(rhs(), ptr->referenced_type(), expected);
                return ptr->referenced_type();
            } else {
                auto ptr_type = sema.borrowd_ptr_type(expected);
                sema.expect_type(rhs(), type, Type(ptr_type));
                return sema.type_error();
            }
        }
        case INC:
        case DEC: {
            auto rtype = sema.check(rhs(), expected);
            sema.expect_num(rhs());
            sema.expect_lvalue(rhs());
            return rtype;
        }
        case ADD:
        case SUB: {
            auto rtype = sema.check(rhs(), expected);
            sema.expect_num(rhs());
            return rtype;
        }
        case NOT: {
            auto rtype = sema.check(rhs(), expected);
            if (auto simd = rtype.isa<SimdType>()) {
                if (simd->elem_type()->is_bool())
                    return rtype;
            }
            if (rtype->is_bool() || sema.expect_int(rhs()))
                return rtype;
            return sema.type_error();
        }
        case RUN:
        case HLT:
            return sema.check(rhs()); // TODO can we propagate expected here?
        default:
            THORIN_UNREACHABLE;
    }

    return sema.type_error();
}

Type InfixExpr::check(TypeSema& sema, Type expected) const {
    switch (kind()) {
        case EQ:
        case NE:
            sema.check(rhs(), sema.check(lhs()));
            if (!lhs()->type().isa<PtrType>() && !lhs()->type().isa<PrimType>() && !lhs()->type().isa<SimdType>()) {
                error(this) << "expected primitive type, pointer type or SIMD type for equality operator\n";
                return sema.type_error();
            }
            return sema.comparison_result(lhs());
        case LT:
        case LE:
        case GT:
        case GE:
            sema.check(rhs(), sema.check(lhs()));
            sema.expect_num(lhs());
            sema.expect_num(rhs());
            return sema.comparison_result(lhs());
        case OROR:
        case ANDAND:
            sema.check(lhs(), sema.type_bool(), "left-hand side of logical boolean expression");
            sema.check(rhs(), sema.type_bool(), "right-hand side of logical boolean expression");
            return sema.type_bool();
        case ADD:
        case SUB:
        case MUL:
        case DIV:
        case REM: {
            auto type = sema.check(lhs(), sema.check(rhs(), expected));
            sema.expect_num(lhs());
            sema.expect_num(rhs());
            return type;
        }
        case SHL:
        case SHR: {
            auto type = sema.check(lhs(), sema.check(rhs(), expected));
            sema.expect_int(lhs());
            sema.expect_int(rhs());
            return type;
        }
        case OR:
        case XOR:
        case AND: {
            auto type = sema.check(lhs(), sema.check(rhs(), expected));
            sema.expect_int_or_bool(lhs());
            sema.expect_int_or_bool(rhs());
            return type;
        }
        case ASGN:
            sema.check(rhs(), sema.check(lhs()));
            if (sema.expect_lvalue(lhs()))
                return sema.unit();
            break;
        case ADD_ASGN:
        case SUB_ASGN:
        case MUL_ASGN:
        case DIV_ASGN:
        case REM_ASGN: {
            sema.check(rhs(), sema.check(lhs()));
            if (sema.expect_lvalue(lhs())) {
                sema.expect_num(lhs());
                sema.expect_num(rhs());
                return sema.unit();
            }
            break;
        }
        case AND_ASGN:
        case  OR_ASGN:
        case XOR_ASGN:
        case SHL_ASGN:
        case SHR_ASGN:  {
            // TODO handle floats etc
            sema.check(rhs(), sema.check(lhs()));
            if (sema.expect_lvalue(lhs())) {
                sema.expect_int_or_bool(lhs());
                sema.expect_int_or_bool(rhs());
                return sema.unit();
            }
            break;
        }
        default: THORIN_UNREACHABLE;
    }

    return sema.type_error();
}

Type PostfixExpr::check(TypeSema& sema, Type expected) const {
    // TODO check if operator supports the type
    sema.check(lhs(), expected);
    sema.expect_lvalue(lhs());
    return lhs()->type();
}

Type CastExpr::check(TypeSema& sema, Type) const {
    // TODO check whether cast is possible at all
    sema.check(lhs());
    return sema.check(ast_type());
}

Type DefiniteArrayExpr::check(TypeSema& sema, Type) const {
    Type elem_type = sema.unknown_type();
    for (auto arg : args())
        sema.check(arg, elem_type, "element of definite array expression");
    return sema.definite_array_type(elem_type, num_args());
}

Type RepeatedDefiniteArrayExpr::check(TypeSema& sema, Type) const {
    return sema.definite_array_type(sema.check(value()), count());
}

Type IndefiniteArrayExpr::check(TypeSema& sema, Type) const {
    sema.check(dim());
    sema.expect_int(dim());
    return sema.indefinite_array_type(sema.check(elem_type()));
}

Type TupleExpr::check(TypeSema& sema, Type expected) const {
    std::vector<Type> types;
    if (auto exp_tup = expected.isa<TupleType>()) {
        if (exp_tup->num_args() != num_args())
            error(this) << "expected tuple with " << exp_tup->num_args() << " elements, but found tuple expression with " << num_args() << " elements\n";

        size_t i = 0;
        for (auto arg : args()) {
            sema.check(arg, exp_tup->arg(i++));
            types.push_back(arg->type());
        }
    } else {
        for (auto arg : args()) {
            sema.check(arg);
            types.push_back(arg->type());
        }
    }
    return sema.tuple_type(types);
}

Type SimdExpr::check(TypeSema& sema, Type) const {
    Type elem_type = sema.unknown_type();
    for (auto arg : args())
        sema.check(arg, elem_type, "element of simd expression");
    return sema.simd_type(elem_type, num_args());
}

Type StructExpr::check(TypeSema& sema, Type expected) const {
    if (auto decl = path()->decl()) {
        StructAppType struct_app;

        if (auto typeable_decl = decl->isa<TypeableDecl>()) {
            if (auto decl_type = typeable_decl->type()) {
                if (num_type_args() <= decl_type->num_type_vars()) {
                    StructAppType exp_type = expected.isa<StructAppType>();

                    // use the expected type if there is any
                    if (exp_type && (decl_type == exp_type->struct_abs_type())) {
                        for (size_t i = 0; i < exp_type->num_args(); ++i) {
                            if ((i < num_type_args()) && (exp_type->arg(i) != sema.check(type_arg(i))))
                                error(type_arg(i)) << "expected different argument for type parameter '" << decl_type->type_var(i) << "': expected '" << exp_type->arg(i) << "' but found '" << type_arg(i)->type() << "'\n";
                            inferred_args_.push_back(exp_type->arg(i));
                        }

                        assert(inferred_args_.size() == decl_type->num_type_vars());
                        struct_app = exp_type;
                    } else { // if no expected type was given fill type arguments with unknowns
                        for (auto type_arg : type_args())
                            inferred_args_.push_back(sema.check(type_arg));

                        for (size_t i = num_type_args(), e = decl_type->num_type_vars(); i != e; ++i)
                            inferred_args_.push_back(sema.unknown_type());

                        assert(inferred_args_.size() == decl_type->num_type_vars());
                        auto instantiated_decl_type = decl_type->instantiate(inferred_args_);

                        if (instantiated_decl_type.isa<StructAppType>())
                            struct_app = instantiated_decl_type.as<StructAppType>();
                        else
                            error(path()) << '\'' << decl->symbol() << '\'' << " does not name a structure\n";
                    }
                } else
                    error(this) << "too many type arguments to structure: " << num_type_args() << " for " << decl_type->num_type_vars() << "\n";

                if (struct_app) {
                    auto struct_abs  = struct_app->struct_abs_type();
                    auto struct_decl = struct_abs->struct_decl();
                    thorin::HashSet<const FieldDecl*> done;
                    for (const auto& elem : elems()) {
                        if (auto field_decl = struct_decl->field_decl(elem.symbol())) {
                            elem.field_decl_ = field_decl;
                            if (!thorin::visit(done, field_decl)) {
                                std::ostringstream oss;
                                oss << "initialization type for field '" << elem.symbol() << '\'';
                                sema.check(elem.expr(), struct_app->elem(field_decl->index()), oss.str().c_str());
                            } else
                                error(elem.expr()) << "field '" << elem.symbol() << "' specified more than once\n";
                        } else
                            error(elem.expr()) << "structure '" << struct_decl->symbol() << "' has no field named '" << elem.symbol() << "'\n";
                    }

                    if (done.size() != struct_decl->field_table().size()) {
                        for (const auto& p : struct_decl->field_table()) {
                            if (!done.contains(p.second))
                                error(this) << "missing field '" << p.first << "'\n";
                        }
                    }

                    return struct_app;
                }
            } else
                return sema.unknown_type();
        } else
            error(path()) << '\'' << decl->symbol() << '\'' << " does not name a structure\n";
    }
    return sema.type_error();
}

Type TypeSema::check_call(const MapExpr* expr, FnType fn_poly, const ASTTypes& type_args, std::vector<Type>& inferred_args, ArrayRef<const Expr*> args, Type expected) {
    size_t num_type_args = type_args.size();
    size_t num_args = args.size();

    if (num_type_args <= fn_poly->num_type_vars()) {
        for (auto type_arg : type_args)
            inferred_args.push_back(check(type_arg));

        for (size_t i = num_type_args, e = fn_poly->num_type_vars(); i != e; ++i)
            inferred_args.push_back(unknown_type());

        assert(inferred_args.size() == fn_poly->num_type_vars());
        auto fn_mono = fn_poly->instantiate(inferred_args).as<FnType>();

        bool is_contuation = num_args == fn_mono->num_args();
        if (is_contuation || num_args+1 == fn_mono->num_args()) {
            for (size_t i = 0; i != num_args; ++i)
                check(args[i], fn_mono->arg(i), "argument type");

            // note: the order is important because of the unifying side-effects of ==
            if (is_contuation || fn_mono->return_type() == expected) { // TODO this looks overly complicated
                // check if all type variables could be inferred
                bool is_known = true;
                for (size_t i = 0, e = inferred_args.size(); i != e; ++i) {
                    if (!inferred_args[i]->is_known()) {
                        is_known = false;
                        error(expr->loc()) << "could not find instance for type variable '" << fn_poly->type_var(i) << "' of function '" << expr->lhs() << "'\n";
                    }
                }

                if (is_known) {
                    check_bounds(expr->loc(), fn_poly, inferred_args);
                    if (is_contuation)
                        return type_noret();
                    if (!fn_mono->return_type()->is_noret())
                        return expect_type(expr, fn_mono->return_type(), expected);
                    error(expr) << "missing last argument to call continuation\n";
                }
            } else
                error(expr->loc()) << "return type '" << fn_mono->return_type() << "' does not match expected type '" << expected << "'\n";
        } else {
            std::string rela = (num_args+1 < fn_mono->num_args()) ? "few" : "many";
            size_t exp_args = fn_mono->num_args() > 0 ? fn_mono->num_args()-1 : 0;
            error(expr->loc()) << "too " << rela << " arguments: " << num_args << " for " << exp_args << "\n";
        }
    } else
        error(expr->loc()) << "too many type arguments to function: " << num_type_args << " for " << fn_poly->num_type_vars() << "\n";

    return type_error();
}

Type FieldExpr::check(TypeSema& sema, Type expected) const {
    if (auto type = check_as_struct(sema, expected))
        return type;

    if (!lhs()->type()->is_error())
        error(lhs()) << "attempted access of field '" << symbol() << "' on type '" << lhs()->type() << "', but no field with that name was found\n";
    return sema.type_error();
}

Type FieldExpr::check_as_struct(TypeSema& sema, Type expected) const {
    auto ltype = sema.check(lhs());
    if (ltype.isa<PtrType>()) {
        ltype.clear();
        PrefixExpr::create_deref(lhs_);
        ltype = sema.check(lhs());
    }

    if (auto struct_app = ltype.isa<StructAppType>()) {
        if (auto field_decl = struct_app->struct_abs_type()->struct_decl()->field_decl(symbol())) {
            index_ = field_decl->index();
            // a struct cannot have fields of type noret, so we can check against expected (noret defaults to false)
            sema.expect_type(this, struct_app->elem(index_), expected, "field expression type");
            return expected;
        }
    }

    return Type();
}

Type MapExpr::check(TypeSema& sema, Type expected) const {
    if (auto field_expr = lhs()->isa<FieldExpr>()) {
        if (field_expr->check_as_struct(sema, sema.unknown_type()))
            return check_as_map(sema, expected);
        return check_as_method_call(sema, expected);
    }

    return check_as_map(sema, expected);
}

Type MapExpr::check_as_map(TypeSema& sema, Type expected) const {
    auto ltype = sema.check(lhs());
    if (ltype.isa<PtrType>()) {
        ltype.clear();
        PrefixExpr::create_deref(lhs_);
        ltype = sema.check(lhs());
    }

    if (auto fn_poly = ltype.isa<FnType>()) {
        return sema.check_call(this, fn_poly, type_args(), inferred_args_, args(), expected);
    } else if (auto array = ltype.isa<ArrayType>()) {
        if (num_args() == 1) {
            sema.check(arg(0));
            if (sema.expect_int(arg(0)))
                return array->elem_type();
            else
                error(this) << "require integer as array subscript\n";
        } else
            error(this) << "too many array subscripts\n";
    } else if (auto exp_tup = ltype.isa<TupleType>()) {
        if (num_args() == 1) {
            sema.check(arg(0));
            if (sema.expect_int(arg(0))) {
                if (auto lit = arg(0)->isa<LiteralExpr>())
                    return exp_tup->arg(lit->get_u64());
                else
                    error(this) << "require literal as tuple subscript\n";
            } else
                error(this) << "require integer as tuple subscript\n";
        } else
            error(this) << "too many tuple subscripts\n";
    } else if(auto simd = ltype.isa<SimdType>()) {
        if (num_args() == 1) {
            sema.check(arg(0));
            if (!sema.expect_int(arg(0)))
                error(this) << "require integer as vector subscript\n";
            return simd->elem_type();
        } else
            error(this) << "too many simd vector subscripts\n";
    } else
        error(this) << "incorrect type for map expression\n";

    return sema.type_error();
}

Type MapExpr::check_as_method_call(TypeSema& sema, Type expected) const {
    auto field_expr = lhs()->as<FieldExpr>();
    if (auto fn_method = sema.check(field_expr->lhs())->find_method(field_expr->symbol())) {
        Array<const Expr*> nargs(num_args() + 1);
        nargs[0] = field_expr->lhs();
        std::copy(args().begin(), args().end(), nargs.begin()+1);
        return field_expr->type_ = sema.check_call(this, fn_method, type_args(), inferred_args_, nargs, expected);
    } else
        error(this) << "no declaration for method '" << field_expr->symbol() << "' found\n";
    return sema.type_error();
}

Type BlockExprBase::check(TypeSema& sema, Type expected) const {
    THORIN_PUSH(sema.cur_block_, this);
    for (auto stmt : stmts())
        stmt->check(sema);

    sema.check(expr(), expected);

    for (auto local : locals_) {
        if (local->is_mut() && !local->is_written())
            warn(local) << "variable '" << local->symbol() << "' declared mutable but variable is never written to\n";
    }

    return expr() ? expr()->type() : sema.unit().as<Type>();
}

Type IfExpr::check(TypeSema& sema, Type expected) const {
    sema.check(cond(), sema.type_bool(), "condition type");

    // if there is an expected type, we want to pipe it down to enable type inference
    // otherwise we cannot do so because if then_type is noret, else type still can be anything
    if (expected.isa<UnknownType>()) {
        Type then_type = sema.check(then_expr(), sema.unknown_type());
        Type else_type = sema.check(else_expr(), sema.unknown_type());

        if (then_type->is_noret() && else_type->is_noret())
            return sema.type_noret();
        if (then_type->is_noret())
            return sema.expect_type(else_expr(), expected, "if expression type");
        if (else_type->is_noret())
            return sema.expect_type(then_expr(), expected, "if expression type");
        if (then_type == else_type) {
            assert(!then_expr()->needs_cast());
            assert(!else_expr()->needs_cast());
            return sema.expect_type(this, then_type, expected, "if expression type");
        }
        if (then_type <= else_type) {
            assert(!then_expr()->needs_cast());
            then_expr()->actual_type_ = then_type;
            then_expr()->type_.clear();
            then_expr()->type_ = else_type;
            return sema.expect_type(this, else_type, expected, "if expression type");
        }
        if (else_type <= then_type) {
            assert(!else_expr()->needs_cast());
            else_expr()->actual_type_ = else_type;
            else_expr()->type_.clear();
            else_expr()->type_ = then_type;
            return sema.expect_type(this, then_type, expected, "if expression type");
        }

        error(this) << "different types in arms of an if expression\n";
        error(then_expr()) << "type of the consequence is '" << then_type << "'\n";
        error(else_expr()) << "type of the alternative is '" << else_type << "'\n";
        return sema.type_error();
    } else {
        // we always allow noret in one of the branches as long
        Type then_type = sema.check(then_expr(), expected, "type of then branch");
        Type else_type = sema.check(else_expr(), expected, "type of else branch");
        return (then_type->is_noret()) ? else_type : then_type;
    }
}

Type WhileExpr::check(TypeSema& sema, Type) const {
    sema.check(cond(), sema.type_bool(), "condition type");
    break_decl()->check(sema, sema.unknown_type());
    continue_decl()->check(sema, sema.unknown_type());
    sema.check(body(), sema.unit(), "body type of while loop");
    return sema.unit();
}

Type ForExpr::check(TypeSema& sema, Type expected) const {
    auto forexpr = expr();
    if (auto prefix = forexpr->isa<PrefixExpr>())
        if (prefix->kind() == PrefixExpr::RUN || prefix->kind() == PrefixExpr::HLT)
            forexpr = prefix->rhs();
    if (auto map = forexpr->isa<MapExpr>()) {
        Type lhst = sema.check(map->lhs());

        if (auto fn_for = lhst.isa<FnType>()) {
            if (fn_for->num_args() != 0) {
                if (auto fn_ret = fn_for->args().back().isa<FnType>()) {
                    break_decl_->type_ = fn_ret; // inherit the type for break

                    // copy over args and check call
                    Array<const Expr*> args(map->args().size()+1);
                    *std::copy(map->args().begin(), map->args().end(), args.begin()) = fn_expr();
                    return sema.check_call(map, fn_for, map->type_args(), map->inferred_args_, args, expected);
                }
            }
        }
    } else if (auto field_expr = forexpr->isa<FieldExpr>()) {
        assert(false && field_expr && "TODO");
    }

    error(expr()) << "the looping expression does not support the 'for' protocol\n";
    return sema.unit();
}

//------------------------------------------------------------------------------

/*
 * statements
 */

void ExprStmt::check(TypeSema& sema) const {
    if (sema.check(expr())->is_noret())
        error(expr()) << "expression does not return; subsequent statements are unreachable\n";
    if (!expr()->has_side_effect())
        warn(expr()) << "statement with no effect\n";
}

void ItemStmt::check(TypeSema& sema) const {
    item()->check(sema);
}

void LetStmt::check(TypeSema& sema) const {
    sema.cur_block_->add_local(local());
    auto expected = local()->check(sema, sema.unknown_type());
    if (init())
        sema.check(init(), expected, "initialization type");
}

//------------------------------------------------------------------------------

}
