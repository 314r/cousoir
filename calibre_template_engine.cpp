// SPDX-License-Identifier: GPL-3.0-only
//
// Ported from Calibre, https://calibre-ebook.com, Copyright (C) Kovid Goyal,
// GPL v3:
// - src/calibre/utils/formatter.py: the template language's lexer
//   (cached_lex_scanner()), _Parser, _Interpreter and TemplateFormatter, and
//   the format string parsing of Python's string.Formatter it relies on
//
// CREDITS.md lists all of the Calibre code in Cousoir.

#include <QCollator>
#include <QLocale>

#include <cmath>
#include <iostream>

#include "calibre_template_p.h"

namespace CalibreTemplate::Internal {

namespace {

[[noreturn]] void fail(const QString &message)
{
    throw TemplateError{ message };
}

// --- The lexer ---------------------------------------------------------------

enum class Lex { Op = 1, Id, Const, Eof, StringInfix, NumericInfix, Keyword, Newline };

struct Token
{
    Lex type = Lex::Eof;
    QString text;
};

// cached_lex_scanner(): the patterns in order, the first that matches where
// the scan is winning; a space is nothing, a newline or a comment a newline.
QList<Token> scan(const QString &program, QString *rest)
{
    struct Rule
    {
        QRegularExpression pattern;
        Lex type;
        int strip;
    };
    const auto rule = [](const char *pattern, Lex type, int strip = 0) {
        return Rule{ QRegularExpression(QString::fromLatin1(pattern),
                                        QRegularExpression::UseUnicodePropertiesOption
                                            | QRegularExpression::DotMatchesEverythingOption),
                     type, strip };
    };
    static const QList<Rule> rules{
        rule("(?:==#|!=#|<=#|<#|>=#|>#)", Lex::NumericInfix),
        rule("(?:==|!=|<=|<|>=|>)", Lex::StringInfix),
        rule("(?:if|then|else|elif|fi)\\b", Lex::Keyword),
        rule("(?:for|in|rof|separator)\\b", Lex::Keyword),
        rule("(?:separator|limit)\\b", Lex::Keyword),
        rule("(?:def|fed|continue)\\b", Lex::Keyword),
        rule("(?:return|inlist|break)\\b", Lex::Keyword),
        rule("(?:inlist_field)\\b", Lex::Keyword),
        rule("(?:with|htiw)\\b", Lex::Keyword),
        rule("(?:\\|\\||&&|!|{|})", Lex::Op),
        rule("[(),=;:\\+\\-*/&]", Lex::Op),
        rule("-?[\\d\\.]+", Lex::Const),
        rule("\\$\\$?#?\\w+", Lex::Id),
        rule("\\$", Lex::Id),
        rule("\\w+", Lex::Id),
        rule("\".*?(?:(?<!\\\\)\")", Lex::Const, 1),
        rule("'.*?(?:(?<!\\\\)')", Lex::Const, 1),
        rule("\\n[ \\t]*#.*?(?:(?=\\n)|$)", Lex::Newline),
        rule("\\s", Lex::Newline),
    };
    QList<Token> tokens;
    qsizetype at = 0;
    while (at < program.size()) {
        bool matched = false;
        for (const Rule &candidate : rules) {
            const QRegularExpressionMatch match = candidate.pattern.match(
                program, at, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
            if (!match.hasMatch() || match.capturedLength() == 0)
                continue;
            const QString text = match.captured();
            if (candidate.type == Lex::Newline) {
                // A space other than a newline is nothing.
                if (text == QLatin1String("\n") || text.startsWith(u'\n'))
                    tokens.append({ Lex::Newline, {} });
            } else {
                tokens.append({ candidate.type, candidate.strip ? text.mid(1, text.size() - 2) : text });
            }
            at += match.capturedLength();
            matched = true;
            break;
        }
        if (!matched)
            break;
    }
    *rest = program.mid(at);
    return tokens;
}

} // namespace

// --- The tree ----------------------------------------------------------------

struct Node;
using NodePtr = std::shared_ptr<Node>;
// An expression list: its value is the last one's.
using Expr = QList<NodePtr>;

struct Node
{
    enum class Type {
        If, Assign, Constant, Rvalue, Func, Field, RawField, CompareString, CompareNumeric, Arguments,
        FirstNonEmpty, Switch, SwitchIf, For, Range, Globals, SetGlobals, Contains, BinaryLogop, UnaryLogop,
        BinaryArithop, UnaryArithop, Print, Break, Continue, Return, Character, Strcat, BinaryStringop,
        LocalFunctionDefine, LocalFunctionCall, ListCountField, With, FString
    };
    Type type;
    int line = 0;
    QString name;
    QString value;
    // Children, as the node uses them: condition, then and else parts;
    // operands; arguments.
    QList<Expr> parts;
    bool hasDefault = false;
};

namespace {

NodePtr make(Node::Type type, int line, QList<Expr> parts = {}, const QString &name = {}, const QString &value = {})
{
    auto node = std::make_shared<Node>();
    node->type = type;
    node->line = line;
    node->parts = std::move(parts);
    node->name = name;
    node->value = value;
    return node;
}

Expr one(const NodePtr &node)
{
    return Expr{ node };
}

// --- The parser --------------------------------------------------------------

class Parser
{
public:
    Expr program(const QString &text, QSet<QString> *localFunctions = nullptr)
    {
        QString rest;
        m_prog = scan(text, &rest);
        m_at = 0;
        m_line = 1;
        m_ownFunctions.clear();
        m_localFunctions = localFunctions ? localFunctions : &m_ownFunctions;
        if (!rest.isEmpty())
            error(QStringLiteral("Failed to scan program. Invalid input '%1'").arg(rest));
        Expr tree = expressionList();
        if (!tokenIsEof())
            error(QStringLiteral("Expected end of program, found '%1'").arg(tokenText()));
        return tree;
    }

private:
    [[noreturn]] void error(const QString &message)
    {
        QString value = QStringLiteral("'Unknown'");
        if (m_at > 0 && m_at - 1 < m_prog.size() && m_prog.at(m_at - 1).type != Lex::Newline)
            value = u'\'' + m_prog.at(m_at - 1).text + u'\'';
        if (m_at > 0 && m_at < m_prog.size())
            fail(QStringLiteral("Formatter: %1 near %2 on line %3").arg(message, value).arg(m_line));
        fail(QStringLiteral("Formatter: %1 near the end of the program").arg(message));
    }

