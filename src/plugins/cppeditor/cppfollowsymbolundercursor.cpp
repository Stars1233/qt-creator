// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "cppfollowsymbolundercursor.h"

#include "cppeditorwidget.h"
#include "cppmodelmanager.h"
#include "cpptoolsreuse.h"
#include "cppvirtualfunctionassistprovider.h"
#include "functionutils.h"
#include "symbolfinder.h"

#include <cplusplus/ASTPath.h>
#include <cplusplus/BackwardsScanner.h>
#include <cplusplus/ExpressionUnderCursor.h>
#include <cplusplus/ResolveExpression.h>
#include <cplusplus/SimpleLexer.h>
#include <cplusplus/TypeOfExpression.h>
#include <texteditor/textdocumentlayout.h>
#include <utils/algorithm.h>
#include <utils/textutils.h>
#include <utils/qtcassert.h>

#include <QList>
#include <QSet>

using namespace CPlusPlus;
using namespace TextEditor;
using namespace Utils;

namespace CppEditor {

namespace {

class VirtualFunctionHelper {
public:
    VirtualFunctionHelper(TypeOfExpression &typeOfExpression,
                          Scope *scope,
                          const Document::Ptr &document,
                          const Snapshot &snapshot,
                          SymbolFinder *symbolFinder);
    VirtualFunctionHelper() = delete;

    bool canLookupVirtualFunctionOverrides(Function *function);

    /// Returns != 0 if canLookupVirtualFunctionOverrides() succeeded.
    Class *staticClassOfFunctionCallExpression() const
    { return m_staticClassOfFunctionCallExpression; }

private:
    Q_DISABLE_COPY(VirtualFunctionHelper)

    Class *staticClassOfFunctionCallExpression_internal() const;

private:
    // Provided
    const Document::Ptr m_expressionDocument;
    Scope *m_scope;
    const Document::Ptr &m_document;
    const Snapshot &m_snapshot;
    TypeOfExpression &m_typeOfExpression;
    SymbolFinder *m_finder;

