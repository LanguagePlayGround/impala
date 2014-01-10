#include <algorithm>
#include <functional>
#include <sstream>
#include <iostream>

#include "thorin/util/array.h"
#include "thorin/util/assert.h"
#include "thorin/util/push.h"

#include "impala/ast.h"
#include "impala/lexer.h"
#include "impala/prec.h"
#include "impala/type.h"

#define VISIBILITY \
         Token::PRIV: \
    case Token::PUB

#define ITEM \
         Token::ENUM: \
    case Token::EXTERN: \
    case Token::FN: \
    case Token::IMPL: \
    case Token::MOD: \
    case Token::STATIC: \
    case Token::STRUCT: \
    case Token::TRAIT

#define MOD_CONTENTS \
         VISIBILITY: \
    case ITEM

#define EXPR \
         Token::LIT_int8: \
    case Token::LIT_int16: \
    case Token::LIT_int32: \
    case Token::LIT_int64: \
    case Token::LIT_float: \
    case Token::LIT_double: \
    case Token::TRUE: \
    case Token::FALSE: \
    case Token::ADD: \
    case Token::SUB: \
    case Token::MUL: \
    case Token::AND: \
    case Token::NOT: \
    case Token::L_N: \
    case Token::INC: \
    case Token::DEC: \
    case Token:: OR: \
    case Token::L_O: \
    case Token::ID: \
    case Token::RUN: \
    case Token::HALT: \
    case Token::IF: \
    case Token::FOR: \
    case Token::L_PAREN: \
    case Token::L_BRACKET
    
#define STMT_NO_EXPR \
         Token::LET: \
    case ITEM

#define STMT \
        STMT_NO_EXPR: \
    case EXPR

using namespace thorin;

namespace impala {

class Parser;

template<class T>
class Loc {
public:
    inline Loc(Parser& parser, T* node);
    inline ~Loc();

    operator T*() { return node_; }
    T* operator -> () { return node_; }

private:
    Parser& parser_;
    T* node_;
};

class Parser {
public:
    Parser(TypeTable& typetable, std::istream& stream, const std::string& filename)
        : typetable(typetable)
        , lexer(stream, filename)
        //, cur_loop(nullptr)
        , cur_fn_(nullptr)
        , cur_var_handle(2) // reserve 1 for conditionals, 0 for mem
        , no_bars_(false)
        , result_(true)
    {
        lookahead[0] = lexer.lex();
        lookahead[1] = lexer.lex();
    }

    const Token& la(size_t i) const { return lookahead[i]; }
    const Token& la () const { return lookahead[0]; }
    const Token& la2() const { return lookahead[1]; }
    thorin::Location prev_loc() const { return prev_loc_; }

#ifdef NDEBUG
    Token eat(TokenKind /*what*/) { return lex(); }
#else
    Token eat(TokenKind what) { assert(what == la() && "internal parser error"); return lex(); }
#endif

    bool accept(TokenKind tok);
    bool expect(TokenKind tok, const std::string& context);
    void error(const std::string& what, const std::string& context);
    bool result() const { return result_; }
    template<class T>
    Loc<T> loc(T* node) { return Loc<T>(*this, node); }

    void parse_comma_list(TokenKind delimiter, const char* context, std::function<void()> f) {
        if (la() != delimiter) {
            do { f(); }
            while ( accept(Token::COMMA) );
        }
        expect(delimiter, context);
    }

    // misc
    Symbol try_id(const std::string& what);
    bool is_expr() const {
        switch (la()) {
            case EXPR: return true;
            default:   return false;
        }
    }

    //void parse_generics_list(GenericDecls&);
    const Param* parse_param();
    void parse_param_list(Params& params, TokenKind delimiter = Token::R_PAREN);
    const ModContents* parse_mod_contents();

    // types
    const Type* parse_type();
    const Type* parse_array_type();
    const Type* parse_fn_type();
    const Type* parse_tuple_type();
    const Type* parse_return_type();

    // items
    Item*       parse_item();
    ConstItem*  parse_const_item();
    EnumDecl*   parse_enum_decl();
    FnDecl*     parse_fn_decl();
    ForeignMod* parse_foreign_mod();
    Impl*       parse_impl();
    ModDecl*    parse_mod_decl();
    Item*       parse_foreign_mod_or_fn_decl();
    StructDecl* parse_struct_decl();
    TraitDecl*  parse_trait_decl();
    Typedef*    parse_typedef();