    void checkEol()
    {
        while (m_at < m_prog.size() && m_prog.at(m_at).type == Lex::Newline) {
            ++m_line;
            ++m_at;
        }
    }
    QString token()
    {
        checkEol();
        if (m_at >= m_prog.size())
            return {};
        return m_prog.at(m_at++).text;
    }
    void consume() { ++m_at; }
    bool is(Lex type, const QString &text = {})
    {
        checkEol();
        return m_at < m_prog.size() && m_prog.at(m_at).type == type && (text.isNull() || m_prog.at(m_at).text == text);
    }
    bool opIs(const QString &op) { return is(Lex::Op, op); }
    bool tokenIs(const QString &keyword) { return is(Lex::Keyword, keyword); }
    bool tokenIsNewline() { return m_at < m_prog.size() && m_prog.at(m_at).type == Lex::Newline; }
    bool tokenIsEof()
    {
        checkEol();
        return m_at >= m_prog.size() || m_prog.at(m_at).type == Lex::Eof;
    }
    QString tokenText()
    {
        checkEol();
        return m_at < m_prog.size() ? m_prog.at(m_at).text : QStringLiteral("'End of program'");
    }

    Expr expressionList()
    {
        Expr list;
        while (true) {
            while (tokenIsNewline()) {
                ++m_line;
                consume();
            }
            if (tokenIsEof())
                break;
            list.append(topExpr());
            if (opIs(QStringLiteral(";")))
                consume();
            else
                break;
        }
        return list;
    }

    NodePtr ifExpression()
    {
        consume();
        const int line = m_line;
        const NodePtr condition = topExpr();
        if (!tokenIs(QStringLiteral("then")))
            error(QStringLiteral("if statement: expected 'then', found '%1'").arg(tokenText()));
        consume();
        const Expr thenPart = expressionList();
        if (tokenIs(QStringLiteral("elif")))
            return make(Node::Type::If, line, { one(condition), thenPart, one(ifExpression()) });
        Expr elsePart;
        bool hasElse = false;
        if (tokenIs(QStringLiteral("else"))) {
            consume();
            elsePart = expressionList();
            hasElse = true;
        }
        if (!tokenIs(QStringLiteral("fi")))
            error(QStringLiteral("if statement: expected 'fi', found '%1'").arg(tokenText()));
        consume();
        NodePtr node = make(Node::Type::If, line, { one(condition), thenPart, elsePart });
        node->hasDefault = hasElse;
        return node;
    }

    NodePtr forExpression()
    {
        const int line = m_line;
        consume();
        if (!is(Lex::Id))
            error(QStringLiteral("'for' statement: expected an identifier"));
        const QString variable = token();
        if (!tokenIs(QStringLiteral("in")))
            error(QStringLiteral("for statement: expected 'in', found '%1'").arg(tokenText()));
        consume();
        bool isList = true;
        Expr listExpr;
        Expr separator;
        Expr start = one(make(Node::Type::Constant, line, {}, {}, QStringLiteral("0")));
        Expr stop;
        Expr step = one(make(Node::Type::Constant, line, {}, {}, QStringLiteral("1")));
        Expr limit;
        if (tokenText() == QLatin1String("range")) {
            isList = false;
            consume();
            if (!opIs(QStringLiteral("(")))
                error(QStringLiteral("for statement: expected '(', found '%1'").arg(tokenText()));
            consume();
            stop = one(topExpr());
            if (opIs(QStringLiteral(","))) {
                consume();
                start = stop;
                stop = one(topExpr());
                if (opIs(QStringLiteral(","))) {
                    consume();
                    step = one(topExpr());
                    if (opIs(QStringLiteral(","))) {
                        consume();
                        limit = one(topExpr());
                    }
                }
            }
            if (!opIs(QStringLiteral(")")))
                error(QStringLiteral("for statement: expected ')', found '%1'").arg(tokenText()));
            consume();
        } else {
            listExpr = one(topExpr());
            if (tokenIs(QStringLiteral("separator"))) {
                consume();
                separator = one(expr());
            }
        }
        if (!opIs(QStringLiteral(":")))
            error(QStringLiteral("for statement: expected ':', found '%1'").arg(tokenText()));
        consume();
        const Expr block = expressionList();
        if (!tokenIs(QStringLiteral("rof")))
            error(QStringLiteral("for statement: expected 'rof', found '%1'").arg(tokenText()));
        consume();
        if (isList) {
            NodePtr node = make(Node::Type::For, line, { listExpr, separator, block }, variable);
            node->hasDefault = !separator.isEmpty();
            return node;
        }
        NodePtr node = make(Node::Type::Range, line, { start, stop, step, limit, block }, variable);
        node->hasDefault = !limit.isEmpty();
        return node;
    }

    NodePtr defineFunctionExpression()
    {
        consume();
        const int line = m_line;
        if (!is(Lex::Id))
            error(QStringLiteral("'def' statement: expected a function name identifier"));
        const QString name = token();
        if (m_localFunctions->contains(name))
            error(QStringLiteral("Function name '%1' is already defined").arg(name));
        if (!opIs(QStringLiteral("(")))
            error(QStringLiteral("'def' statement: expected a '('"));
        consume();
        QList<Expr> arguments;
        while (!opIs(QStringLiteral(")"))) {
            NodePtr argument = topExpr();
            if (argument->type != Node::Type::Assign && argument->type != Node::Type::Rvalue)
                error(QStringLiteral("Parameters to a function must be variables or assignments"));
            if (argument->type == Node::Type::Rvalue)
                argument = make(Node::Type::Assign, line,
                                { one(make(Node::Type::Constant, m_line, {}, {}, QString(u""))) }, argument->name);
            arguments.append(one(argument));
            if (!opIs(QStringLiteral(",")))
                break;
            consume();
        }
        if (token() != QLatin1String(")"))
            error(QStringLiteral("'def' statement: expected a ')' at end of argument list"));
        if (!opIs(QStringLiteral(":")))
            error(QStringLiteral("'def' statement: missing ':'"));
        consume();
        const Expr block = expressionList();
        if (!tokenIs(QStringLiteral("fed")))
            error(QStringLiteral("'def' statement: missing the closing 'fed'"));
        consume();
        m_localFunctions->insert(name);
        arguments.prepend(block);
        return make(Node::Type::LocalFunctionDefine, line, arguments, name);
    }