    // Determined
    ExpressionAST *m_baseExpressionAST = nullptr;
    Function *m_function = nullptr;
    int m_accessTokenKind = 0;
    Class *m_staticClassOfFunctionCallExpression = nullptr; // Output
};

VirtualFunctionHelper::VirtualFunctionHelper(TypeOfExpression &typeOfExpression,
                                             Scope *scope,
                                             const Document::Ptr &document,
                                             const Snapshot &snapshot,
                                             SymbolFinder *finder)
    : m_expressionDocument(typeOfExpression.context().expressionDocument())
    , m_scope(scope)
    , m_document(document)
    , m_snapshot(snapshot)
    , m_typeOfExpression(typeOfExpression)
    , m_finder(finder)
{
    if (ExpressionAST *expressionAST = typeOfExpression.expressionAST()) {
        if (CallAST *callAST = expressionAST->asCall()) {
            if (ExpressionAST *baseExpressionAST = callAST->base_expression)
                m_baseExpressionAST = baseExpressionAST;
        }
    }
}

bool VirtualFunctionHelper::canLookupVirtualFunctionOverrides(Function *function)
{
    m_function = function;

    if (!m_document || m_snapshot.isEmpty() || !m_function || !m_scope)
        return false;

    if (m_scope->asClass() && m_function->isPureVirtual()) {
        m_staticClassOfFunctionCallExpression = m_scope->asClass();
        return true;
    }

    if (!m_baseExpressionAST || !m_expressionDocument
            || m_scope->asClass() || m_scope->asFunction()) {
        return false;
    }

    bool result = false;

    if (IdExpressionAST *idExpressionAST = m_baseExpressionAST->asIdExpression()) {
        NameAST *name = idExpressionAST->name;
        const bool nameIsQualified = name && name->asQualifiedName();
        result = !nameIsQualified && Internal::FunctionUtils::isVirtualFunction(
                    function, LookupContext(m_document, m_snapshot));
    } else if (MemberAccessAST *memberAccessAST = m_baseExpressionAST->asMemberAccess()) {
        NameAST *name = memberAccessAST->member_name;
        const bool nameIsQualified = name && name->asQualifiedName();
        if (!nameIsQualified && Internal::FunctionUtils::isVirtualFunction(
                    function, LookupContext(m_document, m_snapshot))) {
            TranslationUnit *unit = m_expressionDocument->translationUnit();
            QTC_ASSERT(unit, return false);
            m_accessTokenKind = unit->tokenKind(memberAccessAST->access_token);

            if (m_accessTokenKind == T_ARROW) {
                result = true;
            } else if (m_accessTokenKind == T_DOT) {
                const QList<LookupItem> items = m_typeOfExpression.reference(
                            memberAccessAST->base_expression, m_document, m_scope);
                if (!items.isEmpty()) {
                    const LookupItem item = items.first();
                    if (Symbol *declaration = item.declaration())
                        result = declaration->type()->asReferenceType();
                }
            }
        }
    }

    if (!result)
        return false;
    return (m_staticClassOfFunctionCallExpression = staticClassOfFunctionCallExpression_internal());
}

/// For "f()" in "class C { void g() { f(); };" return class C.
/// For "c->f()" in "{ C *c; c->f(); }" return class C.
Class *VirtualFunctionHelper::staticClassOfFunctionCallExpression_internal() const
{
    if (!m_finder)
        return nullptr;

    Class *result = nullptr;

    if (m_baseExpressionAST->asIdExpression()) {
        for (Scope *s = m_scope; s ; s = s->enclosingScope()) {
            if (Function *function = s->asFunction()) {
                result = m_finder->findMatchingClassDeclaration(function, m_snapshot);
                break;
            }
        }
    } else if (MemberAccessAST *memberAccessAST = m_baseExpressionAST->asMemberAccess()) {
        QTC_ASSERT(m_accessTokenKind == T_ARROW || m_accessTokenKind == T_DOT, return result);
        const QList<LookupItem> items = m_typeOfExpression(memberAccessAST->base_expression,
                                                           m_expressionDocument, m_scope);
        ResolveExpression resolveExpression(m_typeOfExpression.context());
        ClassOrNamespace *binding = resolveExpression.baseExpression(items, m_accessTokenKind);
        if (binding) {
            if (Class *klass = binding->rootClass()) {
                result = klass;
            } else {
                const QList<Symbol *> symbols = binding->symbols();
                if (!symbols.isEmpty()) {
                    Symbol * const first = symbols.first();
                    if (first->asForwardClassDeclaration())
                        result = m_finder->findMatchingClassDeclaration(first, m_snapshot);
                }
            }
        }
    }

    return result;
}

Link findMacroLink_helper(const QByteArray &name, Document::Ptr doc, const Snapshot &snapshot,
                          QSet<QString> *processed)
{
    if (doc && !name.startsWith('<') && Utils::insert(*processed, doc->filePath().path())) {
        for (const Macro &macro : doc->definedMacros()) {
            if (macro.name() == name) {
                Link link;
                link.targetFilePath = macro.filePath();
                link.target.line = macro.line();
                return link;
            }
        }

        const QList<Document::Include> includes = doc->resolvedIncludes();
        for (int index = includes.size() - 1; index != -1; --index) {
            const Document::Include &i = includes.at(index);
            Link link = findMacroLink_helper(name, snapshot.document(i.resolvedFileName()),
                                             snapshot, processed);
            if (link.hasValidTarget())
                return link;
        }
    }

    return Link();
}

Link findMacroLink(const QByteArray &name, const Document::Ptr &doc)
{
    if (!name.isEmpty()) {
        if (doc) {
            const Snapshot snapshot = CppModelManager::snapshot();
            QSet<QString> processed;
            return findMacroLink_helper(name, doc, snapshot, &processed);
        }
    }

    return Link();
}

/// Considers also forward declared templates.
static bool isForwardClassDeclaration(Type *type)
{
    if (!type)
        return false;

    if (type->asForwardClassDeclarationType()) {
        return true;
    } else if (Template *templ = type->asTemplateType()) {
        if (Symbol *declaration = templ->declaration()) {
            if (declaration->asForwardClassDeclaration())
                return true;
        }
    }

    return false;
}

inline LookupItem skipForwardDeclarations(const QList<LookupItem> &resolvedSymbols)
{
    QList<LookupItem> candidates = resolvedSymbols;

    LookupItem result = candidates.first();
    const FullySpecifiedType ty = result.type().simplified();

    if (isForwardClassDeclaration(ty.type())) {
        while (!candidates.isEmpty()) {
            LookupItem r = candidates.takeFirst();

            if (!isForwardClassDeclaration(r.type().type())) {
                result = r;
                break;
            }
        }
    }

    if (ty->asObjCForwardClassDeclarationType()) {
        while (!candidates.isEmpty()) {
            LookupItem r = candidates.takeFirst();

            if (!r.type()->asObjCForwardClassDeclarationType()) {
                result = r;
                break;
            }
        }
    }

    if (ty->asObjCForwardProtocolDeclarationType()) {
        while (!candidates.isEmpty()) {
            LookupItem r = candidates.takeFirst();

            if (!r.type()->asObjCForwardProtocolDeclarationType()) {
                result = r;
                break;
            }
        }
    }

    return result;
}

Link attemptDeclDef(const QTextCursor &cursor, Snapshot snapshot,
                    const Document::Ptr &document,
                    SymbolFinder *symbolFinder)
{
    Link result;
    QTC_ASSERT(document, return result);

    snapshot.insert(document);

    QList<AST *> path = ASTPath(document)(cursor);

    if (path.size() < 5)
        return result;

    NameAST *name = path.last()->asName();
    if (!name)
        return result;

    if (QualifiedNameAST *qName = path.at(path.size() - 2)->asQualifiedName()) {
        // TODO: check which part of the qualified name we're on
        if (qName->unqualified_name != name)
            return result;
    }

    for (int i = path.size() - 1; i != -1; --i) {
        AST *node = path.at(i);

        if (node->asParameterDeclaration() != nullptr)
            return result;
    }

    AST *declParent = nullptr;
    DeclaratorAST *decl = nullptr;
    for (int i = path.size() - 2; i > 0; --i) {
        if ((decl = path.at(i)->asDeclarator()) != nullptr) {
            declParent = path.at(i - 1);
            break;
        }
    }
    if (!decl || !declParent)
        return result;

    Symbol *target = nullptr;
    if (FunctionDefinitionAST *funDef = declParent->asFunctionDefinition()) {
        QList<Declaration *> candidates =
                symbolFinder->findMatchingDeclaration(LookupContext(document, snapshot),
                                                        funDef->symbol);
        if (!candidates.isEmpty()) // TODO: improve disambiguation
            target = candidates.first();
    } else if (const SimpleDeclarationAST * const simpleDecl = declParent->asSimpleDeclaration()) {
        FunctionDeclaratorAST *funcDecl = nullptr;
        if (decl->postfix_declarator_list && decl->postfix_declarator_list->value)
            funcDecl = decl->postfix_declarator_list->value->asFunctionDeclarator();
        if (funcDecl)
            target = symbolFinder->findMatchingDefinition(funcDecl->symbol, snapshot, false);
        else if (simpleDecl->symbols)
            target = symbolFinder->findMatchingVarDefinition(simpleDecl->symbols->value, snapshot);
    }

    if (target) {
        result = target->toLink();

        int startLine, startColumn, endLine, endColumn;
        document->translationUnit()->getTokenPosition(name->firstToken(), &startLine,
                                                           &startColumn);
        document->translationUnit()->getTokenEndPosition(name->lastToken() - 1, &endLine,
                                                         &endColumn);

        QTextDocument *textDocument = cursor.document();
        result.linkTextStart =
                textDocument->findBlockByNumber(startLine - 1).position() + startColumn - 1;
        result.linkTextEnd =
                textDocument->findBlockByNumber(endLine - 1).position() + endColumn - 1;
    }

    return result;
}

Symbol *findDefinition(Symbol *symbol, const Snapshot &snapshot, SymbolFinder *symbolFinder)
{
    if (symbol->asFunction())
        return nullptr; // symbol is a function definition.

    if (!symbol->type()->asFunctionType())
        return nullptr; // not a function declaration

    return symbolFinder->findMatchingDefinition(symbol, snapshot, false);
}

bool maybeAppendArgumentOrParameterList(QString *expression, const QTextCursor &textCursor)
{
    QTC_ASSERT(expression, return false);
    QTextDocument *textDocument = textCursor.document();
    QTC_ASSERT(textDocument, return false);

    // Skip white space
    QTextCursor cursor(textCursor);
    while (textDocument->characterAt(cursor.position()).isSpace()
           && cursor.movePosition(QTextCursor::NextCharacter)) {
    }

    // Find/Include "(arg1, arg2, ...)"
    if (textDocument->characterAt(cursor.position()) == QLatin1Char('(')) {
        if (TextBlockUserData::findNextClosingParenthesis(&cursor, true)) {
            expression->append(cursor.selectedText());
            return true;
        }
    }

    return false;
}

bool isCursorOnTrailingReturnType(const QList<AST *> &astPath)
{
    if (astPath.size() < 3)
        return false;
    for (auto it = astPath.cend() - 3, begin = astPath.cbegin(); it >= begin; --it) {
        if (!(*it)->asTrailingReturnType())
            continue;
        const auto nextIt = it + 1;
        const auto nextNextIt = nextIt + 1;
        return (*nextIt)->asNamedTypeSpecifier()
               && ((*nextNextIt)->asSimpleName()
                   || (*nextNextIt)->asQualifiedName()
                   || (*nextNextIt)->asTemplateId());
    }
    return false;
}

void maybeFixExpressionInTrailingReturnType(QString *expression,
                                            const QTextCursor &textCursor,
                                            const Document::Ptr documentFromSemanticInfo)
{
    QTC_ASSERT(expression, return);

    if (!documentFromSemanticInfo)
        return;

    const QString arrow = QLatin1String("->");
    const int arrowPosition = expression->lastIndexOf(arrow);
    if (arrowPosition != -1) {
        ASTPath astPathFinder(documentFromSemanticInfo);
        const QList<AST *> astPath = astPathFinder(textCursor);

        if (isCursorOnTrailingReturnType(astPath))
            *expression = expression->mid(arrowPosition + arrow.size()).trimmed();
    }
}

QString expressionUnderCursorAsString(const QTextCursor &textCursor,
                                      const Document::Ptr documentFromSemanticInfo,
                                      const LanguageFeatures &features)
{
    ExpressionUnderCursor expressionUnderCursor(features);
    QString expression = expressionUnderCursor(textCursor);

    if (!maybeAppendArgumentOrParameterList(&expression, textCursor))
        maybeFixExpressionInTrailingReturnType(&expression, textCursor, documentFromSemanticInfo);

    return expression;
}

} // anonymous namespace

FollowSymbolUnderCursor::FollowSymbolUnderCursor()
    : m_virtualFunctionAssistProvider(new VirtualFunctionAssistProvider)
{
}

static int skipMatchingParentheses(const Tokens &tokens, int idx, int initialDepth)
{
    int j = idx;
    int depth = initialDepth;

    for (; j < tokens.size(); ++j) {
        if (tokens.at(j).is(T_LPAREN)) {
            ++depth;
        } else if (tokens.at(j).is(T_RPAREN)) {
            if (!--depth)
                break;
        }
    }

    return j;
}

void FollowSymbolUnderCursor::findLink(
        const CursorInEditor &data,
        const Utils::LinkHandler &processLinkCallback,
        bool resolveTarget,
        const Snapshot &theSnapshot,
        const Document::Ptr &documentFromSemanticInfo,
        SymbolFinder *symbolFinder,
        bool inNextSplit)
{
    Link link;

    QTextCursor cursor = data.cursor();
    QTextDocument *document = cursor.document();
    if (!document)
        return processLinkCallback(link);

    int line = 0;
    int column = 0;
    Utils::Text::convertPosition(document, cursor.position(), &line, &column);

    Snapshot snapshot = theSnapshot;

    // Move to end of identifier
    QTextCursor tc = cursor;
    QChar ch = document->characterAt(tc.position());
    while (isValidIdentifierChar(ch)) {
        tc.movePosition(QTextCursor::NextCharacter);
        ch = document->characterAt(tc.position());
    }

    // Try to macth decl/def. For this we need the semantic doc with the AST.
    if (documentFromSemanticInfo
            && documentFromSemanticInfo->translationUnit()
            && documentFromSemanticInfo->translationUnit()->ast()) {
        int pos = tc.position();
        while (document->characterAt(pos).isSpace())
            ++pos;
        const QChar ch = document->characterAt(pos);
        if (ch == '(' || ch == ';') {
            link = attemptDeclDef(cursor, snapshot, documentFromSemanticInfo, symbolFinder);
            if (link.hasValidLinkText())
                return processLinkCallback(link);
        }
    }

    // Try to find a signal or slot inside SIGNAL() or SLOT()
    int beginOfToken = 0;
    int endOfToken = 0;

    const LanguageFeatures features = documentFromSemanticInfo
            ? documentFromSemanticInfo->languageFeatures()
            : LanguageFeatures::defaultFeatures();

    SimpleLexer tokenize;
    tokenize.setLanguageFeatures(features);
    const QString blockText = cursor.block().text();
    const Tokens tokens = tokenize(blockText, BackwardsScanner::previousBlockState(cursor.block()));

    bool recognizedQtMethod = false;

    for (int i = 0; i < tokens.size(); ++i) {
        const Token &tk = tokens.at(i);

        if (column >= tk.utf16charsBegin() && column < tk.utf16charsEnd()) {
            int closingParenthesisPos = tokens.size();
            if (i >= 2 && tokens.at(i).is(T_IDENTIFIER) && tokens.at(i - 1).is(T_LPAREN)
                && (tokens.at(i - 2).is(T_SIGNAL) || tokens.at(i - 2).is(T_SLOT))) {

                // token[i] == T_IDENTIFIER
                // token[i + 1] == T_LPAREN
                // token[.....] == ....
                // token[i + n] == T_RPAREN

                if (i + 1 < tokens.size() && tokens.at(i + 1).is(T_LPAREN))
                    closingParenthesisPos = skipMatchingParentheses(tokens, i - 1, 0);
            } else if ((i > 3 && tk.is(T_LPAREN) && tokens.at(i - 1).is(T_IDENTIFIER)
                        && tokens.at(i - 2).is(T_LPAREN)
                    && (tokens.at(i - 3).is(T_SIGNAL) || tokens.at(i - 3).is(T_SLOT)))) {

                // skip until the closing parentheses of the SIGNAL/SLOT macro
                closingParenthesisPos = skipMatchingParentheses(tokens, i, 1);
                --i; // point to the token before the opening parenthesis
            }

            if (closingParenthesisPos < tokens.size()) {
                QTextBlock block = cursor.block();

                beginOfToken = block.position() + tokens.at(i).utf16charsBegin();
                endOfToken = block.position() + tokens.at(i).utf16charsEnd();

                tc.setPosition(block.position() + tokens.at(closingParenthesisPos).utf16charsEnd());
                recognizedQtMethod = true;
            }
            break;
        }
    }

    // Check if we're on an operator declaration or definition.
    if (!recognizedQtMethod && documentFromSemanticInfo) {
        bool cursorRegionReached = false;
        for (int i = 0; i < tokens.size(); ++i) {
            const Token &tk = tokens.at(i);

            // In this case we want to look at one token before the current position to recognize
            // an operator if the cursor is inside the actual operator: operator[$]
            if (column >= tk.utf16charsBegin() && column <= tk.utf16charsEnd()) {
                cursorRegionReached = true;
                if (tk.is(T_OPERATOR)) {
                    link = attemptDeclDef(cursor, theSnapshot,
                                          documentFromSemanticInfo, symbolFinder);
                    if (link.hasValidLinkText())
                        return processLinkCallback(link);
                } else if (tk.isPunctuationOrOperator() && i > 0 && tokens.at(i - 1).is(T_OPERATOR)) {
                    QTextCursor c = cursor;
                    c.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor,
                                   column - tokens.at(i - 1).utf16charsBegin());
                    link = attemptDeclDef(c, theSnapshot, documentFromSemanticInfo, symbolFinder);
                    if (link.hasValidLinkText())
                        return processLinkCallback(link);
                }
            } else if (cursorRegionReached) {
                break;
            }
        }
    }