    // expressions
    bool is_infix();
    const Expr*    parse_expr(Prec prec);
    const Expr*    parse_expr() { return parse_expr(BOTTOM, false); }
    const Expr*    parse_expr(Prec prec, bool no_bars) { THORIN_PUSH(no_bars_, no_bars); return parse_expr(prec); }
    const Expr*    parse_prefix_expr();
    const Expr*    parse_infix_expr(const Expr* lhs);
    const Expr*    parse_postfix_expr(const Expr* lhs);
    const Expr*    parse_primary_expr();
    const Literal* parse_literal();
    const FnExpr*  parse_fn_expr();
    const IfElse*  parse_if_else();
    const For*     parse_for();
    const Block*   parse_block();
    const Block*   try_block(const std::string& context);

    // statements
    const Stmt*     parse_stmt();
    const ExprStmt* parse_expr_stmt();
    const ItemStmt* parse_item_stmt();
    const LetStmt*  parse_let_stmt();

private:
    /// Consume next Token in input stream, fill look-ahead buffer, return consumed Token.
    Token lex();

    TypeTable& typetable;
    Lexer lexer;       ///< invoked in order to get next token
    Token lookahead[2];///< LL(2) look ahead
    //const Loop* cur_loop;
    const Fn* cur_fn_;
    size_t cur_var_handle;
    bool no_bars_;
    thorin::Location prev_loc_;
    bool result_;
};

//------------------------------------------------------------------------------

template<class T>
Loc<T>::Loc(Parser& parser, T* node)
    : parser_(parser)
    , node_(node)
{
    node_->set_pos1(parser_.la().pos1());
}
template<class T>
Loc<T>::~Loc() { node_->set_pos2(parser_.prev_loc().pos2()); }

//------------------------------------------------------------------------------

const ModContents* parse(TypeTable& typetable, std::istream& i, const std::string& filename) {
    return Parser(typetable, i, filename).parse_mod_contents();
}

//------------------------------------------------------------------------------

/*
 * helpers
 */

Token Parser::lex() {
    Token result  = lookahead[0]; // remember result
    lookahead[0] = lookahead[1];  // copy over LA2 to LA1
    lookahead[1] = lexer.lex();   // fill new LA2
    prev_loc_ = result.loc();     // remember previous location

    return result;
}

bool Parser::accept(TokenKind type) {
    if (type != la())
        return false;
    lex();
    return true;
}

bool Parser::expect(TokenKind tok, const std::string& context) {
    if (la() == tok) {
        lex();
        return true;
    } else {
        std::ostringstream oss;
        oss << '\'' << tok << '\'';
        error(oss.str(), context);
        return false;
    }
}

void Parser::error(const std::string& what, const std::string& context) {
    result_ = false;
    std::ostream& os = la().error() << "expected " << what << ", got '" << la() << "'";
    if (!context.empty())
        os << " while parsing " << context;
    os << "\n";
}

Symbol Parser::try_id(const std::string& what) {
    Token name;
    if (la() == Token::ID)
        name = lex();
    else {
        error("identifier", what);
        name = Token(la().loc(), "<error_id>");
    }

    return name.symbol();
}

//void Parser::parse_generics_list(GenericDecls& generic_decls) {
    //if (accept(Token::L_BRACE))
        //parse_comma_list(Token::R_BRACE, "generics list", [&] {
            //generic_decls.push_back(new GenericDecl(try_id("generic identifier")));
        //});
//}

const Param* Parser::parse_param() {
    auto param = loc(new Param(cur_var_handle++));
    param->mut_ = accept(Token::MUT);
    param->symbol_ = try_id("parameter name");
    if (accept(Token::COLON))
        param->orig_type_ = parse_type();

    return param;
}

void Parser::parse_param_list(Params& params, TokenKind delimiter) {
    parse_comma_list(delimiter, "parameter list", [&] { params.push_back(parse_param()); });
}

const ModContents* Parser::parse_mod_contents() {
    auto mod_contents = loc(new ModContents());

    while (true) {
        switch (la()) {
            case VISIBILITY:
            case ITEM:
                mod_contents->items_.push_back(parse_item());
                continue;
            case Token::SEMICOLON:  
                lex(); 
                continue;
            default:
                break;
        }
    }

    return mod_contents;
}

/*
 * items
 */

Item* Parser::parse_item() {
    Position pos1 = la().pos1();
    Visibility visibility;
    switch (la()) {
        case VISIBILITY: visibility = (Visibility) lex().kind();
        default:         visibility = Visibility::None;
    }

    Item* item = nullptr;
    switch (la()) {
        case Token::ENUM:      item = parse_enum_decl();              break;
        case Token::EXTERN:    item = parse_foreign_mod_or_fn_decl(); break;
        case Token::FN:        item = parse_fn_decl();                break;
        case Token::IMPL:      item = parse_impl();                   break;
        case Token::MOD:       item = parse_mod_decl();               break;
        case Token::STATIC:    item = parse_const_item();             break;
        case Token::STRUCT:    item = parse_struct_decl();            break;
        case Token::TRAIT:     item = parse_trait_decl();             break;
        case Token::TYPE:      item = parse_typedef();                break;
        default: THORIN_UNREACHABLE;
    }

    item->set_pos1(pos1);
    item->visibility_ = visibility;
    return item;
}

EnumDecl* Parser::parse_enum_decl() {
    assert(false && "TODO");
    return 0;
}

Item* Parser::parse_foreign_mod_or_fn_decl() {
    Position pos1 = lex().pos1();
    eat(Token::EXTERN);
    Item* item;
    if (la() == Token::FN) {
        auto fn_decl = parse_fn_decl();
        fn_decl->extern_ = true;
        item = fn_decl;
    } else
        item = parse_fn_decl();

    item->set_pos1(pos1);
    return item;
}

ForeignMod* Parser::parse_foreign_mod() {
    assert(false && "TODO");
    return 0;
}

FnDecl* Parser::parse_fn_decl() {
    auto fn_decl = loc(new FnDecl(typetable));
    auto& fn = fn_decl->fn_;
    eat(Token::FN);
    fn_decl->symbol_ = try_id("function identifier");

    THORIN_PUSH(cur_var_handle, cur_var_handle);
    //parse_generics_list(fn_decl->generics_);

    expect(Token::L_PAREN, "function head");
    parse_param_list(fn.params_);

    // return-continuation
    if (accept(Token::ARROW)) {
        Position pos1 = prev_loc().pos1();
        auto type = parse_return_type();
        Position pos2 = prev_loc().pos2();
        auto param = new Param(cur_var_handle++);
        param->mut_ = false;
        param->symbol_ = "return";
        param->orig_type_ = type;
        param->set_loc(pos1, pos2);
        fn.params_.push_back(param);
    }

    THORIN_PUSH(cur_fn_, &fn);
    fn.body_ = try_block("body of function");

    return fn_decl;
}

Impl* Parser::parse_impl() {
    assert(false && "TODO");
    return 0;
}

ModDecl* Parser::parse_mod_decl() {
    auto mod_decl = loc(new ModDecl());
    eat(Token::MOD);
    mod_decl->symbol_ = try_id("module declaration");
    switch (la()) {
        case MOD_CONTENTS: 
            expect(Token::L_BRACE, "module");
            mod_decl->mod_contents_ = parse_mod_contents();
            expect(Token::R_BRACE, "module");
            break;
        default:
            expect(Token::SEMICOLON, "module declaration");
    }

    return mod_decl;
}

ConstItem* Parser::parse_const_item() {
    assert(false && "TODO");
    return 0;
}

StructDecl* Parser::parse_struct_decl() {
    assert(false && "TODO");
    return 0;
}

TraitDecl* Parser::parse_trait_decl() {
    assert(false && "TODO");
    return 0;
}

Typedef* Parser::parse_typedef() {
    assert(false && "TODO");
    return 0;
}

/*
 * types
 */

const Type* Parser::parse_type() {
    switch (la()) {
#define IMPALA_TYPE(itype, atype) \
        case Token::TYPE_##itype:   lex(); return typetable.type_##itype();
#include "impala/tokenlist.h"
        case Token::TYPE_int:       lex(); return typetable.type_int32();
        case Token::TYPE_void:      lex(); return typetable.type_void();
        case Token::TYPE_noret:     lex(); return typetable.noret();
        case Token::FN:                    return parse_fn_type();
        case Token::L_PAREN:               return parse_tuple_type();
        case Token::ID:                    return typetable.idtype(lex().symbol());
        case Token::L_BRACKET:      lex(); return parse_array_type();
        default: error("type", ""); lex(); return typetable.type_error();
    }
}

const Type* Parser::parse_array_type() {
    const Type* result = parse_type();

    while (accept(Token::MUL)) {
        u64 length;
        switch (la()) {
            case Token::LIT_int8:   length = la().box().get_s8();  break;
            case Token::LIT_int16:  length = la().box().get_s16(); break;
            case Token::LIT_int32:  length = la().box().get_s32(); break;
            case Token::LIT_int64:  length = la().box().get_s64(); break;
            default:
                length = 0;
                error("integer literal", "definite array type");
        }
        lex();
        result = typetable.definite_array(result, length);
    }

    if (!result->isa<DefiniteArray>())
        result = typetable.indefinite_array(result);

    expect(Token::R_BRACKET, "array type");
    return result;
}

const Type* Parser::parse_fn_type() {
    eat(Token::FN);
    std::vector<const Type*> elems;
    expect(Token::L_PAREN, "parameter list of function type");
    parse_comma_list(Token::R_PAREN, "closing parenthesis of function type", [&]{ elems.push_back(parse_type()); });

    if (accept(Token::ARROW))
        elems.push_back(parse_return_type());

    return typetable.fntype(elems);
}

const Type* Parser::parse_tuple_type() {
    eat(Token::L_PAREN);
    std::vector<const Type*> elems;
    parse_comma_list(Token::R_PAREN, "closing parenthesis of sigma type", [&]{ elems.push_back(parse_type()); });

    return typetable.tupletype(elems);
}

const Type* Parser::parse_return_type() {
    auto ret_type = parse_type();
    return ret_type->is_void() ? typetable.fntype({}) : typetable.fntype({ret_type});
}

/*
 * expressions
 */

bool Parser::is_infix() {
    bool infix = la().is_infix();
    if (no_bars_ && infix)
        return la() != Token::OR && la() != Token::L_O;
    return infix;
}

const Expr* Parser::parse_expr(Prec prec) {
    auto lhs = la().is_prefix() ? parse_prefix_expr() : parse_primary_expr();

    while (true) {
        /*
         * (lhs  op  LA) op ...  on break  (current prec > lhs prec of LA)  -->  reduce
         *  lhs  op (LA  op ...) otherwise                                  -->  shift
         */

        if (is_infix()) {
            if (prec > PrecTable::infix_l[la()])
                break;

            lhs = parse_infix_expr(lhs);
        } else if ( la().is_postfix() ) {
            if (prec > PrecTable::postfix_l[la()])
                break;

            lhs = parse_postfix_expr(lhs);
        } else
            break;
    }

    return lhs;
}

const Expr* Parser::parse_prefix_expr() {
    if (la() == Token::OR || la() == Token::L_O)
        return parse_fn_expr();
    Token op = lex();
    auto rhs = parse_expr(PrecTable::prefix_r[op]);

    return new PrefixExpr(op.pos1(), (PrefixExpr::Kind) op.kind(), rhs);
}

const Expr* Parser::parse_infix_expr(const Expr* lhs) {
    Token op = lex();
    if (op == Token::QUESTION_MARK) {
        auto t_expr = parse_expr();
        expect(Token::COLON, "conditional expression");
        auto f_expr = parse_expr(PrecTable::infix_r[op]);
        return new ConditionalExpr(lhs, t_expr, f_expr);
    }

    auto rhs = parse_expr(PrecTable::infix_r[op]);

    return new InfixExpr(lhs, (InfixExpr::Kind) op.kind(), rhs);
}

const Expr* Parser::parse_postfix_expr(const Expr* lhs) {
    if (accept(Token::L_PAREN)) {
        auto call = loc(new Call());
        call->ops_.push_back(lhs);
        parse_comma_list(Token::R_PAREN, "arguments of a function call", [&]{ call->append_arg(parse_expr()); });
        return call;
    } else if (accept(Token::L_BRACKET)) {
        auto expr = loc(new IndexExpr());
        expr->ops_.push_back(lhs);
        expr->ops_.push_back(parse_expr());
        expect(Token::R_BRACKET, "index expression");
        return expr;
    } else {
        auto expr = loc(new PostfixExpr());
        expr->ops_.push_back(lhs);
        assert(la() == Token::INC || la() == Token::DEC); 
        expr->kind_ = (PostfixExpr::Kind) lex().kind();
        return expr;
    }
}

const Expr* Parser::parse_primary_expr() {
    switch (la() ) {
        case Token::L_PAREN: {
            Position pos1 = lex().pos1();
            auto expr = parse_expr();
            if (accept(Token::COMMA)) {
                auto tuple = new Tuple();
                tuple->set_pos1(pos1);
                tuple->ops_.push_back(expr);
                parse_comma_list(Token::R_PAREN, "elements of tuple expression", [&]{ tuple->ops_.push_back(parse_expr()); });
                tuple->set_pos2(prev_loc().pos2());
                return tuple;
            } else {
                expect(Token::R_PAREN, "primary expression");
                return expr;
            }
        }
        case Token::L_BRACKET: {
            auto array = new ArrayExpr();
            array->set_pos1(lex().pos1());
            parse_comma_list(Token::R_BRACKET, "elements of array expression", [&]{ array->ops_.push_back(parse_expr()); });
            array->set_pos2(prev_loc().pos2());
            return array;
        }
#define IMPALA_LIT(itype, atype) \
        case Token::LIT_##itype:
#include "impala/tokenlist.h"
        case Token::TRUE:
        case Token::FALSE:      return parse_literal();
        case Token::ID:         return new Id(lex());
        default:                error("expression", ""); return new EmptyExpr(lex().loc());
    }
}

const Literal* Parser::parse_literal() {
    Literal::Kind kind;
    Box box;

    switch (la()) {
        case Token::TRUE:  return new Literal(lex().loc(), Literal::LIT_bool, Box(true));
        case Token::FALSE: return new Literal(lex().loc(), Literal::LIT_bool, Box(false));
#define IMPALA_LIT(itype, atype) \
        case Token::LIT_##itype: { \
            kind = Literal::LIT_##itype; \
            Box box = la().box(); \
            return new Literal(lex().loc(), kind, box); \
        }
#include "impala/tokenlist.h"
        default: THORIN_UNREACHABLE;
    }
}