    NodePtr withExpression()
    {
        consume();
        const int line = m_line;
        const NodePtr book = topExpr();
        if (!opIs(QStringLiteral(":")))
            error(QStringLiteral("with statement: expected ':', found '%1'").arg(tokenText()));
        consume();
        const Expr block = expressionList();
        if (!tokenIs(QStringLiteral("htiw")))
            error(QStringLiteral("'def' statement: missing the closing 'fed'"));
        consume();
        return make(Node::Type::With, line, { one(book), block });
    }

    NodePtr topExpr() { return orExpr(); }

    NodePtr orExpr()
    {
        NodePtr left = andExpr();
        while (opIs(QStringLiteral("||"))) {
            consume();
            left = make(Node::Type::BinaryLogop, m_line, { one(left), one(andExpr()) }, QStringLiteral("or"));
        }
        return left;
    }

    NodePtr andExpr()
    {
        NodePtr left = notExpr();
        while (opIs(QStringLiteral("&&"))) {
            consume();
            left = make(Node::Type::BinaryLogop, m_line, { one(left), one(notExpr()) }, QStringLiteral("and"));
        }
        return left;
    }

    NodePtr notExpr()
    {
        if (opIs(QStringLiteral("!"))) {
            consume();
            return make(Node::Type::UnaryLogop, m_line, { one(notExpr()) }, QStringLiteral("not"));
        }
        return stringBinaryExpr();
    }

    NodePtr stringBinaryExpr()
    {
        NodePtr left = compareExpr();
        while (opIs(QStringLiteral("&"))) {
            const QString op = token();
            left = make(Node::Type::BinaryStringop, m_line, { one(left), one(compareExpr()) }, op);
        }
        return left;
    }

    NodePtr compareExpr()
    {
        NodePtr left = addSubtractExpr();
        if (is(Lex::StringInfix) || tokenIs(QStringLiteral("in")) || tokenIs(QStringLiteral("inlist"))
            || tokenIs(QStringLiteral("inlist_field"))) {
            const QString op = token();
            return make(Node::Type::CompareString, m_line, { one(left), one(addSubtractExpr()) }, op);
        }
        if (is(Lex::NumericInfix)) {
            const QString op = token();
            return make(Node::Type::CompareNumeric, m_line, { one(left), one(addSubtractExpr()) }, op);
        }
        return left;
    }

    NodePtr addSubtractExpr()
    {
        NodePtr left = timesDivideExpr();
        while (opIs(QStringLiteral("+")) || opIs(QStringLiteral("-"))) {
            const QString op = token();
            left = make(Node::Type::BinaryArithop, m_line, { one(left), one(timesDivideExpr()) }, op);
        }
        return left;
    }

    NodePtr timesDivideExpr()
    {
        NodePtr left = unaryPlusMinusExpr();
        while (opIs(QStringLiteral("*")) || opIs(QStringLiteral("/"))) {
            const QString op = token();
            left = make(Node::Type::BinaryArithop, m_line, { one(left), one(unaryPlusMinusExpr()) }, op);
        }
        return left;
    }

    NodePtr unaryPlusMinusExpr()
    {
        if (opIs(QStringLiteral("+"))) {
            consume();
            return make(Node::Type::UnaryArithop, m_line, { one(unaryPlusMinusExpr()) }, QStringLiteral("+"));
        }
        if (opIs(QStringLiteral("-"))) {
            consume();
            return make(Node::Type::UnaryArithop, m_line, { one(unaryPlusMinusExpr()) }, QStringLiteral("-"));
        }
        return expr();
    }

    // A parenthesised list is an expression too: its value is its last.
    NodePtr listNode(const Expr &list)
    {
        NodePtr node = make(Node::Type::Strcat, m_line, {}, QStringLiteral("list"));
        node->parts = { list };
        node->hasDefault = true;
        return node;
    }

    NodePtr expr()
    {
        if (opIs(QStringLiteral("("))) {
            consume();
            const Expr list = expressionList();
            if (!opIs(QStringLiteral(")")))
                error(QStringLiteral("Expected ')', found '%1'").arg(tokenText()));
            consume();
            return listNode(list);
        }
        if (is(Lex::Keyword)) {
            const QString keyword = tokenText();
            if (keyword == QLatin1String("if"))
                return ifExpression();
            if (keyword == QLatin1String("for"))
                return forExpression();
            if (keyword == QLatin1String("break")) {
                consume();
                return make(Node::Type::Break, m_line);
            }
            if (keyword == QLatin1String("continue")) {
                consume();
                return make(Node::Type::Continue, m_line);
            }
            if (keyword == QLatin1String("return")) {
                consume();
                return make(Node::Type::Return, m_line, { one(topExpr()) });
            }
            if (keyword == QLatin1String("def"))
                return defineFunctionExpression();
            if (keyword == QLatin1String("with"))
                return withExpression();
        }
        if (is(Lex::Id)) {
            const int line = m_line;
            QString id = token();
            if (id.size() > 1 && id.at(0) == u'$') {
                if (id.at(1) == u'$')
                    return make(Node::Type::RawField, line,
                                { one(make(Node::Type::Constant, m_line, {}, {}, id.mid(2))) });
                return make(Node::Type::Field, line, { one(make(Node::Type::Constant, m_line, {}, {}, id.mid(1))) });
            }
            if (!opIs(QStringLiteral("("))) {
                if (opIs(QStringLiteral("="))) {
                    consume();
                    return make(Node::Type::Assign, line, { one(topExpr()) }, id);
                }
                return make(Node::Type::Rvalue, line, {}, id);
            }
            id = id.trimmed();
            const bool known = functions().contains(id) || isInlined(id);
            if (!known && !m_localFunctions->contains(id))
                error(QStringLiteral("Unknown function %1").arg(id));
            consume();
            QList<Expr> arguments;
            while (!opIs(QStringLiteral(")"))) {
                arguments.append(expressionList());
                if (!opIs(QStringLiteral(",")))
                    break;
                consume();
            }
            const QString closing = token();
            if (closing != QLatin1String(")"))
                error(QStringLiteral("Expected a ')' for function call, found '%1'").arg(closing));
            if (NodePtr inlined = inlineFunction(id, line, arguments))
                return inlined;
            if (id == QLatin1String("arguments") || id == QLatin1String("globals") || id == QLatin1String("set_globals")) {
                QList<Expr> assignments;
                for (const Expr &argument : std::as_const(arguments)) {
                    NodePtr node = argument.value(0);
                    if (!node || (node->type != Node::Type::Assign && node->type != Node::Type::Rvalue))
                        error(QStringLiteral("Parameters to '%1' must be variables or assignments").arg(id));
                    if (node->type == Node::Type::Rvalue)
                        node = make(Node::Type::Assign, line,
                                    { one(make(Node::Type::Constant, m_line, {}, {}, QString(u""))) }, node->name);
                    assignments.append(one(node));
                }
                const Node::Type type = id == QLatin1String("arguments") ? Node::Type::Arguments
                    : id == QLatin1String("set_globals")                 ? Node::Type::SetGlobals
                                                                         : Node::Type::Globals;
                return make(type, line, assignments);
            }
            if (m_localFunctions->contains(id))
                return make(Node::Type::LocalFunctionCall, m_line, arguments, id);
            const Function &function = functions().value(id);
            if (function.argCount != -1 && arguments.size() != function.argCount)
                error(QStringLiteral("Incorrect number of arguments for function %1").arg(id));
            return make(Node::Type::Func, line, arguments, id);
        }
        if (is(Lex::Const))
            return make(Node::Type::Constant, m_line, {}, {}, QString(u"") + token());
        error(QStringLiteral("Expected an expression, found '%1'").arg(tokenText()));
    }

