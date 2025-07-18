// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "followsymbol_switchmethoddecldef_test.h"

#include "clangdsettings.h"
#include "cppcodemodelsettings.h"
#include "cppeditorwidget.h"
#include "cppfollowsymbolundercursor.h"
#include "cppmodelmanager.h"
#include "cpptoolstestcase.h"
#include "cppvirtualfunctionassistprovider.h"
#include "cppvirtualfunctionproposalitem.h"

#include <projectexplorer/kitmanager.h>
#include <projectexplorer/projectexplorer.h>

#include <texteditor/codeassist/assistinterface.h>
#include <texteditor/codeassist/asyncprocessor.h>
#include <texteditor/codeassist/genericproposalmodel.h>
#include <texteditor/codeassist/iassistprocessor.h>
#include <texteditor/codeassist/iassistproposal.h>
#include <texteditor/texteditor.h>

#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/idocument.h>

#include <utils/environment.h>
#include <utils/fileutils.h>

#include <QDebug>
#include <QDir>
#include <QTest>

//
// The following "non-latin1" code points are used in the tests:
//
//   U+00FC  - 2 code units in UTF8, 1 in UTF16 - LATIN SMALL LETTER U WITH DIAERESIS
//   U+4E8C  - 3 code units in UTF8, 1 in UTF16 - CJK UNIFIED IDEOGRAPH-4E8C
//   U+10302 - 4 code units in UTF8, 2 in UTF16 - OLD ITALIC LETTER KE
//

#define UNICODE_U00FC "\xc3\xbc"
#define UNICODE_U4E8C "\xe4\xba\x8c"
#define UNICODE_U10302 "\xf0\x90\x8c\x82"
#define TEST_UNICODE_IDENTIFIER UNICODE_U00FC UNICODE_U4E8C UNICODE_U10302

/*!
    Tests for Follow Symbol Under Cursor and Switch Between Function Declaration/Definition

    Section numbers refer to

        Working Draft, Standard for Programming Language C++
        Document Number: N3242=11-0012

    You can find potential test code for Follow Symbol Under Cursor on the bottom of this file.
 */

using namespace CPlusPlus;
using namespace TextEditor;
using namespace Core;
using namespace ProjectExplorer;
using namespace Utils;

class OverrideItem
{
public:
    OverrideItem() : line(0) {}
    OverrideItem(const QString &text, int line = 0) : text(text), line(line) {}

    bool isValid() { return line != 0; }
    QByteArray toByteArray() const
    {
        return "OverrideItem(" + text.toLatin1() + ", " + QByteArray::number(line) + ')';
    }

    friend bool operator==(const OverrideItem &lhs, const OverrideItem &rhs)
    {
        return lhs.text == rhs.text && lhs.line == rhs.line;
    }

    QString text;
    int line;
};
typedef QList<OverrideItem> OverrideItemList;
Q_DECLARE_METATYPE(OverrideItem)

QT_BEGIN_NAMESPACE
namespace QTest {
template<> char *toString(const OverrideItem &data)
{
    return qstrdup(data.toByteArray().constData());
}
}

QDebug &operator<<(QDebug &d, const OverrideItem &data)
{
    d << data.toByteArray();
    return d;
}
QT_END_NAMESPACE

typedef QByteArray _;

namespace CppEditor::Internal::Tests {

/// A fake virtual functions assist provider that runs processor->perform() already in configure()
class VirtualFunctionTestAssistProvider : public VirtualFunctionAssistProvider
{
public:
    VirtualFunctionTestAssistProvider(CppEditorWidget *editorWidget)
        : m_editorWidget(editorWidget)
    {}

    // Invoke the processor already here to calculate the proposals. Return false in order to
    // indicate that configure failed, so the actual code assist invocation leading to a pop-up
    // will not happen.
    bool configure(const VirtualFunctionAssistProvider::Parameters &params) override
    {
        VirtualFunctionAssistProvider::configure(params);

        std::unique_ptr<AssistInterface> assistInterface
            = m_editorWidget->createAssistInterface(FollowSymbol, ExplicitlyInvoked);
        const QScopedPointer<AsyncProcessor> processor(
            dynamic_cast<AsyncProcessor *>(createProcessor(assistInterface.get())));
        processor->setupAssistInterface(std::move(assistInterface));
        const QScopedPointer<IAssistProposal> immediateProposal(processor->immediateProposal());
        const QScopedPointer<IAssistProposal> finalProposal(processor->performAsync());

        VirtualFunctionAssistProvider::clearParams();

        m_immediateItems = itemList(immediateProposal->model());
        m_finalItems = itemList(finalProposal->model());

        return false;
    }

    static OverrideItemList itemList(ProposalModelPtr imodel)
    {
        OverrideItemList result;
        GenericProposalModelPtr model = imodel.staticCast<GenericProposalModel>();
        if (!model)
            return result;

        // Mimic relevant GenericProposalWidget::showProposal() calls
        model->removeDuplicates();
        model->reset();
        if (model->isSortable(QString()))
            model->sort(QString());

        for (int i = 0, size = model->size(); i < size; ++i) {
            VirtualFunctionProposalItem *item
                = dynamic_cast<VirtualFunctionProposalItem *>(model->proposalItem(i));

            const QString text = model->text(i);
            const int line = item->link().target.line;
//            Uncomment for updating/generating reference data:
//            qDebug("<< OverrideItem(QLatin1String(\"%s\"), %d)", qPrintable(text), line);
            result << OverrideItem(text, line);
        }

        return result;
    }

public:
    OverrideItemList m_immediateItems;
    OverrideItemList m_finalItems;

private:
    CppEditorWidget *m_editorWidget;
};

QList<TestDocumentPtr> singleDocument(const QByteArray &source)
{
    return {CppTestDocument::create(source, "file.cpp")};
}

/**
 * Encapsulates the whole process of setting up several editors, positioning the cursor,
 * executing Follow Symbol Under Cursor or Switch Between Function Declaration/Definition
 * and checking the result.
 */
class F2TestCase : public CppEditor::Tests::TestCase
{
public:
    enum CppEditorAction {
        FollowSymbolUnderCursorAction,
        SwitchBetweenMethodDeclarationDefinitionAction
    };

    F2TestCase(CppEditorAction action,
               const QList<TestDocumentPtr> &testFiles,
               OverrideItemList expectedVirtualFunctionProposal = OverrideItemList());

    ~F2TestCase()
    {
        ClangdSettings::setUseClangd(m_prevUseClangd);
    }

    static ProjectExplorer::Kit *m_testKit;
private:
    static TestDocumentPtr testFileWithInitialCursorMarker(const QList<TestDocumentPtr> &testFiles);
    static TestDocumentPtr testFileWithTargetCursorMarker(const QList<TestDocumentPtr> &testFiles);