    CppEditorWidget *editorWidget = data.editorWidget();
    if (!editorWidget)
        return processLinkCallback(link);

    // Now we prefer the doc from the snapshot with macros expanded.
    Document::Ptr doc = snapshot.document(data.filePath());
    if (!doc) {
        doc = documentFromSemanticInfo;
        if (!doc)
            return processLinkCallback(link);
    }

    if (!recognizedQtMethod) {
        const QTextBlock block = tc.block();
        int pos = cursor.positionInBlock();
        QChar ch = document->characterAt(cursor.position());
        if (pos > 0 && !isValidIdentifierChar(ch))
            --pos; // positionInBlock points to a delimiter character.
        const Token tk = SimpleLexer::tokenAt(block.text(), pos,
                                              BackwardsScanner::previousBlockState(block),
                                              features);

        beginOfToken = block.position() + tk.utf16charsBegin();
        endOfToken = block.position() + tk.utf16charsEnd();

        // Handle include directives
        if (tk.is(T_STRING_LITERAL) || tk.is(T_ANGLE_STRING_LITERAL)) {
            const int lineno = cursor.blockNumber() + 1;
            const QList<Document::Include> includes = doc->resolvedIncludes();
            for (const Document::Include &incl : includes) {
                if (incl.line() == lineno) {
                    link.targetFilePath = incl.resolvedFileName();
                    link.linkTextStart = beginOfToken + 1;
                    link.linkTextEnd = endOfToken - 1;
                    processLinkCallback(link);
                    return;
                }
            }
        }

        if (tk.isNot(T_IDENTIFIER) && !tk.isQtKeyword())
            return processLinkCallback(link);

        tc.setPosition(endOfToken);
    }