    static bool isInlined(const QString &id)
    {
        static const QSet<QString> names{ QStringLiteral("field"), QStringLiteral("raw_field"), QStringLiteral("test"),
                                          QStringLiteral("first_non_empty"), QStringLiteral("switch"),
                                          QStringLiteral("switch_if"), QStringLiteral("assign"),
                                          QStringLiteral("contains"), QStringLiteral("character"),
                                          QStringLiteral("print"), QStringLiteral("strcat"),
                                          QStringLiteral("list_count_field"), QStringLiteral("f_string"),
                                          QStringLiteral("arguments"), QStringLiteral("globals"),
                                          QStringLiteral("set_globals") };
        return names.contains(id);
    }

    // inlined_function_nodes: the functions the parser makes nodes of, when
    // their arguments fit.
    NodePtr inlineFunction(const QString &id, int line, const QList<Expr> &args)
    {
        const qsizetype n = args.size();
        if (id == QLatin1String("field") && n == 1)
            return make(Node::Type::Field, line, { args.at(0) });
        if (id == QLatin1String("raw_field") && n == 1)
            return make(Node::Type::RawField, line, { args.at(0) });
        if (id == QLatin1String("test") && n == 3) {
            NodePtr node = make(Node::Type::If, line, { args.at(0), args.at(1), args.at(2) });
            node->hasDefault = true;
            return node;
        }
        if (id == QLatin1String("first_non_empty") && n >= 1)
            return make(Node::Type::FirstNonEmpty, line, args);
        if (id == QLatin1String("switch") && n >= 3 && n % 2 == 0)
            return make(Node::Type::Switch, line, args);
        if (id == QLatin1String("switch_if") && n > 0 && n % 2 == 1)
            return make(Node::Type::SwitchIf, line, args);
        if (id == QLatin1String("assign") && n == 2 && args.at(0).size() == 1
            && args.at(0).first()->type == Node::Type::Rvalue)
            return make(Node::Type::Assign, line, { args.at(1) }, args.at(0).first()->name);
        if (id == QLatin1String("contains") && n == 4)
            return make(Node::Type::Contains, line, args);
        if (id == QLatin1String("character") && n == 1)
            return make(Node::Type::Character, line, { args.at(0) });
        if (id == QLatin1String("print"))
            return make(Node::Type::Print, line, args);
        if (id == QLatin1String("strcat"))
            return make(Node::Type::Strcat, line, args);
        if (id == QLatin1String("list_count_field") && n == 1)
            return make(Node::Type::ListCountField, line, { args.at(0) });
        if (id == QLatin1String("f_string") && n == 1)
            return make(Node::Type::FString, line, { args.at(0) });
        return nullptr;
    }

    QList<Token> m_prog;
    qsizetype m_at = 0;
    int m_line = 1;
    QSet<QString> m_ownFunctions;
    QSet<QString> *m_localFunctions = nullptr;
};

// The signals of break, continue and return, carrying the value so far.
struct LoopSignal
{
    bool isBreak;
    QString value;
};
struct ReturnSignal
{
    QString value;
};

} // namespace

// --- The interpreter -----------------------------------------------------------

struct Interpreter
{
    Formatter &formatter;
    const BookProxy *book;
    QHash<QString, QString> locals;
    QHash<QString, NodePtr> localFunctions;

    [[noreturn]] void error(const QString &message, int line)
    {
        fail(QStringLiteral("Interpreter: %1 - line number %2").arg(message).arg(line));
    }

    QString run(const Expr &program, const std::optional<QString> &dollar)
    {
        if (dollar)
            locals.insert(QStringLiteral("$"), *dollar);
        try {
            return list(program);
        } catch (const ReturnSignal &signal) {
            return signal.value;
        } catch (const LoopSignal &signal) {
            fail(QStringLiteral("%1 outside of for loop").arg(signal.isBreak ? QStringLiteral("break")
                                                                            : QStringLiteral("continue")));
        }
    }

    QString list(const Expr &program)
    {
        QString value = QString(u"");
        try {
            for (const NodePtr &node : program)
                value = eval(*node);
        } catch (LoopSignal &signal) {
            signal.value = value;
            throw;
        }
        return value;
    }

    static double floatOrZero(const QString &value)
    {
        // Undefined values and the string 'None' are zero.
        return Py::toFloat(value.isEmpty() || value == QLatin1String("None") ? QStringLiteral("0") : value);
    }