    const bool m_prevUseClangd;
};

ProjectExplorer::Kit *F2TestCase::m_testKit = nullptr;

/// Creates a test case with multiple test files.
/// Exactly one test document must be provided that contains '@', the initial position marker.
/// Exactly one test document must be provided that contains '$', the target position marker.
/// It can be the same document.
F2TestCase::F2TestCase(CppEditorAction action,
                       const QList<TestDocumentPtr> &testFiles,
                       OverrideItemList expectedVirtualFunctionProposal)
    : m_prevUseClangd(ClangdSettings::instance().useClangd())
{
    QVERIFY(succeededSoFar());

    if (m_testKit)
        ClangdSettings::setUseClangd(true);

    // Check if there are initial and target position markers
    TestDocumentPtr initialTestFile = testFileWithInitialCursorMarker(testFiles);
    QVERIFY2(initialTestFile,
        "No test file with initial cursor marker is provided.");
    TestDocumentPtr targetTestFile = testFileWithTargetCursorMarker(testFiles);
    QVERIFY2(targetTestFile,
        "No test file with target cursor marker is provided.");

    const QString curTestName = QLatin1String(QTest::currentTestFunction());
    const QString tag = QLatin1String(QTest::currentDataTag());
    const bool useClangd = m_testKit;
    if (useClangd) {
        if (tag.contains("before keyword") || tag.contains("in keyword")
            || tag.contains("before parenthesis")) {
            QSKIP("clangd correctly goes to definition of SIGNAL macro");
        }
        if (curTestName == "testFollowClassOperator" && tag == "backward")
            QSKIP("clangd goes to operator name first");
        if (tag == "baseClassFunctionIntroducedByUsingDeclaration")
            QSKIP("clangd points to the using declaration");
        if (curTestName == "testFollowClassOperatorInOp")
            QSKIP("clangd goes to operator name first");
    }

    // Write files to disk
    CppEditor::Tests::TemporaryDir temporaryDir;
    QVERIFY(temporaryDir.isValid());
    QString projectFileContent = "QtApplication { files: [";
   for (TestDocumentPtr testFile : testFiles) {
        QVERIFY(testFile->baseDirectory().isEmpty());
        testFile->setBaseDirectory(temporaryDir.path());
        QVERIFY(testFile->writeToDisk());
        projectFileContent += QString::fromLatin1("\"%1\",")
                .arg(testFile->filePath().toFSPathString());
    }
    projectFileContent += "]}\n";

    class ProjectCloser {
    public:
        void setProject(Project *p) { m_p = p; }
        ~ProjectCloser() { if (m_p) ProjectExplorerPlugin::unloadProject(m_p); }
    private:
        Project * m_p = nullptr;
    } projectCloser;

    if (useClangd) {
        CppTestDocument projectFile("project.qbs", projectFileContent.toUtf8());
        projectFile.setBaseDirectory(temporaryDir.path());
        QVERIFY(projectFile.writeToDisk());
        const auto openProjectResult = ProjectExplorerPlugin::openProject(projectFile.filePath());
        QVERIFY2(openProjectResult && openProjectResult.project(),
                 qPrintable(openProjectResult.errorMessage()));
        projectCloser.setProject(openProjectResult.project());
        openProjectResult.project()->configureAsExampleProject(m_testKit);

        // Wait until project is fully indexed.
        QVERIFY(CppEditor::Tests::waitForSignalOrTimeout(openProjectResult.project(),
                &Project::indexingFinished, CppEditor::Tests::clangdIndexingTimeout()));
    }

    // Update Code Model
    QSet<FilePath> filePaths;
   for (const TestDocumentPtr &testFile : testFiles)
        filePaths << testFile->filePath();
    QVERIFY(parseFiles(filePaths));

    // Open Files
   for (TestDocumentPtr testFile : testFiles) {
        QVERIFY(openCppEditor(testFile->filePath(), &testFile->m_editor,
                              &testFile->m_editorWidget));
        if (!useClangd) // Editors get closed when unloading project.
            closeEditorAtEndOfTestCase(testFile->m_editor);

        // Wait until the indexer processed the just opened file.
        // The file is "Full Checked" since it is in the working copy now,
        // that is the function bodies are processed.
        forever {
            const Document::Ptr document = waitForFileInGlobalSnapshot(testFile->filePath());
            QVERIFY(document);
            if (document->checkMode() == Document::FullCheck) {
                if (!document->diagnosticMessages().isEmpty())
                    qDebug() << document->diagnosticMessages().first().text();
                QVERIFY(document->diagnosticMessages().isEmpty());
                break;
            }
        }

        // Rehighlight
        if (!useClangd)
            QVERIFY(waitForRehighlightedSemanticDocument(testFile->m_editorWidget));
    }

    // Activate editor of initial test file
    EditorManager::activateEditor(initialTestFile->m_editor);

    initialTestFile->m_editor->setCursorPosition(initialTestFile->m_cursorPosition);
//    qDebug() << "Initial line:" << initialTestFile->editor->currentLine();
//    qDebug() << "Initial column:" << initialTestFile->editor->currentColumn();

    OverrideItemList immediateVirtualSymbolResults;
    OverrideItemList finalVirtualSymbolResults;

    // Trigger the action
    switch (action) {
    case FollowSymbolUnderCursorAction: {
        CppEditorWidget *widget = initialTestFile->m_editorWidget;
        if (useClangd) {
            widget->enableTestMode();
            widget->openLinkUnderCursor();
            break;
        }

        FollowSymbolUnderCursor *builtinFollowSymbol = &CppModelManager::builtinFollowSymbol();
        QSharedPointer<VirtualFunctionAssistProvider> original
                = builtinFollowSymbol->virtualFunctionAssistProvider();

        // Set test provider, run and get results
        QSharedPointer<VirtualFunctionTestAssistProvider> testProvider(
            new VirtualFunctionTestAssistProvider(widget));
        builtinFollowSymbol->setVirtualFunctionAssistProvider(testProvider);
        widget->openLinkUnderCursor();
        immediateVirtualSymbolResults = testProvider->m_immediateItems;
        finalVirtualSymbolResults = testProvider->m_finalItems;

        // Restore original test provider
        builtinFollowSymbol->setVirtualFunctionAssistProvider(original);
        break;
    }
    case SwitchBetweenMethodDeclarationDefinitionAction:
        // Some test cases were erroneously added as decl/def, but they are really
        // follow symbol functionality (in commit a0764603d0).
        if (useClangd && tag.endsWith("Var"))
            initialTestFile->m_editorWidget->openLinkUnderCursor();
        else
            initialTestFile->m_editorWidget->switchDeclarationDefinition(/*inNextSplit*/false);
        break;
    default:
        QFAIL("Unknown test action");
        break;
    }

    if (useClangd) {
        if (expectedVirtualFunctionProposal.size() <= 1) {
            QVERIFY(CppEditor::Tests::waitForSignalOrTimeout(EditorManager::instance(),
                                                             &EditorManager::linkOpened, 10000));
        } else {
            QTimer t;
            QEventLoop l;
            t.setSingleShot(true);
            QObject::connect(&t, &QTimer::timeout, &l, &QEventLoop::quit);
            const IAssistProposal *immediateProposal = nullptr;
            const IAssistProposal *finalProposal = nullptr;
            QObject::connect(initialTestFile->m_editorWidget, &CppEditorWidget::proposalsReady, &l,
                             [&](const IAssistProposal *i, const IAssistProposal *f) {
                immediateProposal = i;
                finalProposal = f;
                l.quit();
            });
            t.start(10000);
            l.exec();
            QVERIFY(immediateProposal);
            QVERIFY(finalProposal);
            immediateVirtualSymbolResults = VirtualFunctionTestAssistProvider::itemList(
                        immediateProposal->model());
            finalVirtualSymbolResults = VirtualFunctionTestAssistProvider::itemList(
                        finalProposal->model());
        }
    } else {
        QCoreApplication::processEvents();
    }

    // Compare
    IEditor *currentEditor = EditorManager::currentEditor();
    BaseTextEditor *currentTextEditor = dynamic_cast<BaseTextEditor*>(currentEditor);
    QVERIFY(currentTextEditor);

    if (useClangd) {
        QEXPECT_FAIL("matchFunctionSignatureFuzzy1Forward", "clangd returns decl loc", Abort);
        QEXPECT_FAIL("matchFunctionSignatureFuzzy2Forward", "clangd returns decl loc", Abort);
        QEXPECT_FAIL("matchFunctionSignatureFuzzy1Backward", "clangd returns def loc", Abort);
        QEXPECT_FAIL("matchFunctionSignatureFuzzy2Backward", "clangd returns def loc", Abort);
    }
    QCOMPARE(currentTextEditor->document()->filePath(), targetTestFile->filePath());
    int expectedLine, expectedColumn;
    if (useClangd && expectedVirtualFunctionProposal.size() == 1) {
        expectedLine = expectedVirtualFunctionProposal.first().line;
        expectedColumn = -1;
        expectedVirtualFunctionProposal.clear();
    } else {
        currentTextEditor->convertPosition(targetTestFile->m_targetCursorPosition,
                                           &expectedLine, &expectedColumn);
        ++expectedColumn;
        if (useClangd && (tag == "classDestructor" || tag == "fromDestructorDefinitionSymbol"
                || tag == "fromDestructorBody")) {
            --expectedColumn; // clangd goes before the ~, built-in code model after
        }
    }
//    qDebug() << "Expected line:" << expectedLine;
//    qDebug() << "Expected column:" << expectedColumn;

    if (useClangd) {
        QEXPECT_FAIL("matchFunctionSignature_Follow_8_fuzzy",
                     "clangd points to declaration", Abort);
        QEXPECT_FAIL("matchFunctionSignature_Follow_9_fuzzy",
                     "clangd points to declaration", Abort);
    } else {
        QEXPECT_FAIL("globalVarFromEnum", "Contributor works on a fix.", Abort);
        QEXPECT_FAIL("matchFunctionSignature_Follow_5", "foo(int) resolved as CallAST", Abort);
        if (tag.contains("SLOT") && tag.contains("no 2nd QObject"))
            QEXPECT_FAIL("", "FIXME", Abort);
    }

    QCOMPARE(currentTextEditor->currentLine(), expectedLine);
    if (expectedColumn != -1)
        QCOMPARE(currentTextEditor->currentColumn(), expectedColumn);

//    qDebug() << immediateVirtualSymbolResults;
//    qDebug() << finalVirtualSymbolResults;
    OverrideItemList expectedImmediate;
    if (!expectedVirtualFunctionProposal.isEmpty()) {
        bool offerBaseDecl = true;
        if (useClangd) {
            if (tag == "allOverrides from base declaration") {
                expectedVirtualFunctionProposal.removeFirst();
                offerBaseDecl = false;
            }
        }
        if (offerBaseDecl) {
            OverrideItem first = expectedVirtualFunctionProposal.first();
            if (useClangd)
                first.text = "<base declaration>";
            expectedImmediate << first;
        }
        expectedImmediate << OverrideItem(QLatin1String("collecting overrides..."));
    }
    QCOMPARE(immediateVirtualSymbolResults, expectedImmediate);
    if (useClangd) {
        QEXPECT_FAIL("noSiblings_references", "FIXME: too many results", Abort);
        QEXPECT_FAIL("noSiblings_pointers", "FIXME: too many results", Abort);
        QEXPECT_FAIL("noSiblings_noBaseExpression", "FIXME: too many results", Abort);
    }
    QCOMPARE(finalVirtualSymbolResults.size(), expectedVirtualFunctionProposal.size());
    QCOMPARE(finalVirtualSymbolResults, expectedVirtualFunctionProposal);
}

TestDocumentPtr F2TestCase::testFileWithInitialCursorMarker(const QList<TestDocumentPtr> &testFiles)
{
   for (const TestDocumentPtr &testFile : testFiles) {
        if (testFile->hasCursorMarker())
            return testFile;
    }
    return TestDocumentPtr();
}

TestDocumentPtr F2TestCase::testFileWithTargetCursorMarker(const QList<TestDocumentPtr> &testFiles)
{
   for (const TestDocumentPtr &testFile : testFiles) {
        if (testFile->hasTargetCursorMarker())
            return testFile;
    }
    return TestDocumentPtr();
}

} // namespace CppEditor::Internal::Tests