    // Handle macro uses
    const Macro *macro = doc->findMacroDefinitionAt(line);
    if (macro) {
        QTextCursor macroCursor = cursor;
        const QByteArray name = identifierUnderCursor(&macroCursor).toUtf8();
        if (macro->name() == name)
            return processLinkCallback(link); //already on definition!
    } else if (const Document::MacroUse *use = doc->findMacroUseAt(endOfToken - 1)) {
        const FilePath filePath = use->macro().filePath();
        if (filePath.path() == CppModelManager::editorConfigurationFileName().path()) {
            editorWidget->showPreProcessorWidget();
        } else if (filePath.path() != CppModelManager::configurationFileName().path()) {
            const Macro &macro = use->macro();
            link.targetFilePath = macro.filePath();
            link.target.line = macro.line();
            link.linkTextStart = use->utf16charsBegin();
            link.linkTextEnd = use->utf16charsEnd();
        }
        processLinkCallback(link);
        return;
    }

    // Find the last symbol up to the cursor position
    Scope *scope = doc->scopeAt(line, column);
    if (!scope)
        return processLinkCallback(link);

    // Evaluate the type of the expression under the cursor
    QTC_CHECK(document == tc.document());
    const QString expression = expressionUnderCursorAsString(tc, documentFromSemanticInfo,
                                                             features);
    const QSharedPointer<TypeOfExpression> typeOfExpression(new TypeOfExpression);
    typeOfExpression->init(doc, snapshot);
    // make possible to instantiate templates
    typeOfExpression->setExpandTemplates(true);
    const QList<LookupItem> resolvedSymbols =
            typeOfExpression->reference(expression.toUtf8(), scope, TypeOfExpression::Preprocess);