    QString eval(const Node &node)
    {
        switch (node.type) {
        case Node::Type::Constant:
            return node.value;
        case Node::Type::Rvalue: {
            const auto found = locals.constFind(node.name);
            if (found == locals.cend())
                error(QStringLiteral("Unknown identifier '%1'").arg(node.name), node.line);
            return *found;
        }
        case Node::Type::Assign: {
            const QString value = list(node.parts.at(0));
            locals.insert(node.name, value);
            return value;
        }
        case Node::Type::If: {
            if (!list(node.parts.at(0)).isEmpty())
                return list(node.parts.at(1));
            if (!node.parts.at(2).isEmpty())
                return list(node.parts.at(2));
            return QString(u"");
        }
        case Node::Type::Func: {
            QStringList args;
            for (const Expr &argument : node.parts)
                args.append(list(argument));
            FunctionContext context{ formatter, *book, locals };
            return functions().value(node.name).evaluate(context, args);
        }
        case Node::Type::Field: {
            const QString name = list(node.parts.at(0));
            try {
                return book->value(name);
            } catch (const TemplateError &) {
                error(QStringLiteral("Unknown field '%1'").arg(name), node.line);
            }
        }
        case Node::Type::RawField: {
            const QString name = book->library.fieldKey(list(node.parts.at(0)));
            const std::optional<PyValue> raw = book->attribute(name);
            const PyValue value = raw.value_or(PyValue());
            if (value.isNone() && node.parts.size() > 1)
                return list(node.parts.at(1));
            if (value.kind == PyValue::Kind::List) {
                const QString separator = book->library.listToUi(name);
                return value.list.join(separator.isEmpty() ? QStringLiteral(", ") : separator);
            }
            return value.str();
        }
        case Node::Type::FirstNonEmpty:
            for (const Expr &argument : node.parts) {
                const QString value = list(argument);
                if (!value.isEmpty())
                    return value;
            }
            return QString(u"");
        case Node::Type::Switch: {
            const QString value = list(node.parts.at(0));
            for (qsizetype i = 1; i < node.parts.size() - 1; i += 2) {
                if (Py::regex(list(node.parts.at(i)), true).match(value).hasMatch())
                    return list(node.parts.at(i + 1));
            }
            return list(node.parts.last());
        }
        case Node::Type::SwitchIf:
            for (qsizetype i = 0; i + 1 < node.parts.size(); i += 2) {
                if (!list(node.parts.at(i)).isEmpty())
                    return list(node.parts.at(i + 1));
            }
            return list(node.parts.last());
        case Node::Type::Strcat: {
            if (node.hasDefault)
                return list(node.parts.at(0));
            QString result = QString(u"");
            for (const Expr &argument : node.parts)
                result += list(argument);
            return result;
        }
        case Node::Type::Contains: {
            const QString value = list(node.parts.at(0));
            const QString test = list(node.parts.at(1));
            return Py::regex(test, true).match(value).hasMatch() ? list(node.parts.at(2)) : list(node.parts.at(3));
        }
        case Node::Type::Character: {
            const QString key = list(node.parts.at(0));
            static const QHash<QString, QString> characters{
                { QStringLiteral("return"), QStringLiteral("\r") }, { QStringLiteral("newline"), QStringLiteral("\n") },
                { QStringLiteral("tab"), QStringLiteral("\t") }, { QStringLiteral("backslash"), QStringLiteral("\\") } };
            if (!characters.contains(key))
                error(QStringLiteral("Function character: invalid character name '%1").arg(key), node.line);
            return characters.value(key);
        }
        case Node::Type::Print: {
            QStringList values;
            for (const Expr &argument : node.parts)
                values.append(list(argument));
            QStringList quoted;
            for (const QString &value : std::as_const(values))
                quoted.append(Py::repr(value));
            std::cerr << qPrintable(u'[' + quoted.join(QStringLiteral(", ")) + u']') << std::endl;
            return values.isEmpty() ? QString(u"") : values.first();
        }
        case Node::Type::ListCountField: {
            const QString name = book->library.fieldKey(list(node.parts.at(0)));
            const std::optional<PyValue> value = book->attribute(name);
            if (!value || (value->kind != PyValue::Kind::List && value->kind != PyValue::Kind::Dict))
                error(QStringLiteral("Field '%1' is either not a field or not a list").arg(name), node.line);
            return QString::number(value->kind == PyValue::Kind::List ? value->list.size() : value->dict.size());
        }
        case Node::Type::FString: {
            const QString text = list(node.parts.at(0));
            static const QRegularExpression braces(QStringLiteral("\\{.*?\\}"));
            QString result;
            qsizetype last = 0;
            QRegularExpressionMatchIterator matches = braces.globalMatch(text);
            while (matches.hasNext()) {
                const QRegularExpressionMatch match = matches.next();
                result += text.mid(last, match.capturedStart() - last);
                Parser parser;
                QSet<QString> names(localFunctions.keyBegin(), localFunctions.keyEnd());
                result += list(parser.program(match.captured().mid(1, match.capturedLength() - 2), &names));
                last = match.capturedEnd();
            }
            return result + text.mid(last);
        }
        case Node::Type::Break:
            throw LoopSignal{ true, {} };
        case Node::Type::Continue:
            throw LoopSignal{ false, {} };
        case Node::Type::Return:
            throw ReturnSignal{ list(node.parts.at(0)) };
        case Node::Type::For:
            return forLoop(node);
        case Node::Type::Range:
            return rangeLoop(node);
        case Node::Type::CompareString:
            return compareString(node);
        case Node::Type::CompareNumeric: {
            const double left = floatOrZero(list(node.parts.at(0)));
            const double right = floatOrZero(list(node.parts.at(1)));
            const QString &op = node.name;
            const bool result = op == QLatin1String("==#") ? left == right
                : op == QLatin1String("!=#")              ? left != right
                : op == QLatin1String("<#")               ? left < right
                : op == QLatin1String("<=#")              ? left <= right
                : op == QLatin1String(">#")               ? left > right
                                                          : left >= right;
            return result ? QStringLiteral("1") : QString(u"");
        }
        case Node::Type::BinaryLogop: {
            const bool result = node.name == QLatin1String("and")
                ? !list(node.parts.at(0)).isEmpty() && !list(node.parts.at(1)).isEmpty()
                : !list(node.parts.at(0)).isEmpty() || !list(node.parts.at(1)).isEmpty();
            return result ? QStringLiteral("1") : QString(u"");
        }
        case Node::Type::UnaryLogop:
            return list(node.parts.at(0)).isEmpty() ? QStringLiteral("1") : QString(u"");
        case Node::Type::BinaryArithop: {
            const double left = floatOrZero(list(node.parts.at(0)));
            const double right = floatOrZero(list(node.parts.at(1)));
            const QString &op = node.name;
            if (op == QLatin1String("/") && right == 0)
                error(QStringLiteral("Error during operator evaluation: operator '/'"), node.line);
            const double answer = op == QLatin1String("+") ? left + right
                : op == QLatin1String("-")                 ? left - right
                : op == QLatin1String("*")                 ? left * right
                                                           : left / right;
            return Py::number(answer);
        }
        case Node::Type::UnaryArithop: {
            const double value = Py::toFloat(list(node.parts.at(0)));
            return Py::number(node.name == QLatin1String("-") ? -value : value);
        }
        case Node::Type::BinaryStringop:
            return list(node.parts.at(0)) + list(node.parts.at(1));
        case Node::Type::LocalFunctionDefine:
            localFunctions.insert(node.name, std::make_shared<Node>(node));
            return QString(u"");
        case Node::Type::LocalFunctionCall:
            return callLocal(node);
        case Node::Type::Arguments:
            for (qsizetype i = 0; i < node.parts.size(); ++i) {
                const Node &argument = *node.parts.at(i).first();
                const QString key = QStringLiteral("*arg_") + QString::number(i);
                locals.insert(argument.name, locals.contains(key) ? locals.value(key) : list(argument.parts.at(0)));
            }
            return QString(u"");
        case Node::Type::Globals: {
            QString result = QString(u"");
            for (const Expr &part : node.parts) {
                const Node &argument = *part.first();
                result = formatter.globals.contains(argument.name) ? formatter.globals.value(argument.name)
                                                                   : list(argument.parts.at(0));
                locals.insert(argument.name, result);
            }
            return result;
        }
        case Node::Type::SetGlobals: {
            QString result = QString(u"");
            for (const Expr &part : node.parts) {
                const Node &argument = *part.first();
                result = locals.contains(argument.name) ? locals.value(argument.name) : list(argument.parts.at(0));
                formatter.globals.insert(argument.name, result);
            }
            return result;
        }
        case Node::Type::With: {
            bool ok = false;
            const int bookId = int(Py::toFloat(list(node.parts.at(0))));
            const Book *other = book->library.book(bookId);
            Q_UNUSED(ok);
            if (!other)
                error(QStringLiteral("Unhandled exception 'No book with id %1'").arg(bookId), node.line);
            const BookProxy *saved = book;
            const BookProxy proxy(book->library, *other);
            book = &proxy;
            try {
                const QString value = list(node.parts.at(1));
                book = saved;
                return value;
            } catch (...) {
                book = saved;
                throw;
            }
        }
        }
        error(QStringLiteral("Internal error evaluating an expression"), node.line);
    }

