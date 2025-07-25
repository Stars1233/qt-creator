// Copyright (C) Filippo Cucchetto <filippocucchetto@gmail.com>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "nimoutputtaskparser.h"

#include <QRegularExpression>

using namespace ProjectExplorer;
using namespace Utils;

namespace Nim {

NimParser::Result NimParser::handleLine(const QString &lne, OutputFormat)
{
    const QString line = lne.trimmed();
    static const QRegularExpression regex("(.+.nim)\\((\\d+), (\\d+)\\) (.+)");
    static const QRegularExpression warning("(Warning):(.*)");
    static const QRegularExpression error("(Error):(.*)");

    const QRegularExpressionMatch match = regex.match(line);
    if (!match.hasMatch())
        return Status::NotHandled;
    const QString filename = match.captured(1);
    bool lineOk = false;
    const int lineNumber = match.captured(2).toInt(&lineOk);
    const QString message = match.captured(4);
    if (!lineOk)
        return Status::NotHandled;

    Task::TaskType type = Task::Unknown;

    if (warning.match(message).hasMatch())
        type = Task::Warning;
    else if (error.match(message).hasMatch())
        type = Task::Error;
    else
        return Status::NotHandled;

    const CompileTask t(type, message, absoluteFilePath(FilePath::fromUserInput(filename)),
                        lineNumber);
    LinkSpecs linkSpecs;
    addLinkSpecForAbsoluteFilePath(linkSpecs, t.file(), t.line(), t.column(), match, 1);
    scheduleTask(t, 1);
    return {Status::Done, linkSpecs};
}

} // Nim

#ifdef WITH_TESTS

#include <projectexplorer/outputparser_test.h>

#include <QTest>

namespace Nim {

void NimParserTest::testNimParser_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<OutputParserTester::Channel>("inputChannel");
    QTest::addColumn<QStringList>("childStdOutLines");
    QTest::addColumn<QStringList>("childStdErrLines");
    QTest::addColumn<Tasks >("tasks");

    // negative tests
    QTest::newRow("pass-through stdout")
            << "Sometext" << OutputParserTester::STDOUT
            << QStringList("Sometext") << QStringList()
            << Tasks();
    QTest::newRow("pass-through stderr")
            << "Sometext" << OutputParserTester::STDERR
            << QStringList() << QStringList("Sometext")
            << Tasks();

    // positive tests
    QTest::newRow("Parse error string")
            << QString::fromLatin1("main.nim(23, 1) Error: undeclared identifier: 'x'")
            << OutputParserTester::STDERR
            << QStringList() << QStringList()
            << Tasks({CompileTask(Task::Error,
                                  "Error: undeclared identifier: 'x'",
                                  FilePath::fromUserInput("main.nim"), 23)});

    QTest::newRow("Parse warning string")
            << QString::fromLatin1("lib/pure/parseopt.nim(56, 34) Warning: quoteIfContainsWhite is deprecated [Deprecated]")
            << OutputParserTester::STDERR
            << QStringList() << QStringList()
            << Tasks({CompileTask(Task::Warning,
                                  "Warning: quoteIfContainsWhite is deprecated [Deprecated]",
                                   FilePath::fromUserInput("lib/pure/parseopt.nim"), 56)});
}

void NimParserTest::testNimParser()
{
    OutputParserTester testbench;
    testbench.addLineParser(new NimParser);
    QFETCH(QString, input);
    QFETCH(OutputParserTester::Channel, inputChannel);
    QFETCH(Tasks, tasks);
    QFETCH(QStringList, childStdOutLines);
    QFETCH(QStringList, childStdErrLines);

    testbench.testParsing(input, inputChannel, tasks, childStdOutLines, childStdErrLines);
}

} // Nim

#endif