    if (!resolvedSymbols.isEmpty()) {
        LookupItem result = skipForwardDeclarations(resolvedSymbols);

        for (const LookupItem &r : resolvedSymbols) {
            if (Symbol *d = r.declaration()) {
                if (d->asDeclaration() || d->asFunction()) {
                    if (data.filePath() == d->filePath()) {
                        if (line == d->line() && column >= d->column()) {
                            // TODO: check the end
                            result = r; // take the symbol under cursor.
                            break;
                        }
                    }
                } else if (d->asUsingDeclaration()) {
                    int tokenBeginLineNumber = 0;
                    int tokenBeginColumnNumber = 0;
                    Utils::Text::convertPosition(document, beginOfToken, &tokenBeginLineNumber,
                                                 &tokenBeginColumnNumber);
                    if (tokenBeginLineNumber > d->line()
                            || (tokenBeginLineNumber == d->line()
                                && tokenBeginColumnNumber + 1 >= d->column())) {
                        result = r; // take the symbol under cursor.
                        break;
                    }
                }
            }
        }

        if (Symbol *symbol = result.declaration()) {
            Symbol *def = nullptr;

            if (resolveTarget) {
                // Consider to show a pop-up displaying overrides for the function
                Function *function = symbol->type()->asFunctionType();
                VirtualFunctionHelper helper(*typeOfExpression, scope, doc, snapshot, symbolFinder);

                if (helper.canLookupVirtualFunctionOverrides(function)) {
                    VirtualFunctionAssistProvider::Parameters params;
                    params.function = function;
                    params.staticClass = helper.staticClassOfFunctionCallExpression();
                    params.typeOfExpression = typeOfExpression;
                    params.snapshot = snapshot;
                    params.cursorPosition = cursor.position();
                    params.openInNextSplit = inNextSplit;

                    if (m_virtualFunctionAssistProvider->configure(params)) {
                        editorWidget->invokeTextEditorWidgetAssist(
                                    FollowSymbol,m_virtualFunctionAssistProvider.data());
                        m_virtualFunctionAssistProvider->clearParams();
                    }

                    // Ensure a valid link text, so the symbol name will be underlined on Ctrl+Hover.
                    Link link;
                    link.linkTextStart = beginOfToken;
                    link.linkTextEnd = endOfToken;
                    processLinkCallback(link);
                    return;
                }

                Symbol *lastVisibleSymbol = doc->lastVisibleSymbolAt(line, column);

                def = findDefinition(symbol, snapshot, symbolFinder);

                if (def == lastVisibleSymbol)
                    def = nullptr; // jump to declaration then.

                if (symbol->asForwardClassDeclaration()) {
                    def = symbolFinder->findMatchingClassDeclaration(symbol, snapshot);
                } else if (Template *templ = symbol->asTemplate()) {
                    if (Symbol *declaration = templ->declaration()) {
                        if (declaration->asForwardClassDeclaration())
                            def = symbolFinder->findMatchingClassDeclaration(declaration, snapshot);
                    }
                }

            }

            link = (def ? def : symbol)->toLink();
            link.linkTextStart = beginOfToken;
            link.linkTextEnd = endOfToken;
            processLinkCallback(link);
            return;
        }
    }