    QString compareString(const Node &node)
    {
        const QString left = list(node.parts.at(0));
        const QString right = list(node.parts.at(1));
        const QString &op = node.name;
        bool result = false;
        if (op == QLatin1String("in")) {
            result = Py::regex(left, true).match(right).hasMatch();
        } else if (op == QLatin1String("inlist")) {
            const QRegularExpression pattern = Py::regex(left, true);
            for (const QString &item : right.split(u',')) {
                if (!item.trimmed().isEmpty() && pattern.match(item.trimmed()).hasMatch())
                    result = true;
            }
        } else if (op == QLatin1String("inlist_field")) {
            const std::optional<PyValue> value = book->attribute(right);
            if (!value || (value->kind != PyValue::Kind::List && value->kind != PyValue::Kind::Dict))
                error(QStringLiteral("Field '%1' is either not a field or not a list").arg(right), node.line);
            const QRegularExpression pattern = Py::regex(left, true);
            if (value->kind == PyValue::Kind::Dict) {
                for (const auto &[key, item] : value->dict)
                    result = result || pattern.match(key + u':' + item).hasMatch();
            } else {
                for (const QString &item : value->list)
                    result = result || pattern.match(item).hasMatch();
            }
        } else {
            const int order = formatter.compare(left, right);
            result = op == QLatin1String("==") ? order == 0
                : op == QLatin1String("!=")    ? order != 0
                : op == QLatin1String("<")     ? order < 0
                : op == QLatin1String("<=")    ? order <= 0
                : op == QLatin1String(">")     ? order > 0
                                               : order >= 0;
        }
        return result ? QStringLiteral("1") : QString(u"");
    }

    QString forLoop(const Node &node)
    {
        const QString separator = node.hasDefault ? list(node.parts.at(1)) : QStringLiteral(",");
        const QString field = list(node.parts.at(0));
        // getattr(book, field, field): a field's raw value, else the text.
        const std::optional<PyValue> attribute = book->attribute(field);
        const PyValue value = attribute ? *attribute : PyValue::from(field);
        QStringList items;
        if (value.kind == PyValue::Kind::Str) {
            for (const QString &item : value.string.split(separator)) {
                if (!item.trimmed().isEmpty())
                    items.append(item.trimmed());
            }
        } else if (value.kind == PyValue::Kind::List) {
            items = value.list;
        } else if (value.kind == PyValue::Kind::Dict) {
            for (const auto &item : value.dict)
                items.append(item.first);
        } else {
            error(QStringLiteral("Unhandled exception '%1 is not iterable'").arg(value.str()), node.line);
        }
        QString result = QString(u"");
        try {
            for (const QString &item : std::as_const(items)) {
                try {
                    locals.insert(node.name, item);
                    result = list(node.parts.at(2));
                } catch (const LoopSignal &signal) {
                    if (signal.isBreak)
                        throw;
                    result = signal.value;
                }
            }
        } catch (const LoopSignal &signal) {
            result = signal.value;
        }
        return result;
    }

    QString rangeLoop(const Node &node)
    {
        const auto integer = [&](const Expr &expression, const char *what) -> qint64 {
            try {
                return qint64(floatOrZero(list(expression)));
            } catch (const TemplateError &) {
                error(QStringLiteral("for: %1 must be an integer").arg(QLatin1String(what)), node.line);
            }
        };
        const qint64 start = integer(node.parts.at(0), "start");
        const qint64 stop = integer(node.parts.at(1), "stop");
        const qint64 step = integer(node.parts.at(2), "step");
        const qint64 limit = node.hasDefault ? integer(node.parts.at(3), "limit") : 1000;
        if (step == 0)
            error(QStringLiteral("Unhandled exception 'range() arg 3 must not be zero'"), node.line);
        const qint64 length = step > 0 ? (stop > start ? (stop - start + step - 1) / step : 0)
                                       : (start > stop ? (start - stop - step - 1) / -step : 0);
        if (length > limit)
            error(QStringLiteral("for: the range length (%1) is larger than the limit (%2)").arg(length).arg(limit),
                  node.line);
        QString result = QString(u"");
        try {
            for (qint64 i = 0, x = start; i < length; ++i, x += step) {
                try {
                    locals.insert(node.name, QString::number(x));
                    result = list(node.parts.at(4));
                } catch (const LoopSignal &signal) {
                    if (signal.isBreak)
                        throw;
                    result = signal.value;
                }
            }
        } catch (const LoopSignal &signal) {
            result = signal.value;
        }
        return result;
    }