const FnExpr* Parser::parse_fn_expr() {
    auto fn_expr = loc(new FnExpr(typetable));
    auto& fn = fn_expr->fn_;

    THORIN_PUSH(cur_fn_, &fn);
    THORIN_PUSH(cur_var_handle, cur_var_handle);

    if (accept(Token::OR))
        parse_param_list(fn.params_);
    else
        expect(Token::L_O, "parameter list of function expression");

    // return-continuation
    //if (accept(Token::ARROW)) {
        //Position pos1 = prev_loc().pos1();
        //Position pos2 = prev_loc().pos2();
        //fn->params_.push_back(new VarDecl(cur_var_handle++, true, Token(pos1, "return"), arg_types.back(), pos2));
    //}

    fn.body_ = parse_expr();

    return fn_expr;
}

const IfElse* Parser::parse_if_else() {
    auto ifelse = loc(new IfElse());
    eat(Token::IF);
    ifelse->cond_ = parse_expr();
    ifelse->then_block_ = parse_block();
    ifelse->else_block_ = accept(Token::ELSE) ? parse_block() : new Block(prev_loc());
    return ifelse;
}

const For* Parser::parse_for() {
    auto for_expr = loc(new For());
    auto& fn = for_expr->fn_;
    eat(Token::FOR);
    parse_param_list(fn.params_, Token::IN);
    for_expr->expr_ = parse_expr();
    fn.body_ = try_block("body of function");
    return for_expr;
}