    // Handle macro uses
    QTextCursor macroCursor = cursor;
    const QByteArray name = identifierUnderCursor(&macroCursor).toUtf8();
    link = findMacroLink(name, documentFromSemanticInfo);
    if (link.hasValidTarget()) {
        link.linkTextStart = macroCursor.selectionStart();
        link.linkTextEnd = macroCursor.selectionEnd();
        processLinkCallback(link);
        return;
    }

    processLinkCallback(Link());
}

void FollowSymbolUnderCursor::switchDeclDef(
        const CursorInEditor &data,
        const Utils::LinkHandler &processLinkCallback,
        const CPlusPlus::Snapshot &snapshot,
        const CPlusPlus::Document::Ptr &documentFromSemanticInfo,
        SymbolFinder *symbolFinder)
{
    if (!documentFromSemanticInfo) {
        processLinkCallback({});
        return;
    }

    // Find function declaration or definition under cursor
    Function *functionDefinitionSymbol = nullptr;
    Symbol *functionDeclarationSymbol = nullptr;
    Symbol *declarationSymbol = nullptr;

    ASTPath astPathFinder(documentFromSemanticInfo);
    const QList<AST *> astPath = astPathFinder(data.cursor());

    for (AST *ast : astPath) {
        if (FunctionDefinitionAST *functionDefinitionAST = ast->asFunctionDefinition()) {
            if ((functionDefinitionSymbol = functionDefinitionAST->symbol))
                break; // Function definition found!
        } else if (SimpleDeclarationAST *simpleDeclaration = ast->asSimpleDeclaration()) {
            if (List<Symbol *> *symbols = simpleDeclaration->symbols) {
                if (Symbol *symbol = symbols->value) {
                    if (symbol->asDeclaration()) {
                        declarationSymbol = symbol;
                        if (symbol->type()->asFunctionType()) {
                            functionDeclarationSymbol = symbol;
                            break; // Function declaration found!
                        }
                    }
                }
            }
        }
    }

    // Link to function definition/declaration
    Utils::Link symbolLink;
    if (functionDeclarationSymbol) {
        Symbol *symbol
            = symbolFinder->findMatchingDefinition(functionDeclarationSymbol, snapshot, false);
        if (symbol)
            symbolLink = symbol->toLink();
    } else if (declarationSymbol) {
        Symbol *symbol = symbolFinder->findMatchingVarDefinition(declarationSymbol, snapshot);
        if (symbol)
            symbolLink = symbol->toLink();
    } else if (functionDefinitionSymbol) {
        LookupContext context(documentFromSemanticInfo, snapshot);
        ClassOrNamespace *binding = context.lookupType(functionDefinitionSymbol);
        const QList<LookupItem> declarations
            = context.lookup(functionDefinitionSymbol->name(),
                             functionDefinitionSymbol->enclosingScope());

        QList<Symbol *> best;
        for (const LookupItem &r : declarations) {
            if (Symbol *decl = r.declaration()) {
                if (Function *funTy = decl->type()->asFunctionType()) {
                    if (funTy->match(functionDefinitionSymbol)) {
                        if (decl != functionDefinitionSymbol && binding == r.binding())
                            best.prepend(decl);
                        else
                            best.append(decl);
                    }
                }
            }
        }

        if (best.isEmpty())
            return;
        symbolLink = best.first()->toLink();
    }
    processLinkCallback(symbolLink);
}

QSharedPointer<VirtualFunctionAssistProvider> FollowSymbolUnderCursor::virtualFunctionAssistProvider()
{
    return m_virtualFunctionAssistProvider;
}

void FollowSymbolUnderCursor::setVirtualFunctionAssistProvider(
        const QSharedPointer<VirtualFunctionAssistProvider> &provider)
{
    m_virtualFunctionAssistProvider = provider;
}

} // CppEditor