Q_DECLARE_METATYPE(QList<CppEditor::Internal::Tests::TestDocumentPtr>)

namespace CppEditor::Internal::Tests {

void FollowSymbolTest::initTestCase()
{
    const QString clangdFromEnv = Utils::qtcEnvironmentVariable("QTC_CLANGD");
    if (clangdFromEnv.isEmpty())
        return;
    ClangdSettings::setClangdFilePath(Utils::FilePath::fromUserInput(clangdFromEnv));
    const auto clangd = ClangdSettings::instance().clangdFilePath();
    if (clangd.isEmpty() || !clangd.exists())
        return;

    // Find suitable kit.
    // Qt is not actually required for the tests, but we need it for consistency with
    // configureAsExampleProject().
    // FIXME: Make configureAsExampleProject() work with non-Qt kits.
    F2TestCase::m_testKit = Utils::findOr(KitManager::kits(), nullptr, [](const Kit *k) {
        return k->isValid() && !k->hasWarning() && k->value("QtSupport.QtInformation").isValid();
    });
    if (!F2TestCase::m_testKit)
        QSKIP("This test requires at least one kit to be present");
}

void FollowSymbolTest::testSwitchMethodDeclDef_data()
{
    QTest::addColumn<QByteArray>("header");
    QTest::addColumn<QByteArray>("source");

    QTest::newRow("fromFunctionDeclarationSymbol") << _(
        "class C\n"
        "{\n"
        "public:\n"
        "    C();\n"
        "    int @function();\n"  // Line 5
        "};\n"
        ) << _(
        "#include \"file.h\"\n"
        "\n"
        "C::C()\n"
        "{\n"
        "}\n"                   // Line 5
        "\n"
        "int C::$function()\n"
        "{\n"
        "    return 1 + 1;\n"
        "}\n"                   // Line 10
    );

    QTest::newRow("fromFunctionDefinitionSymbol") << _(
        "class C\n"
        "{\n"
        "public:\n"
        "    C();\n"
        "    int $function();\n"
        "};\n"
        ) << _(
        "#include \"file.h\"\n"
        "\n"
        "C::C()\n"
        "{\n"
        "}\n"
        "\n"
        "int C::@function()\n"
        "{\n"
        "    return 1 + 1;\n"
        "}\n"
    );

    QTest::newRow("fromFunctionBody") << _(
        "class C\n"
        "{\n"
        "public:\n"
        "    C();\n"
        "    int $function();\n"
        "};\n"
        ) << _(
        "#include \"file.h\"\n"
        "\n"
        "C::C()\n"
        "{\n"
        "}\n"                   // Line 5
        "\n"
        "int C::function()\n"
        "{\n"
        "    return @1 + 1;\n"
        "}\n"                   // Line 10
    );

    QTest::newRow("fromConstructorDeclarationSymbol") << _(
        "class C\n"
        "{\n"
        "public:\n"
        "    C();\n"
        "    int @function();\n"  // Line 5
        "};\n"
        ) << _(
        "#include \"file.h\"\n"
        "\n"
        "C::C()\n"
        "{\n"
        "}\n"                   // Line 5
        "\n"
        "int C::$function()\n"
        "{\n"
        "    return 1 + 1;\n"
        "}\n"                   // Line 10
        );

    QTest::newRow("fromConstructorDefinitionSymbol") << _(
        "class C\n"
        "{\n"
        "public:\n"
        "    $C();\n"
        "    int function();\n"
        "};\n"
        ) << _(
        "#include \"file.h\"\n"
        "\n"
        "C::@C()\n"
        "{\n"
        "}\n"
        "\n"
        "int C::function()\n"
        "{\n"
        "    return 1 + 1;\n"
        "}\n"
        );

    QTest::newRow("fromConstructorBody") << _(
        "class C\n"
        "{\n"
        "public:\n"
        "    $C();\n"
        "    int function();\n"
        "};\n"
        ) << _(
        "#include \"file.h\"\n"
        "\n"
        "C::C()\n"
        "{@\n"
        "}\n"                   // Line 5
        "\n"
        "int C::function()\n"
        "{\n"
        "    return 1 + 1;\n"
        "}\n"                   // Line 10
        );

    QTest::newRow("fromDestructorDeclarationSymbol") << _(
        "class C\n"
        "{\n"
        "public:\n"
        "    @C();\n"
        "    int function();\n"  // Line 5
        "};\n"
        ) << _(
        "#include \"file.h\"\n"
        "\n"
        "C::$C()\n"
        "{\n"
        "}\n"                   // Line 5
        "\n"
        "int C::function()\n"
        "{\n"
        "    return 1 + 1;\n"
        "}\n"                   // Line 10
        );

    QTest::newRow("fromDestructorDefinitionSymbol") << _(
        "class C\n"
        "{\n"
        "public:\n"
        "    ~$C();\n"
        "    int function();\n"
        "};\n"
        ) << _(
        "#include \"file.h\"\n"
        "\n"
        "C::@~C()\n"
        "{\n"
        "}\n"
        "\n"
        "int C::function()\n"
        "{\n"
        "    return 1 + 1;\n"
        "}\n"
        );

    QTest::newRow("fromDestructorBody") << _(
        "class C\n"
        "{\n"
        "public:\n"
        "    ~$C();\n"
        "    int function();\n"
        "};\n"
        ) << _(
        "#include \"file.h\"\n"
        "\n"
        "C::~C()\n"
        "{@\n"
        "}\n"                   // Line 5
        "\n"
        "int C::function()\n"
        "{\n"
        "    return 1 + 1;\n"
        "}\n"                   // Line 10
        );

    QTest::newRow("fromReturnType") << _(
        "class C\n"
        "{\n"
        "public:\n"
        "    C();\n"
        "    int $function();\n"
        "};\n"
        ) << _(
        "#include \"file.h\"\n"
        "\n"
        "C::C()\n"
        "{\n"
        "}\n"                   // Line 5
        "\n"
        "@int C::function()\n"
        "{\n"
        "    return 1 + 1;\n"
        "}\n"                   // Line 10
    );

    QTest::newRow("matchFunctionSignature_Def_1") << _(
        "class Foo {\n"
        "    void @foo(int);\n"
        "    void foo();\n"
        "};\n"
        ) << _(
        "#include \"file.h\"\n"
        "void Foo::$foo(int) {}\n"
    );

    QTest::newRow("matchFunctionSignature_Def_2") << _(
        "class Foo {\n"
        "    void $foo(int);\n"
        "    void foo();\n"
        "};\n"
        ) << _(
        "#include \"file.h\"\n"
        "void Foo::@foo(int) {}\n"
    );

    QTest::newRow("matchFunctionSignature_Def_3") << _(
        "class Foo {\n"
        "    void foo(int);\n"
        "    void @$foo() {}\n"
        "};\n"
        ) << _(
        "#include \"file.h\"\n"
        "void Foo::foo(int) {}\n"
    );

    QTest::newRow("matchFunctionSignature_Def_4") << _(
        "class Foo {\n"
        "    void foo(int);\n"
        "    void @$foo();\n"
        "};\n"
    ) << _();

    QTest::newRow("matchFunctionSignature_Def_5") << _(
        "class Foo {\n"
        "    void @$foo(int);\n"
        "    void foo();\n"
        "};\n"
    ) << _();

    QTest::newRow("unicodeIdentifier") << _(
        "class Foo { void $" TEST_UNICODE_IDENTIFIER "(); };\n"
        "void Foo::@" TEST_UNICODE_IDENTIFIER "() {}\n"
    ) << _();

    QTest::newRow("globalVar")
            << _("namespace NS { extern int @globalVar; }\n")
            << _("int globalVar;\n"
                 "namespace NS {\n"
                 "extern int globalVar;\n"
                 "int $globalVar;\n"
                 "}\n");

    QTest::newRow("staticMemberVar")
            << _("namespace NS {\n"
                 "class Test {\n"
                 "    static int @var;\n"
                 "};\n"
                 "class OtherClass { static int var; };\n"
                 "}\n"
                 "class OtherClass { static int var; };\n")
            << _("#include \"file.h\"\n"
                 "int var;\n"
                 "int OtherClass::var;\n"
                 "namespace NS {\n"
                 "int OtherClass::var;\n"
                 "int Test::$var;\n"
                 "}\n");

    QTest::newRow("conversionOperatorDecl2Def")
            << _("struct Foo {\n"
                 "    operator @int() const;\n"
                 "};\n")
            << _("#include \"file.h\"\n"
                 "Foo::$operator int() const { return {}; }\n");
    QTest::newRow("conversionOperatorDef2Decl")
            << _("struct Foo {\n"
                 "    $operator int() const;\n"
                 "};\n")
            << _("#include \"file.h\"\n"
                 "Foo::@operator int() const { return {}; }\n");
}

void FollowSymbolTest::testSwitchMethodDeclDef()
{
    QFETCH(QByteArray, header);
    QFETCH(QByteArray, source);

    const QList<TestDocumentPtr> testFiles = QList<TestDocumentPtr>()
        << CppTestDocument::create(header, "file.h")
        << CppTestDocument::create(source, "file.cpp");

    F2TestCase(F2TestCase::SwitchBetweenMethodDeclarationDefinitionAction, testFiles);
}

void FollowSymbolTest::testFollowSymbol_data()
{
    QTest::addColumn<QByteArray>("source");

    /// Check ...
    QTest::newRow("globalVarFromFunction") << _(
        "int $j;\n"
        "int main()\n"
        "{\n"
        "    @j = 2;\n"
        "}\n"           // Line 5
    );

    // 3.3.10 Name hiding (par 3.), from text
    QTest::newRow("funLocalVarHidesClassMember") << _(
        "struct C {\n"
        "    void f()\n"
        "    {\n"
        "        int $member; // hides C::member\n"
        "        ++@member;\n"                       // Line 5
        "    }\n"
        "    int member;\n"
        "};\n"
    );

    // 3.3.10 Name hiding (par 4.), from text
    QTest::newRow("funLocalVarHidesNamespaceMemberIntroducedByUsingDirective") << _(
        "namespace N {\n"
        "    int i;\n"
        "}\n"
        "\n"
        "int main()\n"                       // Line 5
        "{\n"
        "    using namespace N;\n"
        "    int $i;\n"
        "    ++i@; // refers to local i;\n"
        "}\n"                                // Line 10
    );

    // 3.3.3 Block scope (par. 4), from text
    // Same for if, while, switch
    QTest::newRow("loopLocalVarHidesOuterScopeVariable1") << _(
        "int main()\n"
        "{\n"
        "    int i = 1;\n"
        "    for (int $i = 0; i < 10; ++i) { // 'i' refers to for's i\n"
        "        i = @i; // same\n"                                       // Line 5
        "    }\n"
        "}\n"
    );

    // 3.3.3 Block scope (par. 4), from text
    // Same for if, while, switch
    QTest::newRow("loopLocalVarHidesOuterScopeVariable2") << _(
        "int main()\n"
        "{\n"
        "    int i = 1;\n"
        "    for (int $i = 0; @i < 10; ++i) { // 'i' refers to for's i\n"
        "        i = i; // same\n"                                         // Line 5
        "    }\n"
        "}\n"
    );

    // 3.3.7 Class scope, part of the example
    QTest::newRow("subsequentDefinedClassMember") << _(
        "class X {\n"
        "    int f() { return @i; } // i refers to class's i\n"
        "    int $i;\n"
        "};\n"
    );

    // 3.3.7 Class scope, part of the example
    // Variable name hides type name.
    QTest::newRow("classMemberHidesOuterTypeDef") << _(
        "typedef int c;\n"
        "class X {\n"
        "    int f() { return @c; } // c refers to class' c\n"
        "    int $c; // hides typedef name\n"
        "};\n"                                                 // Line 5
    );

    // 3.3.2 Point of declaration (par. 1), copy-paste
    QTest::newRow("globalVarFromEnum") << _(
        "const int $x = 12;\n"
        "int main()\n"
        "{\n"
        "    enum { x = @x }; // x refers to global x\n"
        "}\n"                                             // Line 5
    );

    // 3.3.2 Point of declaration
    QTest::newRow("selfInitialization") << _(
        "int x = 12;\n"
        "int main()\n"
        "{\n"
        "   int $x = @x; // Second x refers to local x\n"
        "}\n"                                              // Line 5
    );

    // 3.3.2 Point of declaration (par. 3), from text
    QTest::newRow("pointerToClassInClassDefinition") << _(
        "class $Foo {\n"
        "    @Foo *p; // Refers to above Foo\n"
        "};\n"
    );

    // 3.3.2 Point of declaration (par. 5), copy-paste
    QTest::newRow("previouslyDefinedMemberFromArrayDefinition") << _(
        "struct X {\n"
        "    enum E { $z = 16 };\n"
        "    int b[X::@z]; // z refers to defined z\n"
        "};\n"
    );

    // 3.3.7 Class scope (par. 2), from text
    QTest::newRow("outerStaticMemberVariableFromInsideSubclass") << _(
        "struct C\n"
        "{\n"
        "   struct I\n"
        "   {\n"
        "       void f()\n"                            // Line 5
        "       {\n"
        "           int i = @c; // refers to C's c\n"
        "       }\n"
        "   };\n"
        "\n"                                           // Line 10
        "   static int $c;\n"
        "};\n"
    );

    // 3.3.7 Class scope (par. 1), part of point 5
    QTest::newRow("memberVariableFollowingDotOperator") << _(
        "struct C\n"
        "{\n"
        "    int $member;\n"
        "};\n"
        "\n"                 // Line 5
        "int main()\n"
        "{\n"
        "    C c;\n"
        "    c.@member++;\n"
        "}\n"                // Line 10
    );

    // 3.3.7 Class scope (par. 1), part of point 5
    QTest::newRow("memberVariableFollowingArrowOperator") << _(
        "struct C\n"
        "{\n"
        "    int $member;\n"
        "};\n"
        "\n"                    // Line 5
        "int main()\n"
        "{\n"
        "    C* c;\n"
        "    c->@member++;\n"
        "}\n"                   // Line 10
    );

    // 3.3.7 Class scope (par. 1), part of point 5
    QTest::newRow("staticMemberVariableFollowingScopeOperator") << _(
        "struct C\n"
        "{\n"
        "    static int $member;\n"
        "};\n"
        "\n"                        // Line 5
        "int main()\n"
        "{\n"
        "    C::@member++;\n"
        "}\n"
    );

    // 3.3.7 Class scope (par. 2), from text
    QTest::newRow("staticMemberVariableFollowingDotOperator") << _(
        "struct C\n"
        "{\n"
        "    static int $member;\n"
        "};\n"
        "\n"                        // Line 5
        "int main()\n"
        "{\n"
        "    C c;\n"
        "    c.@member;\n"
        "}\n"                       // Line 10
    );


    // 3.3.7 Class scope (par. 2), from text
    QTest::newRow("staticMemberVariableFollowingArrowOperator") << _(
        "struct C\n"
        "{\n"
        "    static int $member;\n"
        "};\n"
        "\n"                         // Line 5
        "int main()\n"
        "{\n"
        "    C *c;\n"
        "    c->@member++;\n"
        "}\n"                        // Line 10
    );

    // 3.3.8 Enumeration scope
    QTest::newRow("previouslyDefinedEnumValueFromInsideEnum") << _(
        "enum {\n"
        "    $i = 0,\n"
        "    j = @i // refers to i above\n"
        "};\n"
    );

    // 3.3.8 Enumeration scope
    QTest::newRow("nsMemberHidesNsMemberIntroducedByUsingDirective") << _(
        "namespace A {\n"
        "    char x;\n"
        "}\n"
        "\n"
        "namespace B {\n"                               // Line 5
        "    using namespace A;\n"
        "    int $x; // hides A::x\n"
        "}\n"
        "\n"
        "int main()\n"                                  // Line 10
        "{\n"
        "    B::@x++; // refers to B's X, not A::x\n"
        "}\n"
    );

    // 3.3.10 Name hiding, from text
    // www.stroustrup.com/bs_faq2.html#overloadderived
    QTest::newRow("baseClassFunctionIntroducedByUsingDeclaration") << _(
        "struct B {\n"
        "    int $f(int) {}\n"
        "};\n"
        "\n"
        "class D : public B {\n"                              // Line 5
        "public:\n"
        "    using B::f; // make every f from B available\n"
        "    double f(double) {}\n"
        "};\n"
        "\n"                                                  // Line 10
        "int main()\n"
        "{\n"
        "    D* pd = new D;\n"
        "    pd->@f(2); // refers to B::f\n"
        "    pd->f(2.3); // refers to D::f\n"                 // Line 15
        "}\n"
    );

    // 3.3.10 Name hiding, from text
    // www.stroustrup.com/bs_faq2.html#overloadderived
    QTest::newRow("funWithSameNameAsBaseClassFunIntroducedByUsingDeclaration") << _(
        "struct B {\n"
        "    int f(int) {}\n"
        "};\n"
        "\n"
        "class D : public B {\n"                              // Line 5
        "public:\n"
        "    using B::f; // make every f from B available\n"
        "    double $f(double) {}\n"
        "};\n"
        "\n"                                                  // Line 10
        "int main()\n"
        "{\n"
        "    D* pd = new D;\n"
        "    pd->f(2); // refers to B::f\n"
        "    pd->@f(2.3); // refers to D::f\n"                // Line 15
        "}\n"
    );

    // 3.3.10 Name hiding (par 2.), from text
    // A class name (9.1) or enumeration name (7.2) can be hidden by the name of a variable,
    // data member, function, or enumerator declared in the same scope.
    QTest::newRow("funLocalVarHidesOuterClass") << _(
        "struct C {};\n"
        "\n"
        "int main()\n"
        "{\n"
        "    int $C; // hides type C\n"  // Line 5
        "    ++@C;\n"
        "}\n"
    );

    QTest::newRow("classConstructor") << _(
        "class Foo {\n"
        "    F@oo();"
        "    ~Foo();"
        "};\n\n"
        "Foo::$Foo()\n"
        "{\n"
        "}\n\n"
        "Foo::~Foo()\n"
        "{\n"
        "}\n"
    );

    QTest::newRow("classDestructor") << _(
        "class Foo {\n"
        "    Foo();"
        "    ~@Foo();"
        "};\n\n"
        "Foo::Foo()\n"
        "{\n"
        "}\n\n"
        "Foo::~$Foo()\n"
        "{\n"
        "}\n"
    );

    QTest::newRow("skipForwardDeclarationBasic") << _(
        "class $Foo {};\n"
        "class Foo;\n"
        "@Foo foo;\n"
    );

    QTest::newRow("skipForwardDeclarationTemplates") << _(
        "template <class E> class $Container {};\n"
        "template <class E> class Container;\n"
        "@Container<int> container;\n"
    );

    QTest::newRow("matchFunctionSignature_Follow_1") << _(
        "class Foo {\n"
        "    void @foo(int);\n"
        "    void foo();\n"
        "};\n"
        "void Foo::$foo(int) {}\n"
    );

    QTest::newRow("matchFunctionSignature_Follow_2") << _(
        "class Foo {\n"
        "    void $foo(int);\n"
        "    void foo();\n"
        "};\n"
        "void Foo::@foo(int) {}\n"
    );

    QTest::newRow("matchFunctionSignature_Follow_3") << _(
        "class Foo {\n"
        "    void foo(int);\n"
        "    void @$foo() {}\n"
        "};\n"
        "void Foo::foo(int) {}\n"
    );

    QTest::newRow("matchFunctionSignature_Follow_3.5") << _(
        "void foo(int);\n"
        "void @$foo() {}\n"
    );

    QTest::newRow("matchFunctionSignature_Follow_3.6") << _(
        "void foo(int);\n"
        "void @$foo(double) {}\n"
    );

    QTest::newRow("matchFunctionSignature_Follow_4") << _(
        "class Foo {\n"
        "    void foo(int);\n"
        "    void @$foo();\n"
        "};\n"
    );

    QTest::newRow("matchFunctionSignature_Follow_5") << _(
        "class Foo {\n"
        "    void @$foo(int);\n"
        "    void foo();\n"
        "};\n"
    );

    QTest::newRow("matchFunctionSignature_Follow_6") << _(
        "class Foo {\n"
        "    void $foo(int);\n"
        "};\n"
        "void Foo::@foo(const volatile int) {}\n"
    );

    QTest::newRow("matchFunctionSignature_Follow_7") << _(
        "class Foo {\n"
        "    void $foo(const volatile int);\n"
        "};\n"
        "void Foo::@foo(int) {}\n"
    );

    QTest::newRow("matchFunctionSignature_Follow_8_fuzzy") << _(
        "class Foo {\n"
        "    void @foo(int *);\n"
        "};\n"
        "void Foo::$foo(const int *) {}\n"
    );

    QTest::newRow("matchFunctionSignature_Follow_8_exact") << _(
        "class Foo {\n"
        "    void @foo(int *);\n"
        "};\n"
        "void Foo::foo(const int *) {}\n"
        "void Foo::$foo(int *) {}\n"
    );

    QTest::newRow("matchFunctionSignature_Follow_9_fuzzy") << _(
        "class Foo {\n"
        "    void @foo(int&);\n"
        "};\n"
        "void Foo::$foo(const int&) {}\n"
    );

    QTest::newRow("matchFunctionSignature_Follow_9_exact") << _(
        "class Foo {\n"
        "    void @foo(int&);\n"
        "};\n"
        "void Foo::$foo(int&) {}\n"
        "void Foo::foo(const int&) {}\n"
    );

    QTest::newRow("infiniteLoopLocalTypedef_QTCREATORBUG-11999") << _(
        "template<class MyTree>\n"
        "class TreeConstIterator\n"
        "{\n"
        "    typedef TreeConstIterator<MyTree> MyIter;\n"
        "    void f() { return this->@$g(); }\n"
        "};\n"
        "\n"
        "void h() { typedef TreeConstIterator<MyBase> const_iterator; }\n"
    );

    QTest::newRow("unicodeIdentifier") << _(
        "class Foo { void $" TEST_UNICODE_IDENTIFIER "(); };\n"
        "void Foo::@" TEST_UNICODE_IDENTIFIER "() {}\n"
    );

    QTest::newRow("trailingReturnType") << _(
        "struct $Foo {};\n"
        "auto foo() -> @Foo {}\n"
    );

    QTest::newRow("template_alias") << _(
        "template<class T>"
        "class Bar;"
        "template<class $T>\n"
        "using Foo = Bar<@T>;\n"
    );

    QTest::newRow("shadowed overload with default args") << _(
        "struct Parent { void disconnect(int n = 0); };\n"
        "struct Child : public Parent { void $disconnect(); };\n"
        "void test() { Child c; c.@disconnect(); }\n"
    );
}

void FollowSymbolTest::testFollowSymbol()
{
    QFETCH(QByteArray, source);
    F2TestCase(F2TestCase::FollowSymbolUnderCursorAction, singleDocument(source));
}

void FollowSymbolTest::testFollowSymbolQTCREATORBUG7903_data()
{
    QTest::addColumn<QByteArray>("source");
    QTest::newRow("using_QTCREATORBUG7903_globalNamespace") << _(
        "namespace NS {\n"
        "class Foo {};\n"
        "}\n"
        "using NS::$Foo;\n"
        "void fun()\n"
        "{\n"
        "    @Foo foo;\n"
        "}\n"
    );

    QTest::newRow("using_QTCREATORBUG7903_namespace") << _(
        "namespace NS {\n"
        "class Foo {};\n"
        "}\n"
        "namespace NS1 {\n"
        "void fun()\n"
        "{\n"
        "    using NS::$Foo;\n"
        "    @Foo foo;\n"
        "}\n"
        "}\n"
    );

    QTest::newRow("using_QTCREATORBUG7903_insideFunction") << _(
        "namespace NS {\n"
        "class Foo {};\n"
        "}\n"
        "void fun()\n"
        "{\n"
        "    using NS::$Foo;\n"
        "    @Foo foo;\n"
        "}\n"
    );
}

void FollowSymbolTest::testFollowSymbolQTCREATORBUG7903()
{
    QFETCH(QByteArray, source);
    F2TestCase(F2TestCase::FollowSymbolUnderCursorAction, singleDocument(source));
}

void FollowSymbolTest::testFollowCall_data()
{
    QTest::addColumn<QByteArray>("variableDeclaration"); // without semicolon, can be ""
    QTest::addColumn<QByteArray>("callArgument");
    QTest::addColumn<QByteArray>("expectedSignature"); // you might need to add a function
                                                       // declaration with such a signature

    QTest::newRow("intLiteral-to-int")
            << _("")
            << _("5")
            << _("int");
    QTest::newRow("charLiteral-to-const-char-ptr")
            << _("")
            << _("\"hoo\"")
            << _("const char *");
    QTest::newRow("charLiteral-to-int")
            << _("")
            << _("'a'")
            << _("char");

    QTest::newRow("charPtr-to-constCharPtr")
            << _("char *var = \"var\"")
            << _("var")
            << _("const char *");
    QTest::newRow("charPtr-to-constCharPtr")
            << _("char *var = \"var\"")
            << _("var")
            << _("const char *");
    QTest::newRow("constCharPtr-to-constCharPtr")
            << _("const char *var = \"var\"")
            << _("var")
            << _("const char *");

    QTest::newRow("Bar-to-constBarRef")
            << _("Bar var")
            << _("var")
            << _("const Bar &");
}

void FollowSymbolTest::testFollowCall()
{
    QFETCH(QByteArray, variableDeclaration);
    QFETCH(QByteArray, callArgument);
    QFETCH(QByteArray, expectedSignature);

    const QByteArray templateSource =
        "class Bar {};\n"
        "void fun(int);\n"
        "void fun(const char *);\n"
        "void fun(const Bar &);\n"
        "void fun(char);\n"
        "void fun(double);\n"
        "\n"
        "void t()\n"
        "{\n"
        "   " + variableDeclaration + ";\n"
        "   @fun(" + callArgument + ");\n"
        "}\n";

    const QByteArray matchText = " fun(" + expectedSignature + ")";
    const QByteArray replaceText = " $fun(" + expectedSignature + ")";
    QByteArray source = templateSource;
    source.replace(matchText, replaceText);
    QVERIFY(source != templateSource);
    F2TestCase(F2TestCase::FollowSymbolUnderCursorAction, singleDocument(source));
}

void FollowSymbolTest::testFollowSymbolMultipleDocuments_data()
{
    QTest::addColumn<QList<TestDocumentPtr> >("documents");

    QTest::newRow("skipForwardDeclarationBasic") << (QList<TestDocumentPtr>()
        << CppTestDocument::create("class $Foo {};\n",
                                "defined.h")
        << CppTestDocument::create("class Foo;\n"
                                "@Foo foo;\n",
                                "forwardDeclaredAndUsed.h")
    );

    QTest::newRow("skipForwardDeclarationTemplates") << (QList<TestDocumentPtr>()
        << CppTestDocument::create("template <class E> class $Container {};\n",
                                "defined.h")
        << CppTestDocument::create("template <class E> class Container;\n"
                                "@Container<int> container;\n",
                                "forwardDeclaredAndUsed.h")
    );

    QTest::newRow("matchFunctionSignature") << (QList<TestDocumentPtr>()
        << CppTestDocument::create("class Foo {\n"
                                "    void @foo(int);\n"
                                "    void foo() {}\n"
                                "};\n",
                                "foo.h")
        << CppTestDocument::create("#include \"foo.h\"\n"
                                "void Foo::$foo(int) {}\n",
                                "foo.cpp")
    );

    QTest::newRow("matchFunctionSignature2") << (QList<TestDocumentPtr>()
        << CppTestDocument::create("namespace N { class C; }\n"
                                "bool *@fun(N::C *) const;\n",
                                "foo.h")
        << CppTestDocument::create("#include \"foo.h\"\n"
                                "using namespace N;\n"
                                "bool *$fun(C *) const {}\n",
                                "foo.cpp")
    );

    QTest::newRow("matchFunctionSignatureFuzzy1Forward") << (QList<TestDocumentPtr>()
        << CppTestDocument::create("class Foo {\n"
                                "    void @foo(int);\n"
                                "    void foo();\n"
                                "};\n",
                                "foo.h")
        << CppTestDocument::create("#include \"foo.h\"\n"
                                "void Foo::$foo() {}\n",
                                "foo.cpp")
    );

    QTest::newRow("matchFunctionSignatureFuzzy1Backward") << (QList<TestDocumentPtr>()
        << CppTestDocument::create("class Foo {\n"
                                "    void $foo(int);\n"
                                "};\n",
                                "foo.h")
        << CppTestDocument::create("#include \"foo.h\"\n"
                                "void Foo::@foo() {}\n",
                                "foo.cpp")
    );

    QTest::newRow("matchFunctionSignatureFuzzy2Forward") << (QList<TestDocumentPtr>()
        << CppTestDocument::create("class Foo {\n"
                                "    void foo(int);\n"
                                "    void @foo();\n"
                                "};\n",
                                "foo.h")
        << CppTestDocument::create("#include \"foo.h\"\n"
                                "void Foo::$foo(int) {}\n",
                                "foo.cpp")
    );

    QTest::newRow("matchFunctionSignatureFuzzy2Backward") << (QList<TestDocumentPtr>()
        << CppTestDocument::create("class Foo {\n"
                                "    void $foo();\n"
                                "};\n",
                                "foo.h")
        << CppTestDocument::create("#include \"foo.h\"\n"
                                "void Foo::@foo(int) {}\n",
                                "foo.cpp")
    );

    QTest::newRow("globalVar") << QList<TestDocumentPtr>{
            CppTestDocument::create("namespace NS { extern int @globalVar; }\n", "file.h"),
            CppTestDocument::create(
                "int globalVar;\n"
                "namespace NS {\n"
                "extern int globalVar;\n"
                "int $globalVar;\n"
                "}\n", "file.cpp")};

    QTest::newRow("staticMemberVar") << QList<TestDocumentPtr>{
        CppTestDocument::create(
            "namespace NS {\n"
            "class Test {\n"
            "    static int @var;\n"
            "};\n"
            "class OtherClass { static int var; };\n"
            "}\n"
            "class OtherClass { static int var; };\n", "file.h"),
        CppTestDocument::create(
            "#include \"file.h\"\n"
            "int var;\n"
            "int OtherClass::var;\n"
            "namespace NS {\n"
            "int OtherClass::var;\n"
            "int Test::$var;\n"
            "}\n", "file.cpp")};
}

void FollowSymbolTest::testFollowSymbolMultipleDocuments()
{
    QFETCH(QList<TestDocumentPtr>, documents);
    F2TestCase(F2TestCase::FollowSymbolUnderCursorAction, documents);
}

void FollowSymbolTest::testFollowSymbolQObjectConnect_data()
{
#define TAG(str) secondQObjectParam ? str : str ", no 2nd QObject"
    QTest::addColumn<char>("start");
    QTest::addColumn<char>("target");
    QTest::addColumn<bool>("secondQObjectParam");
    for (int i = 0; i < 2; ++i) {
        bool secondQObjectParam = (i == 0);
        QTest::newRow(TAG("SIGNAL: before keyword"))
                << '1' << '1' << secondQObjectParam;
        QTest::newRow(TAG("SIGNAL: in keyword"))
                << '2' << '1' << secondQObjectParam;
        QTest::newRow(TAG("SIGNAL: before parenthesis"))
                << '3' << '1' << secondQObjectParam;
        QTest::newRow(TAG("SIGNAL: before identifier"))
                << '4' << '1' << secondQObjectParam;
        QTest::newRow(TAG("SIGNAL: in identifier"))
                << '5' << '1' << secondQObjectParam;
        QTest::newRow(TAG("SIGNAL: before identifier parenthesis"))
                << '6' << '1' << secondQObjectParam;
        QTest::newRow(TAG("SLOT: before keyword"))
                << '7' << '2' << secondQObjectParam;
        QTest::newRow(TAG("SLOT: in keyword"))
                << '8' << '2' << secondQObjectParam;
        QTest::newRow(TAG("SLOT: before parenthesis"))
                << '9' << '2' << secondQObjectParam;
        QTest::newRow(TAG("SLOT: before identifier"))
                << 'A' << '2' << secondQObjectParam;
        QTest::newRow(TAG("SLOT: in identifier"))
                << 'B' << '2' << secondQObjectParam;
        QTest::newRow(TAG("SLOT: before identifier parenthesis"))
                << 'C' << '2' << secondQObjectParam;
    }
#undef TAG
}

static void selectMarker(QByteArray *source, char marker, char number)
{
    int idx = 0;
    forever {
        idx = source->indexOf(marker, idx);
        if (idx == -1)
            break;
        if (source->at(idx + 1) == number) {
            ++idx;
            source->remove(idx, 1);
        } else {
            source->remove(idx, 2);
        }
    }
}

void FollowSymbolTest::testFollowSymbolQObjectConnect()
{
    QFETCH(char, start);
    QFETCH(char, target);
    QFETCH(bool, secondQObjectParam);
    QByteArray source =
            "#define QT_STRINGIFY2(x) #x\n"
            "#define QT_STRINGIFY(x) QT_STRINGIFY2(x)\n"
            "#define QLOCATION \"\\0\" __FILE__ \":\" QT_STRINGIFY(__LINE__)\n"
            "const char *qFlagLocation(const char *) { return 0; }\n"
            "#define SLOT(a) qFlagLocation(\"1\"#a QLOCATION)\n"
            "#define SIGNAL(a) qFlagLocation(\"2\"#a QLOCATION)\n"
            "\n"
            "#define slots\n"
            "#define signals public\n"
            "\n"
            "class QObject {};\n"
            "void connect(QObject *, const char *, QObject *, const char *) {}\n"
            "\n"
            "\n"
            "class Foo : public QObject\n"
            "{\n"
            "signals:\n"
            "    void $1endOfWorld();\n"
            "public slots:\n"
            "    void $2onWorldEnded()\n"
            "    {\n"
            "    }\n"
            "};\n"
            "\n"
            "void bla()\n"
            "{\n"
            "   Foo foo;\n"
            "   connect(&foo, @1SI@2GNAL@3(@4end@5OfWorld@6()),\n"
            "           &foo, @7SL@8OT@9(@Aon@BWorldEnded@C()));\n"
            "}\n";

    selectMarker(&source, '@', start);
    selectMarker(&source, '$', target);

    if (!secondQObjectParam)
        source.replace(" &foo, ", QByteArray());

    F2TestCase(F2TestCase::FollowSymbolUnderCursorAction, singleDocument(source));
}

void FollowSymbolTest::testFollowSymbolQObjectOldStyleConnect()
{
    const QByteArray source =
            "class O : public QObject {\n"
            "    Q_OBJECT\n"
            "signals:\n"
            "    void $theSignal();\n"
            "};\n"
            "struct S {\n"
            "    O* o();\n"
            "};\n"
            "int main()\n"
            "{\n"
            "    S s;\n"
            "    QObject::connect(s.o(), SIGNAL(@theSignal()), s.o(), SIGNAL(theSignal()));\n"
            "}\n";

    F2TestCase(F2TestCase::FollowSymbolUnderCursorAction, singleDocument(source));
}

void FollowSymbolTest::testFollowClassOperatorOnOperatorToken_data()
{
    QTest::addColumn<bool>("toDeclaration");
    QTest::newRow("forward") << false;
    QTest::newRow("backward") << true;
}

void FollowSymbolTest::testFollowClassOperatorOnOperatorToken()
{
    QFETCH(bool, toDeclaration);

    QByteArray source =
            "class Foo {\n"
            "    void @operator[](size_t idx);\n"
            "};\n\n"
            "void Foo::$operator[](size_t idx)\n"
            "{\n"
            "}\n";
    if (toDeclaration)
        source.replace('@', '#').replace('$', '@').replace('#', '$');
    F2TestCase(F2TestCase::FollowSymbolUnderCursorAction, singleDocument(source));
}

void FollowSymbolTest::testFollowClassOperator_data()
{
    testFollowClassOperatorOnOperatorToken_data();
}

void FollowSymbolTest::testFollowClassOperator()
{
    QFETCH(bool, toDeclaration);

    QByteArray source =
            "class Foo {\n"
            "    void $2operator@1[](size_t idx);\n"
            "};\n\n"
            "void Foo::$1operator@2[](size_t idx)\n"
            "{\n"
            "}\n";
    if (toDeclaration)
        source.replace("@1", QByteArray()).replace("$1", QByteArray())
                .replace("@2", "@").replace("$2", "$");
    else
        source.replace("@2", QByteArray()).replace("$2", QByteArray())
                .replace("@1", "@").replace("$1", "$");
    F2TestCase(F2TestCase::FollowSymbolUnderCursorAction, singleDocument(source));
}

void FollowSymbolTest::testFollowClassOperatorInOp_data()
{
    testFollowClassOperatorOnOperatorToken_data();
}

void FollowSymbolTest::testFollowClassOperatorInOp()
{
    QFETCH(bool, toDeclaration);

    QByteArray source =
            "class Foo {\n"
            "    void $2operator[@1](size_t idx);\n"
            "};\n\n"
            "void Foo::$1operator[@2](size_t idx)\n"
            "{\n"
            "}\n";
    if (toDeclaration)
        source.replace("@1", QByteArray()).replace("$1", QByteArray())
                .replace("@2", "@").replace("$2", "$");
    else
        source.replace("@2", QByteArray()).replace("$2", QByteArray())
                .replace("@1", "@").replace("$1", "$");
    F2TestCase(F2TestCase::FollowSymbolUnderCursorAction, singleDocument(source));
}

void FollowSymbolTest::testFollowVirtualFunctionCall_data()
{
    QTest::addColumn<QByteArray>("source");
    QTest::addColumn<OverrideItemList>("results");

    /// Check: Static type is base class pointer, all overrides are presented.
    QTest::newRow("allOverrides") << _(
            "struct A { virtual void virt() = 0; };\n"
            "void A::virt() {}\n"
            "\n"
            "struct B : A { void virt(); };\n"
            "void B::virt() {}\n"
            "\n"
            "struct C : B { void virt(); };\n"
            "void C::virt() {}\n"
            "\n"
            "struct CD1 : C { void virt(); };\n"
            "void CD1::virt() {}\n"
            "\n"
            "struct CD2 : C { void virt(); };\n"
            "void CD2::virt() {}\n"
            "\n"
            "int f(A *o) { o->$@virt(); }\n"
            "\n")
        << (OverrideItemList()
            << OverrideItem(QLatin1String("A::virt = 0"), 2)
            << OverrideItem(QLatin1String("B::virt"), 5)
            << OverrideItem(QLatin1String("C::virt"), 8)
            << OverrideItem(QLatin1String("CD1::virt"), 11)
            << OverrideItem(QLatin1String("CD2::virt"), 14));

    /// Check: Cursor on unimplemented base declaration.
    QTest::newRow("allOverrides from base declaration") << _(
            "struct A { virtual void $@virt() = 0; };\n"
            "\n"
            "struct B : A { void virt(); };\n"
            "void B::virt() {}\n"
            "\n"
            "struct C : B { void virt(); };\n"
            "void C::virt() {}\n"
            "\n"
            "struct CD1 : C { void virt(); };\n"
            "void CD1::virt() {}\n"
            "\n"
            "struct CD2 : C { void virt(); };\n"
            "void CD2::virt() {}\n"
            "\n"
            "int f(A *o) { o->virt(); }\n"
            "\n")
        << (OverrideItemList()
            << OverrideItem(QLatin1String("A::virt = 0"), 1)
            << OverrideItem(QLatin1String("B::virt"), 4)
            << OverrideItem(QLatin1String("C::virt"), 7)
            << OverrideItem(QLatin1String("CD1::virt"), 10)
            << OverrideItem(QLatin1String("CD2::virt"), 13));

    /// Check: Static type is derived class pointer, only overrides of sub classes are presented.
    QTest::newRow("possibleOverrides1") << _(
            "struct A { virtual void virt() = 0; };\n"
            "void A::virt() {}\n"
            "\n"
            "struct B : A { void virt(); };\n"
            "void B::virt() {}\n"
            "\n"
            "struct C : B { void virt(); };\n"
            "void C::virt() {}\n"
            "\n"
            "struct CD1 : C { void virt(); };\n"
            "void CD1::virt() {}\n"
            "\n"
            "struct CD2 : C { void virt(); };\n"
            "void CD2::virt() {}\n"
            "\n"
            "int f(B *o) { o->$@virt(); }\n"
            "\n")
        << (OverrideItemList()
            << OverrideItem(QLatin1String("B::virt"), 5)
            << OverrideItem(QLatin1String("C::virt"), 8)
            << OverrideItem(QLatin1String("CD1::virt"), 11)
            << OverrideItem(QLatin1String("CD2::virt"), 14));

    /// Check: Virtual function call in member of class hierarchy,
    ///        only possible overrides are presented.
    QTest::newRow("possibleOverrides2") << _(
            "struct A { virtual void virt(); };\n"
            "void A::virt() {}\n"
            "\n"
            "struct B : public A { void virt(); };\n"
            "void B::virt() {}\n"
            "\n"
            "struct C : public B { void g() { virt$@(); } }; \n"
            "\n"
            "struct D : public C { void virt(); };\n"
            "void D::virt() {}\n")
        << (OverrideItemList()
            << OverrideItem(QLatin1String("B::virt"), 5)
            << OverrideItem(QLatin1String("D::virt"), 10));

    /// Check: If no definition is found, fallback to the declaration.
    QTest::newRow("fallbackToDeclaration") << _(
            "struct A { virtual void virt(); };\n"
            "\n"
            "int f(A *o) { o->$@virt(); }\n")
        << (OverrideItemList()
            << OverrideItem(QLatin1String("A::virt"), 1));

    /// Check: Ensure that the first entry in the final results is the same as the first in the
    ///        immediate results.
    QTest::newRow("itemOrder") << _(
            "struct C { virtual void virt() = 0; };\n"
            "void C::virt() {}\n"
            "\n"
            "struct B : C { void virt(); };\n"
            "void B::virt() {}\n"
            "\n"
            "struct A : B { void virt(); };\n"
            "void A::virt() {}\n"
            "\n"
            "int f(C *o) { o->$@virt(); }\n")
        << (OverrideItemList()
            << OverrideItem(QLatin1String("C::virt = 0"), 2)
            << OverrideItem(QLatin1String("A::virt"), 8)
            << OverrideItem(QLatin1String("B::virt"), 5));

    /// Check: If class templates are involved, the class and function symbols might be generated.
    ///        In that case, make sure that the symbols are not deleted before we reference them.
    QTest::newRow("instantiatedSymbols") << _(
            "template <class T> struct A { virtual void virt() {} };\n"
            "void f(A<int> *l) { l->$@virt(); }\n")
        << (OverrideItemList()
            << OverrideItem(QLatin1String("A::virt"), 1));

    /// Check: Static type is nicely resolved, especially for QSharedPointers.
    QTest::newRow("QSharedPointer") << _(
            "template <class T>\n"
            "class Basic\n"
            "{\n"
            "public:\n"
            "    inline T &operator*() const;\n"
            "    inline T *operator->() const;\n"
            "};\n"
            "\n"
            "template <class T> class ExternalRefCount: public Basic<T> {};\n"
            "template <class T> class QSharedPointer: public ExternalRefCount<T> {};\n"
            "\n"
            "struct A { virtual void virt() {} };\n"
            "struct B : public A { void virt() {} };\n"
            "\n"
            "int f()\n"
            "{\n"
            "    QSharedPointer<A> p(new A);\n"
            "    p->$@virt();\n"
            "}\n")
        << (OverrideItemList()
            << OverrideItem(QLatin1String("A::virt"), 12)
            << OverrideItem(QLatin1String("B::virt"), 13));

    /// Check: In case there is no override for the static type of a function call expression,
    ///        make sure to:
    ///         1) include the last provided override (look up bases)
    ///         2) and all overrides whose classes are derived from that static type
    QTest::newRow("noSiblings_references") << _(
            "struct A { virtual void virt(); };\n"
            "struct B : A { void virt() {} };\n"
            "struct C1 : B { void virt() {} };\n"
            "struct C2 : B { };\n"
            "struct D : C2 { void virt() {} };\n"
            "\n"
            "void f(C2 &o) { o.$@virt(); }\n")
        << (OverrideItemList()
            << OverrideItem(QLatin1String("B::virt"), 2)
            << OverrideItem(QLatin1String("D::virt"), 5));

    /// Variation of noSiblings_references
    QTest::newRow("noSiblings_pointers") << _(
            "struct A { virtual void virt(); };\n"
            "struct B : A { void virt() {} };\n"
            "struct C1 : B { void virt() {} };\n"
            "struct C2 : B { };\n"
            "struct D : C2 { void virt() {} };\n"
            "\n"
            "void f(C2 *o) { o->$@virt(); }\n")
        << (OverrideItemList()
            << OverrideItem(QLatin1String("B::virt"), 2)
            << OverrideItem(QLatin1String("D::virt"), 5));

    /// Variation of noSiblings_references
    QTest::newRow("noSiblings_noBaseExpression") << _(
            "struct A { virtual void virt() {} };\n"
            "struct B : A { void virt() {} };\n"
            "struct C1 : B { void virt() {} };\n"
            "struct C2 : B { void g() { $@virt(); } };\n"
            "struct D : C2 { void virt() {} };\n")
        << (OverrideItemList()
            << OverrideItem(QLatin1String("B::virt"), 2)
            << OverrideItem(QLatin1String("D::virt"), 5));

    /// Check: Trigger on a.virt() if a is of type &A.
    QTest::newRow("onDotMemberAccessOfReferenceTypes") << _(
            "struct A { virtual void virt() = 0; };\n"
            "void A::virt() {}\n"
            "\n"
            "void client(A &o) { o.$@virt(); }\n")
        << (OverrideItemList()
            << OverrideItem(QLatin1String("A::virt = 0"), 2));

    /// Check: Do not trigger on a.virt() if a is of type A.
    QTest::newRow("notOnDotMemberAccessOfNonReferenceType") << _(
            "struct A { virtual void virt(); };\n"
            "void A::$virt() {}\n"
            "\n"
            "void client(A o) { o.@virt(); }\n")
        << OverrideItemList();

    /// Check: Do not trigger on qualified function calls.
    QTest::newRow("notOnQualified") << _(
            "struct A { virtual void virt(); };\n"
            "void A::$virt() {}\n"
            "\n"
            "struct B : public A {\n"
            "    void virt();\n"
            "    void g() { A::@virt(); }\n"
            "};\n")
        << OverrideItemList();

    /// Check: Do not trigger on member function declaration.
    QTest::newRow("notOnDeclaration") << _(
            "struct A { virtual void virt(); };\n"
            "void A::virt() {}\n"
            "\n"
            "struct B : public A { void virt@(); };\n"
            "void B::$virt() {}\n")
        << OverrideItemList();

    /// Check: Do not trigger on function definition.
    QTest::newRow("notOnDefinition") << _(
            "struct A { virtual void virt(); };\n"
            "void A::virt() {}\n"
            "\n"
            "struct B : public A { void $virt(); };\n"
            "void B::@virt() {}\n")
        << OverrideItemList();

    QTest::newRow("notOnNonPointerNonReference") << _(
            "struct A { virtual void virt(); };\n"
            "void A::virt() {}\n"
            "\n"
            "struct B : public A { void virt(); };\n"
            "void B::$virt() {}\n"
            "\n"
            "void client(B b) { b.@virt(); }\n")
        << OverrideItemList();

    QTest::newRow("differentReturnTypes") << _(
            "struct Base { virtual Base *virt() { return this; } };\n"
            "struct Derived : public Base { Derived *virt() { return this; } };\n"
            "void client(Base *b) { b->$@virt(); }\n")
        << (OverrideItemList()
            << OverrideItem(QLatin1String("Base::virt"), 1)
            << OverrideItem(QLatin1String("Derived::virt"), 2));

    QTest::newRow("QTCREATORBUG-10294_cursorIsAtTheEndOfVirtualFunctionName") << _(
            "struct Base { virtual void virt() {} };\n"
            "struct Derived : Base { void virt() {} };\n"
            "void client(Base *b) { b->virt$@(); }\n")
        << (OverrideItemList()
            << OverrideItem(QLatin1String("Base::virt"), 1)
            << OverrideItem(QLatin1String("Derived::virt"), 2));

    QTest::newRow("static_call") << _(
            "struct Base { virtual void virt() {} };\n"
            "struct Derived : Base { void virt() {} };\n"
            "struct Foo {\n"
            "    static Base *base();\n"
            "};\n"
            "void client() { Foo::base()->$@virt(); }\n")
        << (OverrideItemList()
            << OverrideItem(QLatin1String("Base::virt"), 1)
            << OverrideItem(QLatin1String("Derived::virt"), 2));

    QTest::newRow("double_call") << _(
            "struct Base { virtual void virt() {} };\n"
            "struct Derived : Base { void virt() {} };\n"
            "struct Foo { Base *base(); };\n"
            "Foo *instance();\n"
            "void client() { instance()->base()->$@virt(); }\n")
        << (OverrideItemList()
            << OverrideItem(QLatin1String("Base::virt"), 1)
            << OverrideItem(QLatin1String("Derived::virt"), 2));

    QTest::newRow("casting") << _(
            "struct Base { virtual void virt() {} };\n"
            "struct Derived : Base { void virt() {} };\n"
            "void client() { static_cast<Base *>(0)->$@virt(); }\n")
        << (OverrideItemList()
            << OverrideItem(QLatin1String("Base::virt"), 1)
            << OverrideItem(QLatin1String("Derived::virt"), 2));
}

void FollowSymbolTest::testFollowVirtualFunctionCall()
{
    QFETCH(QByteArray, source);
    QFETCH(OverrideItemList, results);

    F2TestCase(F2TestCase::FollowSymbolUnderCursorAction, singleDocument(source), results);
}

/// Check: Base classes can be found although these might be defined in distinct documents.
void FollowSymbolTest::testFollowVirtualFunctionCallMultipleDocuments()
{
    QList<TestDocumentPtr> testFiles = QList<TestDocumentPtr>()
            << CppTestDocument::create("struct A { virtual void virt(int) = 0; };\n",
                                    "a.h")
            << CppTestDocument::create("#include \"a.h\"\n"
                                    "struct B : A { void virt(int) {} };\n",
                                    "b.h")
            << CppTestDocument::create("#include \"a.h\"\n"
                                    "void f(A *o) { o->$@virt(42); }\n",
                                    "u.cpp")
            ;

    const OverrideItemList finalResults = OverrideItemList()
            << OverrideItem(QLatin1String("A::virt = 0"), 1)
            << OverrideItem(QLatin1String("B::virt"), 2);

    F2TestCase(F2TestCase::FollowSymbolUnderCursorAction, testFiles, finalResults);
}

} // namespace CppEditor::Internal::Tests