const Block* Parser::parse_block() {
    eat(Token::L_BRACE);
    auto block = loc(new Block());
    auto& stmts = block->stmts_;
    while (true) {
        switch (la()) {
            case Token::SEMICOLON:  lex(); continue; // ignore semicolon
            case STMT_NO_EXPR:      stmts.push_back(parse_stmt()); continue;
            case EXPR: {
                auto expr = parse_expr();
                if (accept(Token::SEMICOLON)) {
                    auto expr_stmt = new ExprStmt();
                    expr_stmt->set_loc(expr->pos1(), prev_loc().pos2());
                    expr_stmt->expr_ = expr;
                    continue;
                } else
                    block->expr_ = expr;
                // FALLTHROUGH
            } 
            default:
                expect(Token::R_BRACE, "block");
                return block;
        }
    }
}

const Block* Parser::try_block(const std::string& context) {
    if (la() == Token::L_BRACE)
        return parse_block();

    error("block", context);
    auto block = new Block();
    block->set_loc(prev_loc());
    return block;
}

/*
 * statements
 */

const Stmt* Parser::parse_stmt() {
    switch (la()) {
        case EXPR:       return parse_expr_stmt();
        case ITEM:       return parse_item_stmt();
        case Token::LET: return parse_let_stmt();
        default:         THORIN_UNREACHABLE;
    }
}

const ExprStmt* Parser::parse_expr_stmt() {
    auto s = loc(new ExprStmt());
    s->expr_ = parse_expr();
    expect(Token::SEMICOLON, "the end of an expression statement");
    return s;
}

const LetStmt* Parser::parse_let_stmt() {
    auto let_stmt = loc(new LetStmt());
    eat(Token::LET);
    //let_stmt->var_decl_ = parse_var_decl(false);
    //if (accept(Token::ASGN))
        //let_stmt->init_ = parse_expr();

    //expect(Token::SEMICOLON, "the end of an initialization statement");
    return let_stmt;
}

} // namespace impala