    QString callLocal(const Node &node)
    {
        const NodePtr definition = localFunctions.value(node.name);
        if (!definition)
            error(QStringLiteral("Unknown function %1").arg(node.name), node.line);
        const QList<Expr> &parameters = definition->parts;
        const qsizetype count = parameters.size() - 1;
        if (node.parts.size() > count)
            error(QStringLiteral("Function %1: argument count mismatch -- %2 given, at most %3 required")
                      .arg(node.name).arg(node.parts.size()).arg(count),
                  node.line);
        QHash<QString, QString> newLocals;
        for (qsizetype i = 0; i < count; ++i) {
            const Node &parameter = *parameters.at(i + 1).first();
            newLocals.insert(parameter.name, i < node.parts.size() ? list(node.parts.at(i)) : list(parameter.parts.at(0)));
        }
        const QHash<QString, QString> saved = locals;
        locals = newLocals;
        QString value;
        try {
            value = list(parameters.at(0));
        } catch (const ReturnSignal &signal) {
            value = signal.value;
        } catch (...) {
            locals = saved;
            throw;
        }
        locals = saved;
        return value;
    }
};

// --- The formatter ---------------------------------------------------------------

struct Formatter::Private
{
};

Formatter::Formatter(const Library &library) : library(library), d(std::make_unique<Private>()) {}

Formatter::~Formatter() = default;

int Formatter::compare(const QString &a, const QString &b) const
{
    return library.compare(a, b);
}

QString Formatter::evalProgram(const std::optional<QString> &value, const QString &program)
{
    // A program is parsed once per thread: a composite's template runs for
    // every book.
    thread_local QHash<QString, Expr> parsed;
    auto found = parsed.constFind(program);
    if (found == parsed.cend()) {
        Parser parser;
        if (parsed.size() > 500)
            parsed.clear();
        found = parsed.insert(program, parser.program(program));
    }
    const Expr tree = *found;
    Interpreter interpreter{ *this, book, {}, {} };
    return interpreter.run(tree, value);
}

namespace {

// _explode_format_string(): the spec, and the prefix and suffix between the
// last two bars.
void explode(const QString &spec, QString *format, QString *prefix, QString *suffix)
{
    static const QRegularExpression pattern(QStringLiteral("^(.*)\\|([^\\|]*)\\|(.*)$"),
                                            QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch match = pattern.match(spec);
    if (!match.hasMatch()) {
        *format = spec;
        prefix->clear();
        suffix->clear();
        return;
    }
    *format = match.captured(1);
    *prefix = match.captured(2);
    *suffix = match.captured(3);
}

// The arguments of an old style function call: split at commas but those
// escaped, up to the closing parenthesis.
QStringList scanArguments(const QString &text)
{
    QStringList args;
    qsizetype at = 0;
    while (at < text.size()) {
        if (text.at(at) == u',') {
            args.append(QString(u""));
            ++at;
            continue;
        }
        qsizetype end = -1;
        for (qsizetype i = at; i < text.size() && text.at(i) != u'\n'; ++i) {
            if (text.at(i) == u',' && (i == at || text.at(i - 1) != u'\\')) {
                end = i;
                break;
            }
        }
        if (end >= 0) {
            args.append(text.mid(at, end - at));
            at = end + 1;
            continue;
        }
        const qsizetype close = text.indexOf(u')', at);
        if (close < 0 || text.mid(at, close - at).contains(u'\n'))
            break;
        args.append(text.mid(at, close - at));
        at = close + 1;
    }
    for (QString &arg : args)
        arg.replace(QStringLiteral("\\,"), QStringLiteral(","));
    return args;
}

} // namespace

// TemplateFormatter.format_field(): a function of the spec, then the format
// with the prefix and suffix around a value that is not empty.
static QString formatField(Formatter &formatter, const BookProxy &book, QString value, const QString &fullSpec)
{
    QString spec;
    QString prefix;
    QString suffix;
    explode(fullSpec, &spec, &prefix, &suffix);
    QString display;
    qsizetype p = spec.startsWith(u'\'') ? 0 : spec.indexOf(QStringLiteral(":'"));
    if (p > 0)
        p += 1;
    if (p >= 0 && !spec.isEmpty() && spec.endsWith(u'\'')) {
        value = formatter.evalProgram(value, spec.mid(p + 1, spec.size() - p - 2));
        const qsizetype colon = spec.left(p).indexOf(u':');
        display = colon < 0 ? QString() : spec.left(colon);
    } else {
        p = spec.indexOf(u'(');
        display = spec;
        if (p >= 0 && spec.endsWith(u')')) {
            qsizetype colon = spec.left(p).indexOf(u':');
            if (colon < 0) {
                display.clear();
                colon = 0;
            } else {
                display = spec.left(colon);
                colon += 1;
            }
            const QString name = spec.mid(colon, p - colon).trimmed();
            const auto found = functions().constFind(name);
            if (found == functions().cend())
                return formatter.library.word(QStringLiteral("%s: unknown function")).replace(QStringLiteral("%s"), name);
            const Function &function = *found;
            const QStringList args = function.argCount == 2 ? QStringList{ spec.mid(p + 1, spec.size() - p - 2) }
                                                           : scanArguments(spec.mid(p + 1));
            if ((function.argCount == 1 && (args.size() != 1 || !args.first().isEmpty()))
                || (function.argCount > 1 && function.argCount != args.size() + 1))
                fail(QStringLiteral("Incorrect number of arguments for function %1").arg(name));
            QHash<QString, QString> locals;
            FunctionContext context{ formatter, book, locals };
            value = function.argCount == 1 ? function.evaluate(context, { value })
                                           : function.evaluate(context, QStringList{ value } + args);
            value = Py::strip(value);
        }
    }
    if (!value.isEmpty())
        value = formatter.doFormat(value, display);
    if (value.isEmpty())
        return QString(u"");
    return prefix + value + suffix;
}

QString Formatter::doFormat(const QString &value, const QString &spec)
{
    // _do_format(): the type at the spec's end tells int, float or text.
    if (spec.isEmpty() || value.isEmpty())
        return value;
    const QChar type = spec.back();
    PyValue typed = PyValue::from(value);
    if (QStringView(u"bcdoxXn").contains(type)) {
        try {
            typed = PyValue::from(Py::toInt(value));
        } catch (const TemplateError &) {
            fail(QStringLiteral("format: type %1 requires an integer value, got %2").arg(type).arg(value));
        }
    } else if (QStringView(u"eEfFgGn%").contains(type)) {
        try {
            typed = PyValue::from(Py::toFloat(value));
        } catch (const TemplateError &) {
            fail(QStringLiteral("format: type %1 requires a decimal (float) value, got %2").arg(type).arg(value));
        }
    }
    return Py::format(typed, spec);
}

namespace {

using ValueGetter = std::function<QString(const QString &)>;

// string.Formatter.vformat(): the literal text, and each replacement field
// as format_field() makes it, its spec formatted first.
QString vformat(Formatter &formatter, const BookProxy &book, const QString &text, int depth, const ValueGetter &get)
{
    if (depth < 0)
        fail(QStringLiteral("Max string recursion exceeded"));
    QString result;
    qsizetype at = 0;
    int autoIndex = 0;
    while (at < text.size()) {
        const QChar c = text.at(at);
        if (c == u'}') {
            if (at + 1 < text.size() && text.at(at + 1) == u'}') {
                result += u'}';
                at += 2;
                continue;
            }
            fail(QStringLiteral("Single '}' encountered in format string"));
        }
        if (c != u'{') {
            result += c;
            ++at;
            continue;
        }
        if (at + 1 < text.size() && text.at(at + 1) == u'{') {
            result += u'{';
            at += 2;
            continue;
        }
        // The field, to its matching brace.
        qsizetype end = at + 1;
        int level = 1;
        while (end < text.size()) {
            if (text.at(end) == u'{')
                ++level;
            else if (text.at(end) == u'}' && --level == 0)
                break;
            ++end;
        }
        if (end >= text.size())
            fail(QStringLiteral("expected '}' before end of string"));
        const QString field = text.mid(at + 1, end - at - 1);
        at = end + 1;
        // The name, up to ':' or '!', brackets skipped.
        qsizetype i = 0;
        while (i < field.size()) {
            const QChar f = field.at(i);
            if (f == u'{')
                fail(QStringLiteral("unexpected '{' in field name"));
            if (f == u'[') {
                while (i < field.size() && field.at(i) != u']')
                    ++i;
                ++i;
                continue;
            }
            if (f == u':' || f == u'!')
                break;
            ++i;
        }
        const QString name = field.left(i);
        QChar conversion;
        QString spec;
        if (i < field.size() && field.at(i) == u'!') {
            if (i + 1 >= field.size())
                fail(QStringLiteral("end of string while looking for conversion specifier"));
            conversion = field.at(i + 1);
            if (i + 2 < field.size() && field.at(i + 2) != u':')
                fail(QStringLiteral("expected ':' after conversion specifier"));
            spec = field.mid(i + 3);
        } else if (i < field.size()) {
            spec = field.mid(i + 1);
        }
        // get_value(): a number, or an empty name, is the empty text.
        QString value;
        if (name.isEmpty()) {
            fail(QStringLiteral("Replacement index %1 out of range for positional args tuple").arg(autoIndex++));
        } else if (name.contains(u'.') || name.contains(u'[')) {
            fail(QStringLiteral("'str' object has no attribute '%1'").arg(name.section(u'.', 1)));
        } else {
            bool digits = false;
            name.toInt(&digits);
            value = digits ? QString(u"") : get(name);
        }
        if (conversion == u'r' || conversion == u'a')
            value = Py::repr(value);
        else if (!conversion.isNull() && conversion != u's')
            fail(QStringLiteral("Unknown conversion specifier %1").arg(conversion));
        result += formatField(formatter, book, value, vformat(formatter, book, spec, depth - 1, get));
    }
    return result;
}

} // namespace

QString Formatter::safeFormat(const QString &templateText, const BookProxy &proxy, QString *error)
{
    book = &proxy;
    try {
        QString result;
        if (templateText.startsWith(QLatin1String("program:"))) {
            result = evalProgram(std::nullopt, templateText.mid(8));
        } else if (templateText.startsWith(QLatin1String("python:"))) {
            fail(QStringLiteral("Python templates are not run by Cousoir"));
        } else {
            result = Internal::vformat(*this, proxy, templateText, 2,
                                       [&proxy](const QString &name) { return proxy.value(name); });
            static const QRegularExpression spaces(QStringLiteral("\\s+"), QRegularExpression::UseUnicodePropertiesOption);
            result.replace(spaces, QStringLiteral(" "));
        }
        // strip(' ').
        qsizetype start = 0;
        qsizetype end = result.size();
        while (start < end && result.at(start) == u' ')
            ++start;
        while (end > start && result.at(end - 1) == u' ')
            --end;
        if (error)
            *error = QString();
        return result.mid(start, end - start);
    } catch (const TemplateError &e) {
        if (error)
            *error = e.message.isNull() ? QString(u"") : e.message;
        return {};
    } catch (const LoopSignal &signal) {
        if (error)
            *error = QStringLiteral("%1 outside of for loop").arg(signal.isBreak ? QStringLiteral("break")
                                                                                : QStringLiteral("continue"));
        return {};
    } catch (const ReturnSignal &signal) {
        if (error)
            *error = QString();
        return signal.value;
    }
}

QString Formatter::vformat(const QString &templateText)
{
    const BookProxy &proxy = *book;
    return Internal::vformat(*this, proxy, templateText, 2, [&proxy](const QString &name) { return proxy.value(name); });
}

QString Formatter::evalFormat(const QString &templateText, const QHash<QString, QString> &variables, bool strip)
{
    // EvalFormatter: the variables for fields, by lower case name.
    Formatter inner(library);
    inner.book = book;
    const BookProxy &proxy = *book;
    const ValueGetter get = [&variables](const QString &name) -> QString {
        if (name.isEmpty())
            return QString(u"");
        const QString key = name.toLower();
        return variables.contains(key) ? variables.value(key) : QStringLiteral("No such variable %1").arg(key);
    };
    try {
        QString result;
        if (templateText.startsWith(QLatin1String("program:"))) {
            result = inner.evalProgram(variables.contains(QStringLiteral("$")) ? std::optional(variables.value(QStringLiteral("$")))
                                                                                : std::nullopt,
                                       templateText.mid(8));
        } else {
            result = Internal::vformat(inner, proxy, templateText, 2, get);
            if (strip) {
                static const QRegularExpression spaces(QStringLiteral("\\s+"), QRegularExpression::UseUnicodePropertiesOption);
                result.replace(spaces, QStringLiteral(" "));
            }
        }
        if (strip) {
            while (result.startsWith(u' '))
                result.remove(0, 1);
            while (result.endsWith(u' '))
                result.chop(1);
        }
        return result;
    } catch (const TemplateError &e) {
        return QStringLiteral("EVAL ") + e.message;
    }
}

} // namespace CalibreTemplate::Internal