/*
Potential test cases improving name lookup.

If you fix one, add a test and remove the example from here.

///////////////////////////////////////////////////////////////////////////////////////////////////

// 3.3.1 Declarative regions and scopes, copy-paste (main added)
int j = 24;
int main()
{
    int i = j, j; // First j refers to global j, second j refers to just locally defined j
    j = 42; // Refers to locally defined j
}

///////////////////////////////////////////////////////////////////////////////////////////////////

// 3.3.2 Point of declaration (par. 2), copy-paste (main added)
const int i = 2;
int main()
{
    int i[i]; // Second i refers to global
}

///////////////////////////////////////////////////////////////////////////////////////////////////

// 3.3.2 Point of declaration (par. 9), copy-paste (main added)
typedef unsigned char T;
template<class T
= T // lookup finds the typedef name of unsigned char
, T // lookup finds the template parameter
N = 0> struct A { };

int main() {}

///////////////////////////////////////////////////////////////////////////////////////////////////

// 3.3.9 Template parameter scope (par. 3), copy-paste (main added), part 1
template<class T, T* p, class U = T> class X {}; // second and third T refers to first one
template<class T> void f(T* p = new T);

int main() {}

///////////////////////////////////////////////////////////////////////////////////////////////////

// 3.3.9 Template parameter scope (par. 3), copy-paste (main added), part 2
template<class T> class X : public Array<T> {};
template<class T> class Y : public T {};

int main() {}

///////////////////////////////////////////////////////////////////////////////////////////////////

// 3.3.9 Template parameter scope (par. 4), copy-paste (main added), part 2
typedef int N;
template<N X, typename N, template<N Y> class T> struct A; // N refers to N above

int main() {}

///////////////////////////////////////////////////////////////////////////////////////////////////

// 3.3.10 Name hiding (par 1.), from text, example 2a

// www.stroustrup.com/bs_faq2.html#overloadderived
// "In C++, there is no overloading across scopes - derived class scopes are not
// an exception to this general rule. (See D&E or TC++PL3 for details)."
struct B {
    int f(int) {}
};

struct D : public B {
    double f(double) {} // hides B::f
};

int main()
{
    D* pd = new D;
    pd->f(2); // refers to D::f
    pd->f(2.3); // refers to D::f
}

///////////////////////////////////////////////////////////////////////////////////////////////////

// 3.3.10 Name hiding (par 2.), from text
int C; // hides following type C, order is not important
struct C {};

int main()
{
    ++C;
}

*/
